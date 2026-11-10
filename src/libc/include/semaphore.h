/*
 * DOS-XC C library - semaphore.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * Semaphores within one process (sem_init).
 */
#ifndef _SEMAPHORE_H
#define _SEMAPHORE_H
#define __NEED_struct_timespec
#include <sys/_types.h>
typedef struct { volatile int __value; } sem_t;
#define SEM_FAILED ((sem_t *)0)
__BEGIN_DECLS
int sem_init(sem_t *, int, unsigned);
int sem_destroy(sem_t *);
int sem_wait(sem_t *);
int sem_trywait(sem_t *);
int sem_timedwait(sem_t *__restrict, const struct timespec *__restrict);
int sem_post(sem_t *);
int sem_getvalue(sem_t *__restrict, int *__restrict);
__END_DECLS
#endif
