/*
 * DOS-XC C library - sched.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#ifndef _SCHED_H
#define _SCHED_H
#include <sys/cdefs.h>
#define SCHED_OTHER 0
#define SCHED_FIFO 1
#define SCHED_RR 2
struct sched_param { int sched_priority; };
__BEGIN_DECLS
int sched_yield(void);
int sched_get_priority_min(int);
int sched_get_priority_max(int);
__END_DECLS
#endif
