/*
 * DOS-XC /os/libc/include/sys/eventfd.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _SYS_EVENTFD_H
#define _SYS_EVENTFD_H
#include <stdint.h>
#define EFD_SEMAPHORE 1
#define EFD_CLOEXEC 02000000
#define EFD_NONBLOCK 04000
typedef uint64_t eventfd_t;
#ifdef __cplusplus
extern "C" {
#endif
int eventfd(unsigned, int);
#ifdef __cplusplus
}
#endif

#endif
