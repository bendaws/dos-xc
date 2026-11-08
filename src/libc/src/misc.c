/*
 * DOS-XC C library - errno and miscellaneous interfaces
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#include "libc.h"
#include <fcntl.h>
#include <sys/select.h>

int errno;
int *__errno_location(void) { return &errno; }

/* Descriptors are always ready: reads and writes never block. */
int select(int count, fd_set *read_set, fd_set *write_set, fd_set *error_set, struct timeval *timeout) {
    (void)timeout;
    int ready = 0;
    for (int fd = 0; fd < count && fd < FD_SETSIZE; ++fd) {
        if (read_set && FD_ISSET(fd, read_set)) ++ready;
        if (write_set && FD_ISSET(fd, write_set)) ++ready;
    }
    if (error_set) FD_ZERO(error_set);
    return ready;
}

int posix_fadvise(int fd, off_t offset, off_t length, int advice) {
    (void)fd;
    (void)offset;
    (void)length;
    (void)advice;
    return 0;
}
