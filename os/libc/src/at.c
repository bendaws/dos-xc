/*
 * DOS-XC /os/libc/src/at.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "libc.h"
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The kernel keeps no directory handles a path can be relative to, so the
   library remembers the full path of each descriptor open on a directory. */
static char *directories[OPEN_MAX];

static int absolute(const char *path) {
    return path[0] == '/' || path[0] == '\\' || (path[0] && path[1] == ':');
}

/* FD was opened on PATH: keep its full path if it is a directory. */
hidden void __dir_record(int fd, const char *path) {
    struct stat s;
    char full[PATH_MAX];
    if (fd < 0 || fd >= OPEN_MAX) return;
    free(directories[fd]);
    directories[fd] = 0;
    if (fstat(fd, &s) || !S_ISDIR(s.st_mode)) return;
    if (dosxc_sys_fullpath(path, full, sizeof(full)) < 0) return;
    directories[fd] = strdup(full);
}

hidden void __dir_closed(int fd) {
    if (fd < 0 || fd >= OPEN_MAX) return;
    free(directories[fd]);
    directories[fd] = 0;
}

hidden void __dir_copied(int from, int to) {
    if (to < 0 || to >= OPEN_MAX || from == to) return;
    free(directories[to]);
    directories[to] = from >= 0 && from < OPEN_MAX && directories[from] ? strdup(directories[from]) : 0;
}

/* PATH relative to the directory open as DIRFD, in BUFFER; 0 with errno set. */
static const char *at(int dirfd, const char *path, char *buffer) {
    if (!path) { errno = EFAULT; return 0; }
    if (dirfd == AT_FDCWD || absolute(path)) return path;
    if (dirfd < 0 || dirfd >= OPEN_MAX) { errno = EBADF; return 0; }
    if (!directories[dirfd]) {
        errno = fcntl(dirfd, F_GETFD) < 0 ? EBADF : ENOTDIR;
        return 0;
    }
    if (!*path) return directories[dirfd];
    if ((size_t)snprintf(buffer, PATH_MAX, "%s/%s", directories[dirfd], path) >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return 0;
    }
    return buffer;
}

#define AT(dirfd, path) char at_buffer[PATH_MAX]; const char *full = at(dirfd, path, at_buffer); if (!full) return -1

int openat(int dirfd, const char *path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    AT(dirfd, path);
    return open(full, flags, mode);
}

int fstatat(int dirfd, const char *path, struct stat *out, int flags) {
    if ((flags & AT_EMPTY_PATH) && path && !*path) return fstat(dirfd, out);
    AT(dirfd, path);
    return stat(full, out);
}

int unlinkat(int dirfd, const char *path, int flags) {
    AT(dirfd, path);
    return (flags & AT_REMOVEDIR) ? rmdir(full) : unlink(full);
}

int renameat(int from_dir, const char *from, int to_dir, const char *to) {
    char from_buffer[PATH_MAX], to_buffer[PATH_MAX];
    const char *a = at(from_dir, from, from_buffer), *b = a ? at(to_dir, to, to_buffer) : 0;
    return b ? rename(a, b) : -1;
}

int renameat2(int from_dir, const char *from, int to_dir, const char *to, unsigned flags) {
    char from_buffer[PATH_MAX], to_buffer[PATH_MAX];
    const char *a = at(from_dir, from, from_buffer), *b = a ? at(to_dir, to, to_buffer) : 0;
    if (!b) return -1;
    if (flags & RENAME_NOREPLACE) {
        struct stat s;
        if (!stat(b, &s)) { errno = EEXIST; return -1; }
    } else if (flags) {
        errno = EINVAL;
        return -1;
    }
    return rename(a, b);
}

int linkat(int from_dir, const char *from, int to_dir, const char *to, int flags) {
    char from_buffer[PATH_MAX], to_buffer[PATH_MAX];
    const char *a = at(from_dir, from, from_buffer), *b = a ? at(to_dir, to, to_buffer) : 0;
    (void)flags;
    return b ? link(a, b) : -1;
}

int symlinkat(const char *target, int dirfd, const char *path) {
    AT(dirfd, path);
    return symlink(target, full);
}

ssize_t readlinkat(int dirfd, const char *path, char *buffer, size_t size) {
    AT(dirfd, path);
    return readlink(full, buffer, size);
}

int mkdirat(int dirfd, const char *path, mode_t mode) {
    AT(dirfd, path);
    return mkdir(full, mode);
}

int faccessat(int dirfd, const char *path, int mode, int flags) {
    (void)flags;
    AT(dirfd, path);
    return access(full, mode);
}

int fchmodat(int dirfd, const char *path, mode_t mode, int flags) {
    (void)flags;
    AT(dirfd, path);
    return chmod(full, mode);
}

int fchownat(int dirfd, const char *path, uid_t uid, gid_t gid, int flags) {
    (void)flags;
    AT(dirfd, path);
    return chown(full, uid, gid);
}

int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags) {
    (void)times; (void)flags;
    if (!path) return futimens(dirfd, times);
    AT(dirfd, path);
    return chmod(full, 0);
}

int mkfifoat(int dirfd, const char *path, mode_t mode) {
    AT(dirfd, path);
    return mkfifo(full, mode);
}

int mknodat(int dirfd, const char *path, mode_t mode, dev_t device) {
    AT(dirfd, path);
    return mknod(full, mode, device);
}

int fchdir(int fd) {
    if (fd < 0 || fd >= OPEN_MAX || !directories[fd]) {
        errno = fd >= 0 && fd < OPEN_MAX && fcntl(fd, F_GETFD) >= 0 ? ENOTDIR : EBADF;
        return -1;
    }
    return chdir(directories[fd]);
}

hidden const char *__dir_path(int fd) { return fd >= 0 && fd < OPEN_MAX ? directories[fd] : 0; }
