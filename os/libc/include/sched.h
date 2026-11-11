/*
 * DOS-XC /os/libc/include/sched.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _SCHED_H
#define _SCHED_H
#include <sys/cdefs.h>
#define SCHED_OTHER 0
#define SCHED_FIFO 1
#define SCHED_RR 2
struct sched_param { int sched_priority; };
#include <stddef.h>
typedef struct { unsigned long __bits[16]; } cpu_set_t;
#define CPU_SETSIZE 1024
#define CPU_ZERO(set) __builtin_memset((set), 0, sizeof(cpu_set_t))
#define CPU_SET(cpu, set) ((set)->__bits[(cpu) / 64] |= 1ul << ((cpu) % 64))
#define CPU_CLR(cpu, set) ((set)->__bits[(cpu) / 64] &= ~(1ul << ((cpu) % 64)))
#define CPU_ISSET(cpu, set) (((set)->__bits[(cpu) / 64] >> ((cpu) % 64)) & 1)
#define CPU_COUNT(set) __sched_cpucount(sizeof(cpu_set_t), (set))
__BEGIN_DECLS
int sched_yield(void);
int sched_getcpu(void);
int sched_get_priority_min(int);
int sched_get_priority_max(int);
int sched_getaffinity(int pid, size_t size, void *set);   /* one processor */
int sched_setaffinity(int pid, size_t size, const void *set);
int __sched_cpucount(size_t size, const cpu_set_t *set);
__END_DECLS
#endif
