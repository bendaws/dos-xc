/*
 * DOS-XC /os/libc/src/unistd.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "libc.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <utime.h>
#include <dirent.h>
#include <stdint.h>
#include <dosxc/abi.h>
#include <dosxc/syscall.h>

int open(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return (int)__result(dosxc_sys_open(path, flags, mode));
}

int openat(int dirfd, const char *path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    if (dirfd != AT_FDCWD && path[0] != '/' && path[0] != '\\' && !(path[0] && path[1] == ':')) {
        errno = ENOSYS;
        return -1;
    }
    return (int)__result(dosxc_sys_open(path, flags, mode));
}

int creat(const char *path, mode_t mode) { return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode); }
int close(int fd) { return fd >= SHM_FD_BASE ? __shm_close(fd) : (int)__result(dosxc_sys_close(fd)); }
ssize_t read(int fd, void *buffer, size_t size) {
    ssize_t n = __result(dosxc_sys_read(fd, buffer, size));
    if (n > 0 && __console_raw && dosxc_sys_isatty(fd) > 0) n = __console_input(buffer, n);
    return n;
}
ssize_t write(int fd, const void *buffer, size_t size) { return __result(dosxc_sys_write(fd, buffer, size)); }

off_t lseek(int fd, off_t offset, int whence) {
    long long r = dosxc_sys_seek(fd, offset, whence);
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

ssize_t pread(int fd, void *buffer, size_t size, off_t offset) {
    off_t saved = lseek(fd, 0, SEEK_CUR);
    if (saved < 0 || lseek(fd, offset, SEEK_SET) < 0) return -1;
    ssize_t n = read(fd, buffer, size);
    lseek(fd, saved, SEEK_SET);
    return n;
}

ssize_t pwrite(int fd, const void *buffer, size_t size, off_t offset) {
    off_t saved = lseek(fd, 0, SEEK_CUR);
    if (saved < 0 || lseek(fd, offset, SEEK_SET) < 0) return -1;
    ssize_t n = write(fd, buffer, size);
    lseek(fd, saved, SEEK_SET);
    return n;
}

int dup(int fd) { return fd >= SHM_FD_BASE ? __shm_dup(fd) : (int)__result(dosxc_sys_dup(fd, 0)); }
int dup2(int fd, int target) { return (int)__result(dosxc_sys_dup2(fd, target)); }
int dup3(int fd, int target, int flags) {
    if (fd == target) {
        errno = EINVAL;
        return -1;
    }
    int r = dup2(fd, target);
    if (r >= 0 && (flags & O_CLOEXEC)) dosxc_sys_fdflags(r, 1, DOSXC_FD_CLOEXEC);
    return r;
}

int fcntl(int fd, int command, ...) {
    va_list ap;
    va_start(ap, command);
    long argument = va_arg(ap, long);
    va_end(ap);
    if (fd >= SHM_FD_BASE) {
        switch (command) {
        case F_DUPFD:
        case F_DUPFD_CLOEXEC: return __shm_dup(fd);
        case F_GETFD: return FD_CLOEXEC;
        case F_GETFL: return O_RDWR;
        case F_GET_SEALS: return 0;
        case F_SETFD:
        case F_SETFL:
        case F_ADD_SEALS: return 0;
        }
        errno = EINVAL;
        return -1;
    }
    switch (command) {
    case F_DUPFD: return (int)__result(dosxc_sys_dup(fd, (int)argument));
    case F_DUPFD_CLOEXEC: {
        int r = (int)__result(dosxc_sys_dup(fd, (int)argument));
        if (r >= 0) dosxc_sys_fdflags(r, 1, DOSXC_FD_CLOEXEC);
        return r;
    }
    case F_GETFD: return (int)__result(dosxc_sys_fdflags(fd, 0, 0));
    case F_SETFD: return (int)__result(dosxc_sys_fdflags(fd, 1, (int)argument)) < 0 ? -1 : 0;
    case F_GETFL: return (int)__result(dosxc_sys_status_flags(fd));
    case F_SETFL: return (int)__result(dosxc_sys_setfl(fd, (int)argument));
    case F_GETLK: {
        if (__result(dosxc_sys_status_flags(fd)) < 0) return -1;
        ((struct flock *)argument)->l_type = F_UNLCK;
        return 0;
    }
    case F_SETLK:
    case F_SETLKW: return __result(dosxc_sys_status_flags(fd)) < 0 ? -1 : 0;
    }
    errno = EINVAL;
    return -1;
}

int pipe(int fds[2]) { return (int)__result(dosxc_sys_pipe(fds)); }
int pipe2(int fds[2], int flags) {
    if (pipe(fds)) return -1;
    if (flags & O_CLOEXEC) {
        dosxc_sys_fdflags(fds[0], 1, DOSXC_FD_CLOEXEC);
        dosxc_sys_fdflags(fds[1], 1, DOSXC_FD_CLOEXEC);
    }
    if (flags & O_NONBLOCK) {
        dosxc_sys_setfl(fds[0], O_NONBLOCK);
        dosxc_sys_setfl(fds[1], O_NONBLOCK);
    }
    return 0;
}

int isatty(int fd) {
    int r = dosxc_sys_isatty(fd);
    if (r < 0) {
        errno = -r;
        return 0;
    }
    if (!r) errno = ENOTTY;
    return r;
}

int ftruncate(int fd, off_t length) { return fd >= SHM_FD_BASE ? __shm_truncate(fd, length) : (int)__result(dosxc_sys_ftruncate(fd, length)); }
int truncate(const char *path, off_t length) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    int r = ftruncate(fd, length);
    int saved = errno;
    if (close(fd) && !r) return -1;
    errno = saved;
    return r;
}
int fsync(int fd) { return (int)__result(dosxc_sys_fsync(fd)); }
int fdatasync(int fd) { return fsync(fd); }
void sync(void) {}
int lockf(int fd, int command, off_t length) {
    (void)command;
    (void)length;
    return __result(dosxc_sys_status_flags(fd)) < 0 ? -1 : 0;
}

/* ---- stat ---- */
hidden void __stat_convert(const struct dosxc_stat *in, void *output) {
    struct stat *out = output;
    memset(out, 0, sizeof(*out));
    out->st_dev = in->device;
    out->st_ino = in->inode;
    out->st_mode = in->mode;
    out->st_nlink = 1;
    out->st_size = (off_t)in->size;
    out->st_blksize = 4096;
    out->st_blocks = (blkcnt_t)((in->size + 511) / 512);
    out->st_atim.tv_sec = out->st_mtim.tv_sec = out->st_ctim.tv_sec = in->mtime;
    out->st_attributes = in->attributes;
}

int stat(const char *path, struct stat *out) {
    struct dosxc_stat s;
    if (__result(dosxc_sys_stat(path, &s)) < 0) return -1;
    __stat_convert(&s, out);
    return 0;
}
int lstat(const char *path, struct stat *out) { return stat(path, out); }
int fstat(int fd, struct stat *out) {
    if (fd >= SHM_FD_BASE) {
        off_t size;
        if (__shm_size(fd, &size)) return -1;
        memset(out, 0, sizeof *out);
        out->st_mode = S_IFREG | 0600;
        out->st_nlink = 1;
        out->st_size = size;
        out->st_blksize = 4096;
        return 0;
    }
    struct dosxc_stat s;
    if (__result(dosxc_sys_fstat(fd, &s)) < 0) return -1;
    __stat_convert(&s, out);
    return 0;
}
int fstatat(int dirfd, const char *path, struct stat *out, int flags) {
    (void)flags;
    if (dirfd != AT_FDCWD && path[0] != '/' && path[0] != '\\') {
        errno = ENOSYS;
        return -1;
    }
    return stat(path, out);
}

int access(const char *path, int mode) {
    struct dosxc_stat s;
    if (__result(dosxc_sys_stat(path, &s)) < 0) return -1;
    if ((mode & W_OK) && (s.attributes & DOSXC_ATTR_READONLY)) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

int mkdir(const char *path, mode_t mode) {
    (void)mode;
    return (int)__result(dosxc_sys_mkdir(path));
}
int rmdir(const char *path) { return (int)__result(dosxc_sys_rmdir(path)); }
int unlink(const char *path) { return (int)__result(dosxc_sys_unlink(path)); }
int chdir(const char *path) { return (int)__result(dosxc_sys_chdir(path)); }
int fchdir(int fd) {
    (void)fd;
    errno = ENOSYS;
    return -1;
}
int chmod(const char *path, mode_t mode) {
    (void)mode;
    struct dosxc_stat s;
    return (int)__result(dosxc_sys_stat(path, &s)) < 0 ? -1 : 0;
}
int fchmod(int fd, mode_t mode) {
    (void)mode;
    struct dosxc_stat s;
    return (int)__result(dosxc_sys_fstat(fd, &s)) < 0 ? -1 : 0;
}
static mode_t current_umask = 022;
mode_t umask(mode_t mask) {
    mode_t old = current_umask;
    current_umask = mask & 0777;
    return old;
}
int chown(const char *path, uid_t uid, gid_t gid) { (void)uid; (void)gid; return chmod(path, 0); }
int lchown(const char *path, uid_t uid, gid_t gid) { return chown(path, uid, gid); }
int fchown(int fd, uid_t uid, gid_t gid) { (void)uid; (void)gid; return fchmod(fd, 0); }
int mkfifo(const char *path, mode_t mode) { (void)path; (void)mode; errno = ENOSYS; return -1; }
int mknod(const char *path, mode_t mode, dev_t device) { (void)path; (void)mode; (void)device; errno = ENOSYS; return -1; }
int link(const char *from, const char *to) { (void)from; (void)to; errno = EPERM; return -1; }
int symlink(const char *from, const char *to) { (void)from; (void)to; errno = EPERM; return -1; }
/* What Linux programs read to find themselves: the program's path from the
   root of the current drive, with forward slashes. */
static ssize_t own_path(char *buffer, size_t size) {
    static struct dosxc_library_where where;
    const char *from;
    size_t length;
    extern uintptr_t __dosxc_image_base;

    if (dosxc_sys_library(DOSXC_LIB_ADDRESS, (void *)__dosxc_image_base, &where) <= 0) {
        errno = ENOENT;
        return -1;
    }
    from = where.path;
    if (from[0] && from[1] == ':') from += 2;
    length = strlen(from);
    if (length > size) length = size;
    for (size_t i = 0; i < length; i++) buffer[i] = from[i] == '\\' ? '/' : from[i];
    return (ssize_t)length;
}

ssize_t readlink(const char *path, char *buffer, size_t size) {
    struct stat s;
    if (!strcmp(path, "/proc/self/exe")) return own_path(buffer, size);
    if (stat(path, &s)) return -1;
    errno = EINVAL;
    return -1;
}
/* FAT keeps one timestamp; DOS-XC does not let programs change it. */
int utime(const char *path, const struct utimbuf *times) { (void)times; return chmod(path, 0); }
int utimes(const char *path, const struct timeval times[2]) { (void)times; return chmod(path, 0); }
int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags) {
    (void)dirfd; (void)times; (void)flags;
    return chmod(path, 0);
}
int futimens(int fd, const struct timespec times[2]) { (void)times; return fchmod(fd, 0); }

char *getcwd(char *buffer, size_t size) {
    char local[PATH_MAX];
    int allocated = 0;
    if (!buffer) {
        if (!size) size = PATH_MAX;
        buffer = malloc(size);
        if (!buffer) return 0;
        allocated = 1;
    } else if (!size) {
        errno = EINVAL;
        return 0;
    }
    if (__result(dosxc_sys_getcwd(local, sizeof(local))) < 0) goto fail;
    if (strlen(local) + 1 > size) {
        errno = ERANGE;
        goto fail;
    }
    strcpy(buffer, local);
    return buffer;
fail:
    if (allocated) free(buffer);
    return 0;
}

char *realpath(const char *path, char *resolved) {
    char local[PATH_MAX];
    struct dosxc_stat s;
    if (!path) {
        errno = EINVAL;
        return 0;
    }
    if (__result(dosxc_sys_resolve(path, local, sizeof(local))) < 0) return 0;
    if (__result(dosxc_sys_stat(local, &s)) < 0) return 0;
    if (!resolved) return strdup(local);
    strcpy(resolved, local);
    return resolved;
}

long pathconf(const char *path, int name) {
    (void)path;
    switch (name) {
    case _PC_NAME_MAX: return NAME_MAX;
    case _PC_PATH_MAX: return PATH_MAX;
    case _PC_PIPE_BUF: return PIPE_BUF;
    case _PC_LINK_MAX: return 1;
    }
    return 255;
}
long fpathconf(int fd, int name) { (void)fd; return pathconf(0, name); }

/* ---- directories ---- */
struct __dosxc_dir {
    struct dosxc_dirent *entries;
    size_t count, position;
    struct dirent current;
};

DIR *opendir(const char *path) {
    DIR *d = calloc(1, sizeof(*d));
    if (!d) return 0;
    size_t capacity = 64;
    for (;;) {
        d->entries = malloc(capacity * sizeof(struct dosxc_dirent));
        if (!d->entries) {
            free(d);
            return 0;
        }
        long n = dosxc_sys_listdir(path, d->entries, capacity);
        if (n < 0) {
            free(d->entries);
            free(d);
            errno = (int)-n;
            return 0;
        }
        if ((size_t)n <= capacity) {
            d->count = (size_t)n;
            return d;
        }
        free(d->entries);
        capacity = (size_t)n + 16;
    }
}

DIR *fdopendir(int fd) {
    (void)fd;
    errno = ENOSYS;
    return 0;
}

struct dirent *readdir(DIR *d) {
    if (d->position >= d->count) return 0;
    struct dosxc_dirent *e = &d->entries[d->position];
    memset(&d->current, 0, sizeof(d->current));
    strcpy(d->current.d_name, e->name);
    d->current.d_ino = d->position + 1;
    d->current.d_off = (off_t)++d->position;
    d->current.d_reclen = sizeof(struct dirent);
    d->current.d_type = (e->attributes & DOSXC_ATTR_DIRECTORY) ? DT_DIR : DT_REG;
    return &d->current;
}

int readdir_r(DIR *d, struct dirent *entry, struct dirent **result) {
    struct dirent *e = readdir(d);
    if (e) *entry = *e;
    *result = e ? entry : 0;
    return 0;
}

int closedir(DIR *d) {
    free(d->entries);
    free(d);
    return 0;
}
void rewinddir(DIR *d) { d->position = 0; }
long telldir(DIR *d) { return (long)d->position; }
void seekdir(DIR *d, long position) { d->position = (size_t)position; }
int dirfd(DIR *d) { (void)d; errno = ENOSYS; return -1; }

int alphasort(const struct dirent **a, const struct dirent **b) { return strcoll((*a)->d_name, (*b)->d_name); }

int scandir(const char *path, struct dirent ***list, int (*select)(const struct dirent *),
            int (*compare)(const struct dirent **, const struct dirent **)) {
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent **items = 0, *e;
    size_t count = 0;
    while ((e = readdir(d))) {
        if (select && !select(e)) continue;
        struct dirent **bigger = realloc(items, (count + 1) * sizeof(*items));
        struct dirent *copy = malloc(sizeof(*copy));
        if (!bigger || !copy) {
            free(copy);
            items = bigger ? bigger : items;
            while (count) free(items[--count]);
            free(items);
            closedir(d);
            errno = ENOMEM;
            return -1;
        }
        items = bigger;
        *copy = *e;
        items[count++] = copy;
    }
    closedir(d);
    if (compare) qsort(items, count, sizeof(*items), (int (*)(const void *, const void *))compare);
    *list = items;
    return (int)count;
}
