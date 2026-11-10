/*
 * DOS-XC C library - pthread.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * POSIX threads within one process.  See docs/PROGRAMS.md for the limits:
 * cancellation is deferred and happens only at pthread_testcancel and in
 * condition waits.
 */
#ifndef _PTHREAD_H
#define _PTHREAD_H
#define __NEED_size_t
#define __NEED_clockid_t
#define __NEED_struct_timespec
#define __NEED_sigset_t
#include <sys/_types.h>
#include <sched.h>
#include <time.h>

typedef struct __dosxc_thread *pthread_t;
typedef struct { int __detached, __stack_given; size_t __stack_size; void *__stack; } pthread_attr_t;
typedef struct { volatile int __lock; int __type; volatile pthread_t __owner; int __count; } pthread_mutex_t;
typedef struct { int __type; } pthread_mutexattr_t;
typedef struct { volatile int __sequence; clockid_t __clock; } pthread_cond_t;
typedef struct { clockid_t __clock; } pthread_condattr_t;
typedef struct {
    pthread_mutex_t __mutex;
    pthread_cond_t __readers, __writers;
    int __active_readers, __waiting_writers, __writer;
} pthread_rwlock_t;
typedef struct { int __unused; } pthread_rwlockattr_t;
typedef volatile int pthread_once_t;
typedef unsigned pthread_key_t;
typedef volatile int pthread_spinlock_t;
typedef struct { pthread_mutex_t __mutex; pthread_cond_t __cond; unsigned __count, __waiting, __cycle; } pthread_barrier_t;
typedef struct { int __unused; } pthread_barrierattr_t;

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1
#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_RECURSIVE 1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT PTHREAD_MUTEX_NORMAL
#define PTHREAD_MUTEX_RECURSIVE_NP PTHREAD_MUTEX_RECURSIVE
#define PTHREAD_MUTEX_ERRORCHECK_NP PTHREAD_MUTEX_ERRORCHECK
#define PTHREAD_PROCESS_PRIVATE 0
#define PTHREAD_PROCESS_SHARED 1
#define PTHREAD_SCOPE_SYSTEM 0
#define PTHREAD_SCOPE_PROCESS 1
#define PTHREAD_INHERIT_SCHED 0
#define PTHREAD_EXPLICIT_SCHED 1
#define PTHREAD_CANCEL_ENABLE 0
#define PTHREAD_CANCEL_DISABLE 1
#define PTHREAD_CANCEL_DEFERRED 0
#define PTHREAD_CANCEL_ASYNCHRONOUS 1
#define PTHREAD_CANCELED ((void *)-1)
#define PTHREAD_BARRIER_SERIAL_THREAD (-1)
#define PTHREAD_STACK_MIN 16384
#define PTHREAD_KEYS_MAX 128
#define PTHREAD_DESTRUCTOR_ITERATIONS 4
#define PTHREAD_ONCE_INIT 0
#define PTHREAD_MUTEX_INITIALIZER {0, PTHREAD_MUTEX_NORMAL, 0, 0}
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP {0, PTHREAD_MUTEX_RECURSIVE, 0, 0}
#define PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP {0, PTHREAD_MUTEX_ERRORCHECK, 0, 0}
#define PTHREAD_COND_INITIALIZER {0, 0}
#define PTHREAD_RWLOCK_INITIALIZER {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0}

struct __dosxc_cleanup { void (*__routine)(void *); void *__argument; struct __dosxc_cleanup *__previous; };

__BEGIN_DECLS
void __dosxc_cleanup_push(struct __dosxc_cleanup *);
void __dosxc_cleanup_pop(struct __dosxc_cleanup *, int);
#define pthread_cleanup_push(routine, argument) \
    do { struct __dosxc_cleanup __cleanup = {(routine), (argument), 0}; __dosxc_cleanup_push(&__cleanup);
#define pthread_cleanup_pop(execute) \
    __dosxc_cleanup_pop(&__cleanup, (execute)); } while (0)

int pthread_create(pthread_t *__restrict, const pthread_attr_t *__restrict, void *(*)(void *), void *__restrict);
int pthread_join(pthread_t, void **);
int pthread_detach(pthread_t);
void pthread_exit(void *) __attribute__((__noreturn__));
pthread_t pthread_self(void);
int pthread_equal(pthread_t, pthread_t);
int pthread_kill(pthread_t, int);
int pthread_sigmask(int, const sigset_t *__restrict, sigset_t *__restrict);
int pthread_atfork(void (*)(void), void (*)(void), void (*)(void));
int pthread_setname_np(pthread_t, const char *);
int pthread_getconcurrency(void);
int pthread_setconcurrency(int);

int pthread_attr_init(pthread_attr_t *);
int pthread_attr_destroy(pthread_attr_t *);
int pthread_attr_setdetachstate(pthread_attr_t *, int);
int pthread_attr_getdetachstate(const pthread_attr_t *, int *);
int pthread_attr_setstacksize(pthread_attr_t *, size_t);
int pthread_attr_getstacksize(const pthread_attr_t *__restrict, size_t *__restrict);
int pthread_attr_setstack(pthread_attr_t *, void *, size_t);
int pthread_attr_getstack(const pthread_attr_t *__restrict, void **__restrict, size_t *__restrict);
int pthread_attr_setguardsize(pthread_attr_t *, size_t);
int pthread_attr_getguardsize(const pthread_attr_t *__restrict, size_t *__restrict);
int pthread_attr_setscope(pthread_attr_t *, int);
int pthread_attr_getscope(const pthread_attr_t *__restrict, int *__restrict);
int pthread_attr_setinheritsched(pthread_attr_t *, int);
int pthread_attr_getinheritsched(const pthread_attr_t *__restrict, int *__restrict);
int pthread_attr_setschedpolicy(pthread_attr_t *, int);
int pthread_attr_getschedpolicy(const pthread_attr_t *__restrict, int *__restrict);
int pthread_attr_setschedparam(pthread_attr_t *__restrict, const struct sched_param *__restrict);
int pthread_attr_getschedparam(const pthread_attr_t *__restrict, struct sched_param *__restrict);
int pthread_setschedparam(pthread_t, int, const struct sched_param *);
int pthread_getschedparam(pthread_t, int *__restrict, struct sched_param *__restrict);

int pthread_mutex_init(pthread_mutex_t *__restrict, const pthread_mutexattr_t *__restrict);
int pthread_mutex_destroy(pthread_mutex_t *);
int pthread_mutex_lock(pthread_mutex_t *);
int pthread_mutex_trylock(pthread_mutex_t *);
int pthread_mutex_timedlock(pthread_mutex_t *__restrict, const struct timespec *__restrict);
int pthread_mutex_unlock(pthread_mutex_t *);
int pthread_mutexattr_init(pthread_mutexattr_t *);
int pthread_mutexattr_destroy(pthread_mutexattr_t *);
int pthread_mutexattr_settype(pthread_mutexattr_t *, int);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *__restrict, int *__restrict);
int pthread_mutexattr_setpshared(pthread_mutexattr_t *, int);
int pthread_mutexattr_getpshared(const pthread_mutexattr_t *__restrict, int *__restrict);

int pthread_cond_init(pthread_cond_t *__restrict, const pthread_condattr_t *__restrict);
int pthread_cond_destroy(pthread_cond_t *);
int pthread_cond_wait(pthread_cond_t *__restrict, pthread_mutex_t *__restrict);
int pthread_cond_timedwait(pthread_cond_t *__restrict, pthread_mutex_t *__restrict, const struct timespec *__restrict);
int pthread_cond_signal(pthread_cond_t *);
int pthread_cond_broadcast(pthread_cond_t *);
int pthread_condattr_init(pthread_condattr_t *);
int pthread_condattr_destroy(pthread_condattr_t *);
int pthread_condattr_setclock(pthread_condattr_t *, clockid_t);
int pthread_condattr_getclock(const pthread_condattr_t *__restrict, clockid_t *__restrict);
int pthread_condattr_setpshared(pthread_condattr_t *, int);
int pthread_condattr_getpshared(const pthread_condattr_t *__restrict, int *__restrict);

int pthread_rwlock_init(pthread_rwlock_t *__restrict, const pthread_rwlockattr_t *__restrict);
int pthread_rwlock_destroy(pthread_rwlock_t *);
int pthread_rwlock_rdlock(pthread_rwlock_t *);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *);
int pthread_rwlock_timedrdlock(pthread_rwlock_t *__restrict, const struct timespec *__restrict);
int pthread_rwlock_wrlock(pthread_rwlock_t *);
int pthread_rwlock_trywrlock(pthread_rwlock_t *);
int pthread_rwlock_timedwrlock(pthread_rwlock_t *__restrict, const struct timespec *__restrict);
int pthread_rwlock_unlock(pthread_rwlock_t *);
int pthread_rwlockattr_init(pthread_rwlockattr_t *);
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *);

int pthread_once(pthread_once_t *, void (*)(void));
int pthread_key_create(pthread_key_t *, void (*)(void *));
int pthread_key_delete(pthread_key_t);
void *pthread_getspecific(pthread_key_t);
int pthread_setspecific(pthread_key_t, const void *);

int pthread_spin_init(pthread_spinlock_t *, int);
int pthread_spin_destroy(pthread_spinlock_t *);
int pthread_spin_lock(pthread_spinlock_t *);
int pthread_spin_trylock(pthread_spinlock_t *);
int pthread_spin_unlock(pthread_spinlock_t *);

int pthread_barrier_init(pthread_barrier_t *__restrict, const pthread_barrierattr_t *__restrict, unsigned);
int pthread_barrier_destroy(pthread_barrier_t *);
int pthread_barrier_wait(pthread_barrier_t *);
int pthread_barrierattr_init(pthread_barrierattr_t *);
int pthread_barrierattr_destroy(pthread_barrierattr_t *);

int pthread_cancel(pthread_t);
int pthread_setcancelstate(int, int *);
int pthread_setcanceltype(int, int *);
void pthread_testcancel(void);
__END_DECLS
#endif
