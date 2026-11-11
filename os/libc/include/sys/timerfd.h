/*
 * DOS-XC /os/libc/include/sys/timerfd.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _SYS_TIMERFD_H
#define _SYS_TIMERFD_H
#include <time.h>
#define TFD_NONBLOCK 0x800
#define TFD_CLOEXEC 0x80000
#define TFD_TIMER_ABSTIME 1
#ifdef __cplusplus
extern "C" {
#endif
int timerfd_create(int, int);
int timerfd_settime(int, int, const struct itimerspec *, struct itimerspec *);
int timerfd_gettime(int, struct itimerspec *);
#ifdef __cplusplus
}
#endif
#endif
