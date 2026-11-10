/*
 * DOS-XC C library - buffered streams
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#include "stdio_impl.h"
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static unsigned char stdin_buffer[BUFSIZ], stdout_buffer[BUFSIZ];
static FILE stdin_file = {.fd = 0, .flags = F_READ | F_STATIC | F_PROBE, .buffer = stdin_buffer, .capacity = BUFSIZ};
static FILE stdout_file = {.fd = 1, .flags = F_WRITE | F_STATIC | F_PROBE, .buffer = stdout_buffer, .capacity = BUFSIZ};
static FILE stderr_file = {.fd = 2, .flags = F_WRITE | F_STATIC | F_UNBUFFERED};
FILE *stdin = &stdin_file;
FILE *stdout = &stdout_file;
FILE *stderr = &stderr_file;
static FILE *open_files;
static struct __dosxc_rlock open_files_lock;

/* Terminals are line buffered; files and pipes fully buffered. */
static void probe(FILE *f) {
    if (!(f->flags & F_PROBE)) return;
    f->flags &= ~F_PROBE;
    if (dosxc_sys_isatty(f->fd) == 1) f->flags |= F_LINE;
}

/* ---- raw transfer: descriptors or memory ---- */
static ssize_t raw_write(FILE *f, const unsigned char *data, size_t size) {
    if (f->flags & F_MEMORY) {
        if (f->memory_position + size > f->memory_capacity) {
            if (!f->memory_out) {
                size = f->memory_capacity > f->memory_position ? f->memory_capacity - f->memory_position : 0;
            } else {
                size_t capacity = f->memory_capacity ? f->memory_capacity : 64;
                while (capacity < f->memory_position + size + 1) capacity *= 2;
                char *bigger = realloc(f->memory, capacity);
                if (!bigger) return -1;
                memset(bigger + f->memory_capacity, 0, capacity - f->memory_capacity);
                f->memory = bigger;
                f->memory_capacity = capacity - 1;
            }
        }
        memcpy(f->memory + f->memory_position, data, size);
        f->memory_position += size;
        if (f->memory_position > f->memory_length) {
            f->memory_length = f->memory_position;
            if (f->memory_out || f->memory_length < f->memory_capacity) f->memory[f->memory_length] = 0;
        }
        if (f->memory_out) {
            *f->memory_out = f->memory;
            *f->memory_size_out = f->memory_length;
        }
        return (ssize_t)size;
    }
    size_t done = 0;
    while (done < size) {
        ssize_t n = write(f->fd, data + done, size - done);
        if (n <= 0) return done ? (ssize_t)done : -1;
        done += (size_t)n;
    }
    return (ssize_t)done;
}

static ssize_t raw_read(FILE *f, unsigned char *data, size_t size) {
    if (f->flags & F_MEMORY) {
        size_t available = f->memory_length > f->memory_position ? f->memory_length - f->memory_position : 0;
        if (size > available) size = available;
        memcpy(data, f->memory + f->memory_position, size);
        f->memory_position += size;
        return (ssize_t)size;
    }
    return read(f->fd, data, size);
}

static off_t raw_seek(FILE *f, off_t offset, int whence) {
    if (f->flags & F_MEMORY) {
        off_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (off_t)f->memory_position : (off_t)f->memory_length;
        if (base + offset < 0 || (!f->memory_out && (size_t)(base + offset) > f->memory_capacity)) {
            errno = EINVAL;
            return -1;
        }
        f->memory_position = (size_t)(base + offset);
        return (off_t)f->memory_position;
    }
    return lseek(f->fd, offset, whence);
}

/* ---- buffer management ---- */
static int flush_write(FILE *f) {
    if (f->mode != MODE_WRITE || !f->used) return 0;
    ssize_t n = raw_write(f, f->buffer, f->used);
    if (n != (ssize_t)f->used) {
        f->flags |= F_ERROR;
        if (n > 0) {
            memmove(f->buffer, f->buffer + n, f->used - (size_t)n);
            f->used -= (size_t)n;
        }
        return EOF;
    }
    f->used = 0;
    return 0;
}

static void drop_read(FILE *f) {
    if (f->mode == MODE_READ && f->length > f->position && !(f->flags & F_MEMORY))
        lseek(f->fd, -(off_t)(f->length - f->position), SEEK_CUR);
    if (f->mode == MODE_READ && (f->flags & F_MEMORY)) f->memory_position -= f->length - f->position;
    f->position = f->length = 0;
}

int fflush(FILE *f) {
    if (!f) {
        int r = 0;
        if (fflush(stdout)) r = EOF;
        if (fflush(stderr)) r = EOF;
        LOCK_GUARD(&open_files_lock);
        for (FILE *p = open_files; p; p = p->next)
            if (fflush(p)) r = EOF;
        return r;
    }
    STREAM_GUARD(f);
    if (f->mode == MODE_WRITE) return flush_write(f);
    if (f->mode == MODE_READ) drop_read(f);
    f->mode = MODE_NONE;
    return 0;
}
int fflush_unlocked(FILE *f) { return fflush(f); }

hidden void __stdio_exit(void) { fflush(0); }

static int buffer_size(FILE *f, unsigned char **buffer) {
    if (f->flags & F_UNBUFFERED || !f->capacity) {
        *buffer = f->small;
        return sizeof(f->small);
    }
    *buffer = f->buffer;
    return (int)f->capacity;
}

static int to_read(FILE *f) {
    if (!(f->flags & F_READ)) {
        f->flags |= F_ERROR;
        errno = EBADF;
        return -1;
    }
    if (f->mode == MODE_WRITE && flush_write(f)) return -1;
    if (f->mode != MODE_READ) {
        f->mode = MODE_READ;
        f->position = f->length = 0;
    }
    return 0;
}

static int to_write(FILE *f) {
    if (!(f->flags & F_WRITE)) {
        f->flags |= F_ERROR;
        errno = EBADF;
        return -1;
    }
    if (f->mode == MODE_READ) drop_read(f);
    if (f->mode != MODE_WRITE) {
        f->mode = MODE_WRITE;
        f->used = 0;
        probe(f);
        if (f->flags & F_APPEND) raw_seek(f, 0, SEEK_END);
    }
    return 0;
}

/* Refill an empty read buffer; returns bytes available or 0 at EOF/error. */
static size_t refill(FILE *f) {
    if (f == stdin || (f->flags & F_LINE)) {
        probe(stdout);
        if (stdout->flags & F_LINE) fflush(stdout);
    }
    unsigned char *buffer;
    int capacity = buffer_size(f, &buffer);
    if (f->flags & F_UNBUFFERED) capacity = 1;
    ssize_t n = raw_read(f, buffer, (size_t)capacity);
    if (n <= 0) {
        f->flags |= n < 0 ? F_ERROR : F_EOF;
        f->position = f->length = 0;
        return 0;
    }
    f->position = 0;
    f->length = (size_t)n;
    return (size_t)n;
}

static unsigned char *read_buffer(FILE *f) {
    unsigned char *b;
    buffer_size(f, &b);
    return b;
}

int fgetc(FILE *f) {
    STREAM_GUARD(f);
    if (to_read(f)) return EOF;
    if (f->position == f->length && !refill(f)) return EOF;
    return read_buffer(f)[f->position++];
}
int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }
int fgetc_unlocked(FILE *f) { return fgetc(f); }
int getc_unlocked(FILE *f) { return fgetc(f); }
int getchar_unlocked(void) { return fgetc(stdin); }

int ungetc(int c, FILE *f) {
    STREAM_GUARD(f);
    if (c == EOF || to_read(f)) return EOF;
    unsigned char *b;
    int capacity = buffer_size(f, &b);
    if (f->position == 0) {
        if (f->length >= (size_t)capacity) return EOF;
        memmove(b + 1, b, f->length);
        ++f->length;
        ++f->position;
    }
    b[--f->position] = (unsigned char)c;
    f->flags &= ~F_EOF;
    return (unsigned char)c;
}

size_t fread(void *destination, size_t size, size_t count, FILE *f) {
    STREAM_GUARD(f);
    size_t total = size * count, done = 0;
    if (!total) return 0;
    if (to_read(f)) return 0;
    unsigned char *out = destination;
    while (done < total) {
        size_t available = f->length - f->position;
        if (available) {
            size_t n = total - done < available ? total - done : available;
            memcpy(out + done, read_buffer(f) + f->position, n);
            f->position += n;
            done += n;
            continue;
        }
        if (total - done >= f->capacity && !(f->flags & F_LINE) && f != stdin) {
            ssize_t n = raw_read(f, out + done, total - done);
            if (n <= 0) {
                f->flags |= n < 0 ? F_ERROR : F_EOF;
                break;
            }
            done += (size_t)n;
            continue;
        }
        if (!refill(f)) break;
    }
    return done / size;
}
size_t fread_unlocked(void *d, size_t s, size_t c, FILE *f) { return fread(d, s, c, f); }

char *fgets(char *s, int n, FILE *f) {
    STREAM_GUARD(f);
    if (n <= 0) return 0;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (!i) return 0;
    s[i] = 0;
    return s;
}
char *fgets_unlocked(char *s, int n, FILE *f) { return fgets(s, n, f); }

char *gets(char *s) {
    int i = 0, c;
    while ((c = fgetc(stdin)) != EOF && c != '\n') s[i++] = (char)c;
    if (c == EOF && !i) return 0;
    s[i] = 0;
    return s;
}

ssize_t getdelim(char **line, size_t *capacity, int delimiter, FILE *f) {
    STREAM_GUARD(f);
    size_t n = 0;
    if (!line || !capacity) {
        errno = EINVAL;
        return -1;
    }
    for (;;) {
        int c = fgetc(f);
        if (c == EOF) break;
        if (n + 2 > *capacity) {
            size_t bigger = *capacity ? *capacity * 2 : 128;
            char *p = realloc(*line, bigger);
            if (!p) {
                f->flags |= F_ERROR;
                return -1;
            }
            *line = p;
            *capacity = bigger;
        }
        (*line)[n++] = (char)c;
        if (c == delimiter) break;
    }
    if (!n) return -1;
    (*line)[n] = 0;
    return (ssize_t)n;
}
ssize_t getline(char **line, size_t *capacity, FILE *f) { return getdelim(line, capacity, '\n', f); }

int fputc(int c, FILE *f) {
    STREAM_GUARD(f);
    if (to_write(f)) return EOF;
    unsigned char byte = (unsigned char)c;
    if (f->flags & F_UNBUFFERED || !f->capacity) {
        if (raw_write(f, &byte, 1) != 1) {
            f->flags |= F_ERROR;
            return EOF;
        }
        return byte;
    }
    f->buffer[f->used++] = byte;
    if (f->used == f->capacity || ((f->flags & F_LINE) && byte == '\n'))
        if (flush_write(f)) return EOF;
    return byte;
}
int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }
int fputc_unlocked(int c, FILE *f) { return fputc(c, f); }
int putc_unlocked(int c, FILE *f) { return fputc(c, f); }
int putchar_unlocked(int c) { return fputc(c, stdout); }

size_t fwrite(const void *source, size_t size, size_t count, FILE *f) {
    STREAM_GUARD(f);
    size_t total = size * count;
    if (!total) return 0;
    if (to_write(f)) return 0;
    const unsigned char *data = source;
    if (f->flags & F_UNBUFFERED || !f->capacity || total >= f->capacity) {
        if (flush_write(f)) return 0;
        ssize_t n = raw_write(f, data, total);
        if (n < (ssize_t)total) {
            f->flags |= F_ERROR;
            return n > 0 ? (size_t)n / size : 0;
        }
        return count;
    }
    if (f->used + total > f->capacity && flush_write(f)) return 0;
    memcpy(f->buffer + f->used, data, total);
    f->used += total;
    if ((f->flags & F_LINE) && memchr(data, '\n', total) && flush_write(f)) return 0;
    return count;
}
size_t fwrite_unlocked(const void *s, size_t z, size_t c, FILE *f) { return fwrite(s, z, c, f); }

int fputs(const char *s, FILE *f) {
    size_t n = strlen(s);
    return fwrite(s, 1, n, f) == n || !n ? 0 : EOF;
}
int fputs_unlocked(const char *s, FILE *f) { return fputs(s, f); }
int puts(const char *s) { return fputs(s, stdout) == EOF || fputc('\n', stdout) == EOF ? EOF : 0; }
int getw(FILE *f) {
    int v;
    return fread(&v, sizeof(v), 1, f) == 1 ? v : EOF;
}
int putw(int v, FILE *f) { return fwrite(&v, sizeof(v), 1, f) == 1 ? 0 : EOF; }

/* ---- positioning ---- */
int fseeko(FILE *f, off_t offset, int whence) {
    STREAM_GUARD(f);
    if (f->mode == MODE_WRITE && flush_write(f)) return -1;
    if (whence == SEEK_CUR && f->mode == MODE_READ) offset -= (off_t)(f->length - f->position);
    f->position = f->length = 0;
    f->mode = MODE_NONE;
    if (raw_seek(f, offset, whence) < 0) return -1;
    f->flags &= ~F_EOF;
    return 0;
}
int fseek(FILE *f, long offset, int whence) { return fseeko(f, offset, whence); }

off_t ftello(FILE *f) {
    STREAM_GUARD(f);
    off_t position = raw_seek(f, 0, SEEK_CUR);
    if (position < 0) return -1;
    if (f->mode == MODE_READ) position -= (off_t)(f->length - f->position);
    if (f->mode == MODE_WRITE) position += (off_t)f->used;
    return position;
}
long ftell(FILE *f) { return (long)ftello(f); }
void rewind(FILE *f) {
    fseeko(f, 0, SEEK_SET);
    f->flags &= ~F_ERROR;
}
int fgetpos(FILE *f, fpos_t *p) {
    off_t at = ftello(f);
    if (at < 0) return -1;
    p->__position = at;
    return 0;
}
int fsetpos(FILE *f, const fpos_t *p) { return fseeko(f, p->__position, SEEK_SET); }

int feof(FILE *f) { return (f->flags & F_EOF) != 0; }
int ferror(FILE *f) { return (f->flags & F_ERROR) != 0; }
void clearerr(FILE *f) { f->flags &= ~(F_EOF | F_ERROR); }
int fileno(FILE *f) {
    if (f->flags & F_MEMORY) {
        errno = EBADF;
        return -1;
    }
    return f->fd;
}
int feof_unlocked(FILE *f) { return feof(f); }
int ferror_unlocked(FILE *f) { return ferror(f); }
void clearerr_unlocked(FILE *f) { clearerr(f); }
int fileno_unlocked(FILE *f) { return fileno(f); }
void flockfile(FILE *f) { if (__dosxc_threaded) __dosxc_rlock_acquire(&f->lock); }
int ftrylockfile(FILE *f) { flockfile(f); return 0; }
void funlockfile(FILE *f) { if (__dosxc_threaded) __dosxc_rlock_release(&f->lock); }

int setvbuf(FILE *f, char *buffer, int mode, size_t size) {
    STREAM_GUARD(f);
    if (f->mode != MODE_NONE && fflush(f)) return -1;
    f->flags &= ~(F_LINE | F_UNBUFFERED | F_PROBE);
    if (mode == _IONBF) {
        f->flags |= F_UNBUFFERED;
        return 0;
    }
    if (mode == _IOLBF) f->flags |= F_LINE;
    if (buffer && size) {
        if (f->flags & F_OWN_BUFFER) free(f->buffer);
        f->flags &= ~F_OWN_BUFFER;
        f->buffer = (unsigned char *)buffer;
        f->capacity = size;
    } else if (!f->capacity) {
        f->buffer = malloc(BUFSIZ);
        if (!f->buffer) {
            f->flags |= F_UNBUFFERED;
            return -1;
        }
        f->capacity = BUFSIZ;
        f->flags |= F_OWN_BUFFER;
    }
    return 0;
}
void setbuf(FILE *f, char *buffer) { setvbuf(f, buffer, buffer ? _IOFBF : _IONBF, BUFSIZ); }
void setbuffer(FILE *f, char *buffer, size_t size) { setvbuf(f, buffer, buffer ? _IOFBF : _IONBF, size); }
void setlinebuf(FILE *f) { setvbuf(f, 0, _IOLBF, 0); }

/* ---- opening and closing ---- */
static int parse_mode(const char *mode, int *flags) {
    int access;
    switch (*mode) {
    case 'r': access = O_RDONLY; *flags = F_READ; break;
    case 'w': access = O_WRONLY | O_CREAT | O_TRUNC; *flags = F_WRITE; break;
    case 'a': access = O_WRONLY | O_CREAT | O_APPEND; *flags = F_WRITE | F_APPEND; break;
    default: errno = EINVAL; return -1;
    }
    for (const char *m = mode + 1; *m; ++m) {
        if (*m == '+') {
            access = (access & ~O_ACCMODE) | O_RDWR;
            *flags |= F_READ | F_WRITE;
        } else if (*m == 'x') {
            access |= O_EXCL;
        } else if (*m == 'e') {
            access |= O_CLOEXEC;
        }
    }
    return access;
}

static FILE *new_stream(int fd, int flags) {
    FILE *f = calloc(1, sizeof(FILE));
    if (!f) return 0;
    f->fd = fd;
    f->flags = flags | F_PROBE | F_OWN_BUFFER;
    f->buffer = malloc(BUFSIZ);
    if (f->buffer) f->capacity = BUFSIZ;
    else f->flags |= F_UNBUFFERED;
    {
        LOCK_GUARD(&open_files_lock);
        f->next = open_files;
        open_files = f;
    }
    return f;
}

FILE *fopen(const char *path, const char *mode) {
    int flags, access = parse_mode(mode, &flags);
    if (access < 0) return 0;
    int fd = open(path, access, 0666);
    if (fd < 0) return 0;
    FILE *f = new_stream(fd, flags);
    if (!f) close(fd);
    return f;
}

FILE *fdopen(int fd, const char *mode) {
    int flags;
    if (parse_mode(mode, &flags) < 0) return 0;
    if (fcntl(fd, F_GETFL) < 0) return 0;
    return new_stream(fd, flags);
}

static void unlink_stream(FILE *f) {
    LOCK_GUARD(&open_files_lock);
    for (FILE **p = &open_files; *p; p = &(*p)->next)
        if (*p == f) {
            *p = f->next;
            return;
        }
}

int fclose(FILE *f) {
    int r = fflush(f);
    if (f->flags & F_MEMORY) {
        if (!f->memory_out && (f->flags & F_OWN_MEMORY)) free(f->memory);
    } else if (close(f->fd)) {
        r = EOF;
    }
    if (f->flags & F_STATIC) {
        f->flags = 0;
        return r;
    }
    unlink_stream(f);
    if (f->flags & F_OWN_BUFFER) free(f->buffer);
    free(f);
    return r;
}

FILE *freopen(const char *path, const char *mode, FILE *f) {
    int flags, access = parse_mode(mode, &flags);
    if (access < 0) return 0;
    fflush(f);
    if (!path) {
        f->flags = (f->flags & ~(F_READ | F_WRITE | F_APPEND | F_EOF | F_ERROR)) | flags;
        return f;
    }
    int fd = open(path, access, 0666);
    if (fd < 0) {
        fclose(f);
        return 0;
    }
    if (!(f->flags & F_MEMORY)) {
        if (fd != f->fd) {
            dup2(fd, f->fd);
            close(fd);
        }
    } else {
        f->fd = fd;
    }
    f->flags = (f->flags & (F_STATIC | F_OWN_BUFFER | F_UNBUFFERED)) | flags | F_PROBE;
    f->mode = MODE_NONE;
    f->position = f->length = f->used = 0;
    return f;
}

FILE *tmpfile(void) {
    const char *directory = getenv("TMPDIR");
    if (!directory || !*directory) directory = P_tmpdir;
    char name[PATH_MAX_STDIO];
    snprintf(name, sizeof(name), "%s/tmpXXXXXX", directory);
    int fd = mkstemp(name);
    if (fd < 0) return 0;
    unlink(name);             /* the open image lives on until fclose() */
    FILE *f = new_stream(fd, F_READ | F_WRITE);
    if (!f) close(fd);
    return f;
}

FILE *fmemopen(void *buffer, size_t size, const char *mode) {
    int flags;
    if (parse_mode(mode, &flags) < 0) return 0;
    FILE *f = calloc(1, sizeof(FILE));
    if (!f) return 0;
    f->fd = -1;
    f->flags = flags | F_MEMORY | F_UNBUFFERED;
    if (!buffer) {
        buffer = calloc(1, size + 1);
        if (!buffer) {
            free(f);
            return 0;
        }
        f->flags |= F_OWN_MEMORY;
    }
    f->memory = buffer;
    f->memory_capacity = size;
    if (*mode == 'r') f->memory_length = size;
    else if (*mode == 'a') f->memory_length = f->memory_position = strnlen(buffer, size);
    else if (size) f->memory[0] = 0;
    {
        LOCK_GUARD(&open_files_lock);
        f->next = open_files;
        open_files = f;
    }
    return f;
}

FILE *open_memstream(char **out, size_t *size) {
    FILE *f = calloc(1, sizeof(FILE));
    if (!f) return 0;
    f->memory = calloc(1, 64);
    if (!f->memory) {
        free(f);
        return 0;
    }
    f->fd = -1;
    f->flags = F_WRITE | F_MEMORY | F_UNBUFFERED;
    f->memory_capacity = 63;
    f->memory_out = out;
    f->memory_size_out = size;
    *out = f->memory;
    *size = 0;
    {
        LOCK_GUARD(&open_files_lock);
        f->next = open_files;
        open_files = f;
    }
    return f;
}

int remove(const char *path) {
    struct stat s;
    if (stat(path, &s)) return -1;
    return S_ISDIR(s.st_mode) ? rmdir(path) : unlink(path);
}

int rename(const char *from, const char *to) { return (int)__result(dosxc_sys_rename(from, to)); }

void perror(const char *prefix) {
    const char *message = strerror(errno);
    if (prefix && *prefix) fprintf(stderr, "%s: %s\n", prefix, message);
    else fprintf(stderr, "%s\n", message);
}
