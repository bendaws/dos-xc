/*
 * DOS-XC /os/libc/src/misc.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "libc.h"
#include <fcntl.h>
#include <sys/random.h>
#include <sys/select.h>

ssize_t getrandom(void *buffer, size_t size, unsigned flags) {
    (void)flags;
    return (ssize_t)dosxc_sys_random(buffer, size);
}

int getentropy(void *buffer, size_t size) {
    if (size > 256) { errno = EIO; return -1; }
    getrandom(buffer, size, 0);
    return 0;
}

int posix_fadvise(int fd, off_t offset, off_t length, int advice) {
    (void)fd;
    (void)offset;
    (void)length;
    (void)advice;
    return 0;
}
