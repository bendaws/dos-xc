/*
 * DOS-XC C library - errno and miscellaneous interfaces
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#include "libc.h"
#include <fcntl.h>
#include <sys/select.h>

int errno;
int *__errno_location(void) { return &errno; }

int posix_fadvise(int fd, off_t offset, off_t length, int advice) {
    (void)fd;
    (void)offset;
    (void)length;
    (void)advice;
    return 0;
}
