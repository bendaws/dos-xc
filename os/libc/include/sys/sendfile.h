/*
 * DOS-XC /os/libc/include/sys/sendfile.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _SYS_SENDFILE_H
#define _SYS_SENDFILE_H
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
ssize_t sendfile(int out, int in, off_t *offset, size_t count);
#ifdef __cplusplus
}
#endif

#endif
