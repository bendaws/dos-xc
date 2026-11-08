/*
 * DOS-XC /runtime/files.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * Files for 32/64-bit programs.  The FAT driver reads and writes whole
 * files, so an open file is an in-memory image ("inode") shared by every
 * descriptor that refers to the same path; it is written back when its last
 * reference closes (or on fsync).  Descriptors follow POSIX: dup() shares
 * the file offset, children inherit every descriptor not marked
 * close-on-exec, and pipes are unbounded buffers (a child always finishes
 * before its parent reads, so a pipe never needs to block).
 */

#include "runtime.h"
#include "net.h"

enum { KIND_FILE = 1, KIND_CONSOLE, KIND_NULL, KIND_PIPE_READ, KIND_PIPE_WRITE, KIND_DIRECTORY, KIND_SOCKET };
#define MAX_INODES 192
#define MAX_OPEN 384
#define MAX_PIPES 32

struct inode {
    int refs, dirty, unlinked;
    int loaded;                 /* data holds the file; otherwise reads go to disk */
    struct fat32_cursor cursor;
    char path[PATH_MAX_RT];
    u8 *data;
    uptr size, capacity;
    u8 attributes;
    u16 date, time;
};
struct pipe { int readers, writers; u8 *data; uptr head, tail, capacity; };
struct open_file {
    int refs, kind, flags;
    u64 offset;
    struct inode *inode;
    struct pipe *pipe;
    struct socket *socket;
    char path[PATH_MAX_RT];
};

static struct inode inodes[MAX_INODES];
static struct pipe pipes[MAX_PIPES];
static struct open_file open_files[MAX_OPEN];
static u8 empty_buffer[4];

#define O_ACCMODE DOSXC_O_ACCMODE
#define O_RDONLY DOSXC_O_RDONLY
#define WRITABLE(f) (((f)->flags & O_ACCMODE) != O_RDONLY)
#define READABLE(f) (((f)->flags & O_ACCMODE) != DOSXC_O_WRONLY)

/* ---- paths ---------------------------------------------------------- */

static int device_kind(const char *path) {
    static const char *const null_names[] = {"/dev/null", "\\dev\\null", "NUL", "NUL:"};
    static const char *const console_names[] = {"/dev/tty", "/dev/console", "\\dev\\tty", "CON", "CON:"};
    for (unsigned i = 0; i < sizeof(null_names) / sizeof(*null_names); ++i)
        if (rt_equal_nocase(path, null_names[i])) return KIND_NULL;
    for (unsigned i = 0; i < sizeof(console_names) / sizeof(*console_names); ++i)
        if (rt_equal_nocase(path, console_names[i])) return KIND_CONSOLE;
    return 0;
}

/* The canonical directory a process without parent starts in. */
static void root_directory(char *out) {
    out[0] = (char)rt_upper(ctx->drive);
    out[1] = ':';
    rt_strcpy(out + 2, ctx->cwd[0] == '\\' ? ctx->cwd : "\\");
}

/* "D:\DIR\FILE" from any of: FILE, DIR/FILE, /DIR/FILE, D:/DIR/FILE, D:FILE.
   A drive spec after a separator ("C:/HOME/D:/X", which Unix programs make
   by joining the current folder with a DOS path) starts again at that
   drive's root. */
int path_resolve(const struct process *p, const char *s, char *out) {
    char buffer[PATH_MAX_RT], cwd_buffer[PATH_MAX_RT];
    uptr n;
    if (!s || !*s) return -DOSXC_ENOENT;
    const char *cwd = p ? p->cwd : cwd_buffer;
    if (!p) root_directory(cwd_buffer);
    char drive = cwd[0];
    int absolute = *s == '/' || *s == '\\';
    if (((s[0] | 32) >= 'a' && (s[0] | 32) <= 'z') && s[1] == ':') {
        drive = (char)rt_upper(s[0]);
        if (!volume_exists(drive)) return -DOSXC_ENOENT;
        if (drive != cwd[0]) absolute = 1;
        s += 2;
        if (*s == '/' || *s == '\\') absolute = 1;
    }
    buffer[0] = drive;
    buffer[1] = ':';
    if (absolute) {
        buffer[2] = '\\';
        n = 3;
    } else {
        n = rt_strlen(cwd);
        if (n < 3 || n >= PATH_MAX_RT) return -DOSXC_ENAMETOOLONG;
        memcpy(buffer, cwd, n);
    }
    while (*s) {
        while (*s == '/' || *s == '\\') ++s;
        if (!*s) break;
        const char *start = s;
        while (*s && *s != '/' && *s != '\\') ++s;
        uptr length = (uptr)(s - start);
        if (length == 1 && start[0] == '.') continue;
        if (length == 2 && start[1] == ':' && ((start[0] | 32) >= 'a' && (start[0] | 32) <= 'z')) {
            char letter = (char)rt_upper(start[0]);
            if (!volume_exists(letter)) return -DOSXC_ENOENT;
            buffer[0] = letter;
            n = 3;
            continue;
        }
        if (length == 2 && start[0] == '.' && start[1] == '.') {
            while (n > 3 && buffer[n - 1] != '\\') --n;
            if (n > 3) --n;
            continue;
        }
        for (uptr i = 0; i < length; ++i) {
            char c = start[i];
            if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || (u8)c < 32)
                return -DOSXC_EINVAL;
        }
        if (n + length + 2 >= PATH_MAX_RT) return -DOSXC_ENAMETOOLONG;
        if (n > 3) buffer[n++] = '\\';
        memcpy(buffer + n, start, length);
        n += length;
    }
    buffer[n] = 0;
    rt_strcpy(out, buffer);
    return 0;
}

/* The POSIX spelling programs see: "D:/DIR/FILE" (as on DJGPP). */
void path_posix(const char *canonical, char *out, uptr size) {
    uptr i = 0;
    if (canonical[0] && canonical[1] == ':' && size > 3) {
        out[i++] = canonical[0];
        out[i++] = ':';
        canonical += 2;
    }
    for (; *canonical && i + 1 < size; ++canonical) out[i++] = *canonical == '\\' ? '/' : *canonical;
    out[i] = 0;
}

static void parent_of(const char *canonical, char *out) {
    rt_strcpy(out, canonical);
    uptr n = rt_strlen(out);
    while (n > 3 && out[n - 1] != '\\') --n;
    out[n > 3 ? n - 1 : 3] = 0;
}

static int fat_info(const char *canonical, struct fat32_info *info) {
    return !fs_stat(canonical, info) ? 0 : -1;
}

static u32 path_hash(const char *path) {
    u32 hash = 2166136261u;
    for (; *path; ++path) hash = (hash ^ (u32)rt_upper((u8)*path)) * 16777619u;
    return hash ? hash : 1;
}

static i64 fat_time(u16 date, u16 time) {
    if (!date) return 0;
    return rt_epoch(1980 + (date >> 9), (date >> 5) & 15, date & 31, time >> 11, (time >> 5) & 63, (time & 31) * 2);
}

static void fill_stat(struct dosxc_stat *st, u32 kind, u64 size, u8 attributes, i64 mtime, const char *path) {
    memset(st, 0, sizeof(*st));
    st->size = size;
    st->mtime = mtime;
    st->attributes = attributes;
    st->device = 1;
    st->inode = path ? path_hash(path) : kind;
    if (attributes & DOSXC_ATTR_DIRECTORY) st->mode = DOSXC_S_IFDIR | 0755;
    else if (kind == KIND_FILE) st->mode = DOSXC_S_IFREG | ((attributes & DOSXC_ATTR_READONLY) ? 0555 : 0755);
    else if (kind == KIND_PIPE_READ || kind == KIND_PIPE_WRITE) st->mode = DOSXC_S_IFIFO | 0600;
    else if (kind == KIND_SOCKET) st->mode = DOSXC_S_IFSOCK | 0666;
    else st->mode = DOSXC_S_IFCHR | 0666;
}

/* ---- inodes ----------------------------------------------------------- */

static struct inode *find_inode(const char *canonical) {
    for (unsigned i = 0; i < MAX_INODES; ++i)
        if (inodes[i].refs && !inodes[i].unlinked && rt_equal_nocase(inodes[i].path, canonical)) return &inodes[i];
    return 0;
}

static int reserve(struct inode *n, uptr needed) {
    if (needed <= n->capacity) return 0;
    uptr capacity = n->capacity ? n->capacity : 4096;
    while (capacity < needed) {
        if (capacity > ((uptr)-1) / 2) return -DOSXC_ENOMEM;
        capacity *= 2;
    }
    u8 *data = memory_alloc(capacity, PAGE, OWNER_KERNEL, 0);
    if (!data) return -DOSXC_ENOMEM;
    if (n->size) memcpy(data, n->data, n->size);
    if (n->data) memory_free(n->data, OWNER_KERNEL);
    n->data = data;
    n->capacity = capacity;
    return 0;
}

static int flush(struct inode *n) {
    if (!n->dirty || n->unlinked) return 0;
    if (n->size > 0xffffffffu) return -DOSXC_EFBIG;
    if (!fs_writable(n->path)) return -DOSXC_EROFS;
    if (fs_save(n->path, n->data ? n->data : empty_buffer, (u32)n->size)) return -DOSXC_ENOSPC;
    n->dirty = 0;
    struct fat32_info info;
    if (!fat_info(n->path, &info)) {
        n->date = info.date;
        n->time = info.time;
    }
    return 0;
}

static int release_inode(struct inode *n) {
    if (--n->refs) return 0;
    int result = flush(n);
    if (n->data) memory_free(n->data, OWNER_KERNEL);
    memset(n, 0, sizeof(*n));
    return result;
}

/* ---- open files ------------------------------------------------------- */

static struct open_file *new_open_file(int kind) {
    for (unsigned i = 0; i < MAX_OPEN; ++i)
        if (!open_files[i].refs) {
            memset(&open_files[i], 0, sizeof(open_files[i]));
            open_files[i].refs = 1;
            open_files[i].kind = kind;
            return &open_files[i];
        }
    return 0;
}

static void free_pipe(struct pipe *p) {
    if (p->readers || p->writers) return;
    if (p->data) memory_free(p->data, OWNER_KERNEL);
    memset(p, 0, sizeof(*p));
}

static int release_open(struct open_file *f) {
    if (--f->refs) return 0;
    int result = 0;
    if (f->kind == KIND_FILE) result = release_inode(f->inode);
    else if (f->kind == KIND_PIPE_READ) { --f->pipe->readers; free_pipe(f->pipe); }
    else if (f->kind == KIND_PIPE_WRITE) { --f->pipe->writers; free_pipe(f->pipe); }
    else if (f->kind == KIND_SOCKET) socket_close(f->socket);
    f->kind = 0;
    return result;
}

static int lowest_fd(struct process *p, int minimum) {
    if (minimum < 0) minimum = 0;
    for (int fd = minimum; fd < MAX_FDS; ++fd)
        if (!p->fds[fd]) return fd;
    return -DOSXC_EMFILE;
}

static struct open_file *lookup(int fd) {
    if (fd < 0 || fd >= MAX_FDS || !current) return 0;
    return current->fds[fd];
}

static int open_inode(struct open_file *f, const char *path, int flags) {
    int writing = (flags & O_ACCMODE) != O_RDONLY || (flags & DOSXC_O_TRUNC);
    struct inode *n = find_inode(path);
    struct fat32_info info;
    memset(&info, 0, sizeof(info));
    int exists = n || !fat_info(path, &info);
    if (exists && !n && (info.attributes & DOSXC_ATTR_DIRECTORY)) {
        if (writing) return -DOSXC_EISDIR;
        f->kind = KIND_DIRECTORY;
        rt_strcpy(f->path, path);
        return 0;
    }
    if (flags & DOSXC_O_DIRECTORY) return exists ? -DOSXC_ENOTDIR : -DOSXC_ENOENT;
    if (exists && (flags & DOSXC_O_CREAT) && (flags & DOSXC_O_EXCL)) return -DOSXC_EEXIST;
    if (!exists) {
        if (!(flags & DOSXC_O_CREAT)) return -DOSXC_ENOENT;
        char parent[PATH_MAX_RT];
        struct fat32_info folder;
        parent_of(path, parent);
        if (fat_info(parent, &folder)) return -DOSXC_ENOENT;
        if (!(folder.attributes & DOSXC_ATTR_DIRECTORY)) return -DOSXC_ENOTDIR;
        if (!fs_writable(path)) return -DOSXC_EROFS;
        if (fs_save(path, empty_buffer, 0)) return -DOSXC_ENOSPC;
        if (fat_info(path, &info)) return -DOSXC_EIO;
    } else if (!n && writing && (info.attributes & DOSXC_ATTR_READONLY)) {
        return -DOSXC_EACCES;
    }
    if (!n) {
        for (unsigned i = 0; i < MAX_INODES && !n; ++i)
            if (!inodes[i].refs) n = &inodes[i];
        if (!n) return -DOSXC_ENFILE;
        memset(n, 0, sizeof(*n));
        rt_strcpy(n->path, path);
        n->attributes = info.attributes;
        n->date = info.date;
        n->time = info.time;
        n->loaded = 1;
        if (!(flags & DOSXC_O_TRUNC) && info.size) {
            n->size = info.size;
            if (!writing) n->loaded = 0;       /* read on demand */
            else {
                if (reserve(n, info.size)) return -DOSXC_ENOMEM;
                if (fs_read(path, n->data, info.size)) {
                    memory_free(n->data, OWNER_KERNEL);
                    memset(n, 0, sizeof(*n));
                    return -DOSXC_EIO;
                }
            }
        }
    } else if ((flags & DOSXC_O_TRUNC) && n->size) {
        n->size = 0;
        n->loaded = 1;
        n->dirty = 1;
    }
    if (writing && !n->loaded) {               /* a reader opened it first */
        if (n->size && reserve(n, n->size)) return -DOSXC_ENOMEM;
        if (n->size && fs_read(path, n->data, (u32)n->size)) return -DOSXC_EIO;
        n->loaded = 1;
    }
    if ((flags & DOSXC_O_TRUNC) && exists && info.size) n->dirty = 1;
    ++n->refs;
    f->kind = KIND_FILE;
    f->inode = n;
    return 0;
}

static int api_open(const char *path, int flags, int mode) {
    (void)mode;
    struct process *p = current;
    int fd = lowest_fd(p, 0);
    if (fd < 0) return fd;
    int kind = device_kind(path);
    struct open_file *f = new_open_file(kind ? kind : KIND_FILE);
    if (!f) return -DOSXC_ENFILE;
    f->flags = flags & (O_ACCMODE | DOSXC_O_APPEND | DOSXC_O_NONBLOCK);
    if (!kind) {
        char canonical[PATH_MAX_RT];
        int r = path_resolve(p, path, canonical);
        if (!r) r = open_inode(f, canonical, flags);
        if (r) {
            f->refs = 0;
            f->kind = 0;
            return r;
        }
    }
    p->fds[fd] = f;
    p->cloexec[fd] = (flags & DOSXC_O_CLOEXEC) != 0;
    return fd;
}

static int api_close(int fd) {
    struct open_file *f = lookup(fd);
    if (!f) return -DOSXC_EBADF;
    current->fds[fd] = 0;
    current->cloexec[fd] = 0;
    return release_open(f);
}

/* ---- console (terminal.c) -------------------------------------------- */

int files_console_mode(int mode) { return terminal_mode(mode); }

/* ---- data transfer ---------------------------------------------------- */

static dosxc_ssize api_read(int fd, void *buffer, dosxc_size size) {
    struct open_file *f = lookup(fd);
    if (!f || !READABLE(f)) return -DOSXC_EBADF;
    switch (f->kind) {
    case KIND_FILE: {
        struct inode *n = f->inode;
        if (f->offset >= n->size) return 0;
        uptr available = (uptr)(n->size - f->offset);
        if (size > available) size = available;
        if (!n->loaded) {
            if (fs_read_cursor(n->path, &n->cursor, (u32)f->offset, buffer, (u32)size)) return -DOSXC_EIO;
        } else memcpy(buffer, n->data + f->offset, size);
        f->offset += size;
        return (dosxc_ssize)size;
    }
    case KIND_CONSOLE: return terminal_read(buffer, size);
    case KIND_NULL: return 0;
    case KIND_PIPE_READ: {
        struct pipe *p = f->pipe;
        uptr available = p->tail - p->head;
        if (size > available) size = available;
        memcpy(buffer, p->data + p->head, size);
        p->head += size;
        if (p->head == p->tail) p->head = p->tail = 0;
        return (dosxc_ssize)size;
    }
    case KIND_DIRECTORY: return -DOSXC_EISDIR;
    case KIND_SOCKET: return (dosxc_ssize)socket_read(f->socket, buffer, size, 0, (f->flags & DOSXC_O_NONBLOCK) != 0);
    }
    return -DOSXC_EBADF;
}

static dosxc_ssize api_write(int fd, const void *buffer, dosxc_size size) {
    struct open_file *f = lookup(fd);
    if (!f) return -DOSXC_EBADF;
    switch (f->kind) {
    case KIND_FILE: {
        if (!WRITABLE(f)) return -DOSXC_EBADF;
        struct inode *n = f->inode;
        if (f->flags & DOSXC_O_APPEND) f->offset = n->size;
        u64 end = f->offset + size;
        if (end > (uptr)-1 || end > 0xffffffffull) return -DOSXC_EFBIG;
        int r = reserve(n, (uptr)end);
        if (r) return r;
        if (f->offset > n->size) memset(n->data + n->size, 0, (uptr)(f->offset - n->size));
        memcpy(n->data + f->offset, buffer, size);
        f->offset = end;
        if (end > n->size) n->size = (uptr)end;
        n->dirty = 1;
        return (dosxc_ssize)size;
    }
    case KIND_CONSOLE: return terminal_write(buffer, size);
    case KIND_NULL: return (dosxc_ssize)size;
    case KIND_PIPE_WRITE: {
        struct pipe *p = f->pipe;
        if (!p->readers) return -DOSXC_EPIPE;
        if (p->tail + size > p->capacity) {
            uptr capacity = p->capacity ? p->capacity : 65536;
            while (capacity < p->tail + size) capacity *= 2;
            u8 *data = memory_alloc(capacity, PAGE, OWNER_KERNEL, 0);
            if (!data) return -DOSXC_ENOMEM;
            if (p->data) {
                memcpy(data, p->data, p->tail);
                memory_free(p->data, OWNER_KERNEL);
            }
            p->data = data;
            p->capacity = capacity;
        }
        memcpy(p->data + p->tail, buffer, size);
        p->tail += size;
        return (dosxc_ssize)size;
    }
    case KIND_SOCKET: return (dosxc_ssize)socket_write(f->socket, buffer, size, 0, (f->flags & DOSXC_O_NONBLOCK) != 0);
    }
    return -DOSXC_EBADF;
}

static dosxc_i64 api_seek(int fd, dosxc_i64 offset, int whence) {
    struct open_file *f = lookup(fd);
    if (!f) return -DOSXC_EBADF;
    if (f->kind == KIND_NULL) return 0;
    if (f->kind != KIND_FILE && f->kind != KIND_DIRECTORY) return -DOSXC_ESPIPE;
    i64 base = whence == 0 ? 0 : whence == 1 ? (i64)f->offset : whence == 2 && f->inode ? (i64)f->inode->size : -1;
    if (base < 0 || (whence == 2 && !f->inode)) return -DOSXC_EINVAL;
    if (base + offset < 0) return -DOSXC_EINVAL;
    f->offset = (u64)(base + offset);
    return (dosxc_i64)f->offset;
}

static int api_dup(int fd, int minimum) {
    struct open_file *f = lookup(fd);
    if (!f) return -DOSXC_EBADF;
    if (minimum < 0 || minimum >= MAX_FDS) return -DOSXC_EINVAL;
    int target = lowest_fd(current, minimum);
    if (target < 0) return target;
    ++f->refs;
    current->fds[target] = f;
    current->cloexec[target] = 0;
    return target;
}

static int api_dup2(int fd, int target) {
    struct open_file *f = lookup(fd);
    if (!f) return -DOSXC_EBADF;
    if (target < 0 || target >= MAX_FDS) return -DOSXC_EBADF;
    if (fd == target) return target;
    ++f->refs;
    if (current->fds[target]) release_open(current->fds[target]);
    current->fds[target] = f;
    current->cloexec[target] = 0;
    return target;
}

static int api_fdflags(int fd, int set, int flags) {
    if (!lookup(fd)) return -DOSXC_EBADF;
    if (set) current->cloexec[fd] = (flags & DOSXC_FD_CLOEXEC) != 0;
    return current->cloexec[fd] ? DOSXC_FD_CLOEXEC : 0;
}

static int api_status_flags(int fd) {
    struct open_file *f = lookup(fd);
    return f ? f->flags : -DOSXC_EBADF;
}

static int api_pipe(int fds[2]) {
    struct pipe *p = 0;
    for (unsigned i = 0; i < MAX_PIPES && !p; ++i)
        if (!pipes[i].readers && !pipes[i].writers) p = &pipes[i];
    if (!p) return -DOSXC_ENFILE;
    int reader = lowest_fd(current, 0);
    if (reader < 0) return reader;
    current->fds[reader] = (struct open_file *)1;     /* hold the slot */
    int writer = lowest_fd(current, 0);
    current->fds[reader] = 0;
    if (writer < 0) return writer;
    struct open_file *r = new_open_file(KIND_PIPE_READ);
    if (!r) return -DOSXC_ENFILE;
    struct open_file *w = new_open_file(KIND_PIPE_WRITE);
    if (!w) {
        r->refs = 0;
        return -DOSXC_ENFILE;
    }
    memset(p, 0, sizeof(*p));
    p->readers = p->writers = 1;
    r->pipe = w->pipe = p;
    r->flags = O_RDONLY;
    w->flags = DOSXC_O_WRONLY;
    current->fds[reader] = r;
    current->fds[writer] = w;
    current->cloexec[reader] = current->cloexec[writer] = 0;
    fds[0] = reader;
    fds[1] = writer;
    return 0;
}

static int api_isatty(int fd) {
    struct open_file *f = lookup(fd);
    if (!f) return -DOSXC_EBADF;
    return f->kind == KIND_CONSOLE;
}

static int api_ftruncate(int fd, dosxc_i64 length) {
    struct open_file *f = lookup(fd);
    if (!f) return -DOSXC_EBADF;
    if (f->kind != KIND_FILE || !WRITABLE(f)) return -DOSXC_EINVAL;
    if (length < 0 || (u64)length > 0xffffffffull) return -DOSXC_EINVAL;
    struct inode *n = f->inode;
    if ((uptr)length > n->size) {
        int r = reserve(n, (uptr)length);
        if (r) return r;
        memset(n->data + n->size, 0, (uptr)length - n->size);
    }
    n->size = (uptr)length;
    n->dirty = 1;
    return 0;
}

static int api_fsync(int fd) {
    struct open_file *f = lookup(fd);
    if (!f) return -DOSXC_EBADF;
    return f->kind == KIND_FILE ? flush(f->inode) : 0;
}

static int api_fstat(int fd, struct dosxc_stat *st) {
    struct open_file *f = lookup(fd);
    if (!f) return -DOSXC_EBADF;
    if (f->kind == KIND_FILE) {
        struct inode *n = f->inode;
        fill_stat(st, KIND_FILE, n->size, n->attributes, n->dirty ? cpu_rtc_time() : fat_time(n->date, n->time), n->path);
    } else if (f->kind == KIND_DIRECTORY) {
        fill_stat(st, KIND_DIRECTORY, 0, DOSXC_ATTR_DIRECTORY, 0, f->path);
    } else {
        fill_stat(st, (u32)f->kind, f->pipe ? f->pipe->tail - f->pipe->head : 0, 0, 0, 0);
    }
    return 0;
}

/* ---- names ------------------------------------------------------------ */

static int api_stat(const char *path, struct dosxc_stat *st) {
    int kind = device_kind(path);
    if (kind) {
        fill_stat(st, (u32)kind, 0, 0, 0, 0);
        return 0;
    }
    char canonical[PATH_MAX_RT];
    int r = path_resolve(current, path, canonical);
    if (r) return r;
    struct inode *n = find_inode(canonical);
    if (n) {
        fill_stat(st, KIND_FILE, n->size, n->attributes, n->dirty ? cpu_rtc_time() : fat_time(n->date, n->time), canonical);
        return 0;
    }
    struct fat32_info info;
    if (fat_info(canonical, &info)) return -DOSXC_ENOENT;
    fill_stat(st, KIND_FILE, info.size, info.attributes, fat_time(info.date, info.time), canonical);
    return 0;
}

static int api_unlink(const char *path) {
    char canonical[PATH_MAX_RT];
    int r = path_resolve(current, path, canonical);
    if (r) return r;
    struct fat32_info info;
    if (fat_info(canonical, &info)) return -DOSXC_ENOENT;
    if (info.attributes & DOSXC_ATTR_DIRECTORY) return -DOSXC_EISDIR;
    if (info.attributes & DOSXC_ATTR_READONLY) return -DOSXC_EACCES;
    struct inode *n = find_inode(canonical);
    if (!fs_writable(canonical)) return -DOSXC_EROFS;
    if (n) n->unlinked = 1;
    return fs_remove(canonical) ? -DOSXC_EIO : 0;
}

static int api_rename(const char *from, const char *to) {
    char source[PATH_MAX_RT], target[PATH_MAX_RT];
    int r = path_resolve(current, from, source);
    if (!r) r = path_resolve(current, to, target);
    if (r) return r;
    struct fat32_info info, existing;
    if (fat_info(source, &info)) return -DOSXC_ENOENT;
    if (rt_equal_nocase(source, target)) return 0;
    if (rt_upper(source[0]) != rt_upper(target[0])) return -DOSXC_EXDEV;
    if (!fs_writable(source)) return -DOSXC_EROFS;
    struct inode *n = find_inode(source);
    if (n && (r = flush(n))) return r;
    if (!fat_info(target, &existing)) {
        if (existing.attributes & DOSXC_ATTR_DIRECTORY) return -DOSXC_EISDIR;
        if (info.attributes & DOSXC_ATTR_DIRECTORY) return -DOSXC_ENOTDIR;
        struct inode *old = find_inode(target);
        if (old) old->unlinked = 1;
        if (fs_remove(target)) return -DOSXC_EACCES;
    }
    if (fs_rename(source, target)) return -DOSXC_EACCES;
    if (n) rt_strcpy(n->path, target);
    return 0;
}

static int api_mkdir(const char *path) {
    char canonical[PATH_MAX_RT], parent[PATH_MAX_RT];
    int r = path_resolve(current, path, canonical);
    if (r) return r;
    struct fat32_info info;
    if (!fat_info(canonical, &info)) return -DOSXC_EEXIST;
    parent_of(canonical, parent);
    if (fat_info(parent, &info)) return -DOSXC_ENOENT;
    if (!(info.attributes & DOSXC_ATTR_DIRECTORY)) return -DOSXC_ENOTDIR;
    if (!fs_writable(canonical)) return -DOSXC_EROFS;
    return fs_mkdir(canonical) ? -DOSXC_ENOSPC : 0;
}

static int api_rmdir(const char *path) {
    char canonical[PATH_MAX_RT];
    int r = path_resolve(current, path, canonical);
    if (r) return r;
    struct fat32_info info;
    if (fat_info(canonical, &info)) return -DOSXC_ENOENT;
    if (!(info.attributes & DOSXC_ATTR_DIRECTORY)) return -DOSXC_ENOTDIR;
    if (rt_strlen(canonical) <= 3 || rt_equal_nocase(canonical, current->cwd)) return -DOSXC_EBUSY;
    if (!fs_writable(canonical)) return -DOSXC_EROFS;
    return fs_rmdir(canonical) ? -DOSXC_ENOTEMPTY : 0;
}

static int api_chdir(const char *path) {
    char canonical[PATH_MAX_RT];
    int r = path_resolve(current, path, canonical);
    if (r) return r;
    struct fat32_info info;
    if (fat_info(canonical, &info)) return -DOSXC_ENOENT;
    if (!(info.attributes & DOSXC_ATTR_DIRECTORY)) return -DOSXC_ENOTDIR;
    rt_strcpy(current->cwd, canonical);
    return 0;
}

static int api_getcwd(char *buffer, dosxc_size size) {
    char posix[PATH_MAX_RT];
    path_posix(current->cwd, posix, sizeof(posix));
    if (rt_strlen(posix) + 1 > size) return -DOSXC_ERANGE;
    rt_strcpy(buffer, posix);
    return 0;
}

static int api_resolve(const char *path, char *buffer, dosxc_size size) {
    char canonical[PATH_MAX_RT];
    int r = path_resolve(current, path, canonical);
    if (r) return r;
    char posix[PATH_MAX_RT];
    path_posix(canonical, posix, sizeof(posix));
    uptr n = rt_strlen(posix);
    if (n + 1 > size) return -DOSXC_ENAMETOOLONG;
    rt_strcpy(buffer, posix);
    return (int)n;
}

/* The DOS spelling, "D:\DIR\FILE". */
int files_fullpath(const char *path, char *buffer, dosxc_size size) {
    char canonical[PATH_MAX_RT];
    int r = path_resolve(current, path, canonical);
    if (r) return r;
    uptr n = rt_strlen(canonical);
    if (n + 1 > size) return -DOSXC_ENAMETOOLONG;
    rt_strcpy(buffer, canonical);
    return (int)n;
}

struct listing { struct dosxc_dirent *entries; uptr capacity, count; };

static int collect(void *context, const struct fat32_info *info) {
    struct listing *l = context;
    if (info->name[0] == '.' && (!info->name[1] || (info->name[1] == '.' && !info->name[2]))) return 0;
    if (l->count < l->capacity) {
        struct dosxc_dirent *e = &l->entries[l->count];
        memset(e, 0, sizeof(*e));
        uptr n = rt_strlen(info->name);
        if (n > 255) n = 255;
        memcpy(e->name, info->name, n);
        e->attributes = info->attributes;
        e->size = info->size;
        e->mtime = fat_time(info->date, info->time);
    }
    ++l->count;
    return 0;
}

static dosxc_ssize api_listdir(const char *path, struct dosxc_dirent *entries, dosxc_size capacity) {
    char canonical[PATH_MAX_RT];
    int r = path_resolve(current, path, canonical);
    if (r) return r;
    struct fat32_info info;
    if (fat_info(canonical, &info)) return -DOSXC_ENOENT;
    if (!(info.attributes & DOSXC_ATTR_DIRECTORY)) return -DOSXC_ENOTDIR;
    struct listing l = {entries, capacity, 0};
    if (fs_list(canonical, collect, &l)) return -DOSXC_EIO;
    return (dosxc_ssize)l.count;
}

/* ---- sockets (socket.c) ------------------------------------------------ */

int files_socket_open(struct socket *s) {
    int fd = lowest_fd(current, 0);
    if (fd < 0) return fd;
    struct open_file *f = new_open_file(KIND_SOCKET);
    if (!f) return -DOSXC_ENFILE;
    f->flags = DOSXC_O_RDWR;
    f->socket = s;
    current->fds[fd] = f;
    current->cloexec[fd] = 0;
    return fd;
}

struct socket *files_socket(int fd, int *flags) {
    struct open_file *f = lookup(fd);
    if (!f || f->kind != KIND_SOCKET) return 0;
    if (flags) *flags = f->flags;
    return f->socket;
}

int files_set_flags(int fd, int flags) {
    struct open_file *f = lookup(fd);
    if (!f) return -DOSXC_EBADF;
    f->flags = (f->flags & ~(DOSXC_O_NONBLOCK | DOSXC_O_APPEND)) | (flags & (DOSXC_O_NONBLOCK | DOSXC_O_APPEND));
    return 0;
}

/* ---- process plumbing ------------------------------------------------- */

int files_is_executable(const char *canonical, u8 magic[4]) {
    struct fat32_info info;
    if (fat_info(canonical, &info) || (info.attributes & DOSXC_ATTR_DIRECTORY)) return -DOSXC_ENOENT;
    if (info.size < 4) return -DOSXC_ENOEXEC;
    struct inode *n = find_inode(canonical);
    if (n && n->dirty) return -DOSXC_EBUSY;           /* still being written */
    return fs_read_at(canonical, 0, magic, 4) ? -DOSXC_EIO : 0;
}

int files_setup(struct process *child, struct process *parent, const char *output, int append) {
    memset(child->fds, 0, sizeof(child->fds));
    memset(child->cloexec, 0, sizeof(child->cloexec));
    if (parent) rt_strcpy(child->cwd, parent->cwd);
    else root_directory(child->cwd);
    if (parent) {
        for (int fd = 0; fd < MAX_FDS; ++fd)
            if (parent->fds[fd] && !parent->cloexec[fd]) {
                child->fds[fd] = parent->fds[fd];
                ++child->fds[fd]->refs;
            }
        return 0;
    }
    struct open_file *console = new_open_file(KIND_CONSOLE);
    if (!console) return -DOSXC_ENFILE;
    console->flags = DOSXC_O_RDWR;
    console->refs = 3;
    child->fds[0] = child->fds[1] = child->fds[2] = console;
    terminal_input_reset();
    terminal_reset();
    if (output) {
        struct process *saved = current;
        current = child;
        current->fds[1] = 0;
        --console->refs;
        int fd = api_open(output, DOSXC_O_WRONLY | DOSXC_O_CREAT | (append ? DOSXC_O_APPEND : DOSXC_O_TRUNC), 0666);
        current = saved;
        if (fd != 1) {
            files_release(child);
            return fd < 0 ? fd : -DOSXC_EIO;
        }
    }
    return 0;
}

void files_release(struct process *p) {
    for (int fd = 0; fd < MAX_FDS; ++fd)
        if (p->fds[fd]) {
            struct open_file *f = p->fds[fd];
            p->fds[fd] = 0;
            if (release_open(f)) rt_print("Warning: a file could not be written back to disk.\r\n");
        }
}

/* Write every modified file back (before a 16-bit program runs). */
void files_flush_all(void) {
    for (unsigned i = 0; i < MAX_INODES; ++i)
        if (inodes[i].refs) flush(&inodes[i]);
}

/* Drive LETTER went away (a USB disk was pulled or ejected).  Files still
   open there fail from now on instead of reaching whatever disk gets the
   letter next; directories of processes there are left as they are. */
void files_volume_gone(char letter) {
    letter = (char)rt_upper(letter);
    for (unsigned i = 0; i < MAX_INODES; ++i)
        if (inodes[i].refs && rt_upper(inodes[i].path[0]) == letter && inodes[i].path[1] == ':') {
            inodes[i].path[0] = '?';
            inodes[i].dirty = 0;
            memset(&inodes[i].cursor, 0, sizeof(inodes[i].cursor));
        }
    for (unsigned i = 0; i < MAX_OPEN; ++i)
        if (open_files[i].refs && rt_upper(open_files[i].path[0]) == letter && open_files[i].path[1] == ':')
            open_files[i].path[0] = '?';
}

/* Write back what is waiting for drive LETTER (before it is ejected). */
void files_flush_volume(char letter) {
    letter = (char)rt_upper(letter);
    for (unsigned i = 0; i < MAX_INODES; ++i)
        if (inodes[i].refs && rt_upper(inodes[i].path[0]) == letter) flush(&inodes[i]);
}

int files_init(void) {
    memset(inodes, 0, sizeof(inodes));
    memset(open_files, 0, sizeof(open_files));
    memset(pipes, 0, sizeof(pipes));
    return 0;
}

void files_fill_api(struct dosxc_api *api) {
    api->open = api_open;
    api->close = api_close;
    api->read = api_read;
    api->write = api_write;
    api->seek = api_seek;
    api->dup = api_dup;
    api->dup2 = api_dup2;
    api->fdflags = api_fdflags;
    api->status_flags = api_status_flags;
    api->pipe = api_pipe;
    api->isatty = api_isatty;
    api->ftruncate = api_ftruncate;
    api->fsync = api_fsync;
    api->fstat = api_fstat;
    api->stat = api_stat;
    api->unlink = api_unlink;
    api->rename = api_rename;
    api->mkdir = api_mkdir;
    api->rmdir = api_rmdir;
    api->chdir = api_chdir;
    api->getcwd = api_getcwd;
    api->listdir = api_listdir;
    api->resolve = api_resolve;
}
