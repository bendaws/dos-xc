/*
 * DOS-XC /os/libc/src/pthread.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "libc.h"
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_STACK (2u * 1024 * 1024)
#define KEY_MAX PTHREAD_KEYS_MAX

struct __dosxc_thread {
    void *(*start)(void *);
    void *argument, *result;
    int error;
    int fault_nesting;               /* fault handlers running on this thread */
    volatile int alive;              /* the kernel zeroes it once the stack is unused */
    volatile int detach_state;       /* 0 joinable, 1 detached, 2 finished */
    volatile int cancel_pending;
    int cancel_disabled;
    char *stack;                     /* owned by the library unless it was given */
    struct __dosxc_cleanup *cleanup;
    void *specific[KEY_MAX];
    void **tls;
    size_t tls_count;
    size_t stack_size;
    char *stack_base;                /* when the caller gave the stack */
    void *tp, *tp_block;             /* native thread-local data; see __tls_get_addr */
    void **dtv;
    size_t dtv_count;
    struct __dosxc_thread *next_zombie;
};

int __dosxc_threaded;
static volatile uintptr_t *thread_slot;
static struct __dosxc_thread main_thread;
static struct __dosxc_thread *volatile zombies;
static volatile int running_threads;

static inline struct __dosxc_thread *self(void) {
    uintptr_t value = thread_slot ? *thread_slot : 0;
    return value ? (struct __dosxc_thread *)value : &main_thread;
}

static int futex_wait(volatile int *address, int expected, long nanoseconds) {
    return (int)dosxc_sys_task(DOSXC_TASK_FUTEX_WAIT, (void *)address, expected, nanoseconds);
}

static void futex_wake(volatile int *address, long count) {
    dosxc_sys_task(DOSXC_TASK_FUTEX_WAKE, (void *)address, count, 0);
}

/* Nanoseconds until ABSOLUTE, or 0 when it has passed. */
static long long remaining(const struct timespec *absolute, clockid_t clock) {
    struct timespec now;
    clock_gettime(clock, &now);
    long long ns = ((long long)absolute->tv_sec - (long long)now.tv_sec) * 1000000000LL + (absolute->tv_nsec - now.tv_nsec);
    return ns > 0 ? ns : 0;
}

/* Wait while *ADDRESS is EXPECTED: 0 when woken, ETIMEDOUT when ABSOLUTE passes. */
static int timed_wait(volatile int *address, int expected, const struct timespec *absolute, clockid_t clock) {
    if (!absolute) {
        futex_wait(address, expected, 0);
        return 0;
    }
    long long ns = remaining(absolute, clock);
    if (!ns) return ETIMEDOUT;
    long chunk = ns > 1000000000LL ? 1000000000L : (long)ns;
    int r = futex_wait(address, expected, chunk);
    if (r == -DOSXC_ETIMEDOUT) return remaining(absolute, clock) ? 0 : ETIMEDOUT;
    return 0;
}

/* ---- The lock everything else is built on: 0 free, 1 held, 2 held with waiters ---- */
static inline int word_try(volatile int *word) {
    int expected = 0;
    return __atomic_compare_exchange_n(word, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static int word_lock(volatile int *word, const struct timespec *absolute) {
    if (word_try(word)) return 0;
    while (__atomic_exchange_n(word, 2, __ATOMIC_ACQUIRE) != 0) {
        int r = timed_wait(word, 2, absolute, CLOCK_REALTIME);
        if (r) return r;
    }
    return 0;
}

static inline void word_unlock(volatile int *word) {
    if (__atomic_exchange_n(word, 0, __ATOMIC_RELEASE) == 2) futex_wake(word, 1);
}

void __dosxc_rlock_acquire(struct __dosxc_rlock *lock) {
    void *me = self();
    if (lock->owner == me) {
        ++lock->count;
        return;
    }
    word_lock(&lock->state, 0);
    lock->owner = me;
    lock->count = 1;
}

void __dosxc_rlock_release(struct __dosxc_rlock *lock) {
    if (--lock->count) return;
    lock->owner = 0;
    word_unlock(&lock->state);
}

#ifdef __x86_64__
#define TCB_SIZE 64
#define MAX_MODULES (DOSXC_LIBRARIES_MAX + 1)
static struct dosxc_tls_module modules[MAX_MODULES];
static unsigned module_count;
static size_t static_size, static_align = 64;
static struct __dosxc_rlock modules_lock;
extern uintptr_t __stack_chk_guard;

static void read_modules(void) {
    long total = dosxc_sys_library(DOSXC_LIB_TLS, (void *)0, &modules[0]);
    if (total > MAX_MODULES) total = MAX_MODULES;
    for (long i = 1; i < total; ++i) dosxc_sys_library(DOSXC_LIB_TLS, (void *)i, &modules[i]);
    module_count = total > 0 ? (unsigned)total : 0;
}

static int make_thread_pointer(struct __dosxc_thread *t) {
    if (!static_size) return 0;
    char *block = malloc(static_size + static_align + TCB_SIZE);
    if (!block) return -1;
    memset(block, 0, static_size + static_align + TCB_SIZE);
    char *tp = (char *)(((uintptr_t)block + static_size + static_align - 1) & ~(uintptr_t)(static_align - 1));
    for (unsigned i = 0; i < module_count; ++i)
        if (modules[i].offset) memcpy(tp - modules[i].offset, (void *)modules[i].image, modules[i].filesz);
    *(void **)tp = tp;
    ((uintptr_t *)tp)[5] = __stack_chk_guard;
    t->tp = tp;
    t->tp_block = block;
    return 0;
}

hidden void __dosxc_tls_init(void) {
    read_modules();
    for (unsigned i = 0; i < module_count; ++i) {
        if (modules[i].align > static_align) static_align = modules[i].align;
        if (modules[i].offset > static_size) static_size = modules[i].offset;
    }
    if (make_thread_pointer(&main_thread) == 0 && main_thread.tp)
        dosxc_sys_task(DOSXC_TASK_SET_TP, main_thread.tp, 0, 0);
}

struct tls_index { size_t module, offset; };

void *__tls_get_addr(struct tls_index *index);
void *__tls_get_addr(struct tls_index *index) {
    struct __dosxc_thread *t = self();
    size_t n = index->module;
    if (n > module_count) {
        LOCK_GUARD(&modules_lock);
        read_modules();
    }
    if (!n || n > module_count) abort();
    const struct dosxc_tls_module *m = &modules[n - 1];
    if (m->offset) return (char *)t->tp - m->offset + index->offset;
    if (n > t->dtv_count) {
        void **grown = realloc(t->dtv, n * sizeof(void *));
        if (!grown) abort();
        memset(grown + t->dtv_count, 0, (n - t->dtv_count) * sizeof(void *));
        t->dtv = grown;
        t->dtv_count = n;
    }
    if (!t->dtv[n - 1]) {
        size_t align = m->align < sizeof(void *) ? sizeof(void *) : m->align;
        char *block = malloc(m->memsz + align + sizeof(void *));
        if (!block) abort();
        char *data = (char *)(((uintptr_t)block + sizeof(void *) + align - 1) & ~(uintptr_t)(align - 1));
        ((void **)data)[-1] = block;
        memset(data, 0, m->memsz);
        memcpy(data, (void *)m->image, m->filesz);
        t->dtv[n - 1] = data;
    }
    return (char *)t->dtv[n - 1] + index->offset;
}
#endif

static void free_thread(struct __dosxc_thread *t) {
    free(t->tp_block);
    free(t->stack);
    free(t);
}

static void reap_zombies(void) {
    struct __dosxc_thread *list = __atomic_exchange_n(&zombies, (struct __dosxc_thread *)0, __ATOMIC_ACQUIRE);
    while (list) {
        struct __dosxc_thread *next = list->next_zombie;
        if (list->alive) {
            struct __dosxc_thread *head = zombies;
            do list->next_zombie = head;
            while (!__atomic_compare_exchange_n(&zombies, &head, list, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
        } else {
            free_thread(list);
        }
        list = next;
    }
}

__attribute__((used, visibility("hidden"), noreturn)) void __dosxc_thread_main(struct __dosxc_thread *t);
void __dosxc_thread_main(struct __dosxc_thread *t) {
    if (t->tp) dosxc_sys_task(DOSXC_TASK_SET_TP, t->tp, 0, 0);
    pthread_exit(t->start(t->argument)); }

#if defined(__x86_64__)
__asm__(".text\n"
        ".type __dosxc_thread_entry,@function\n"
        "__dosxc_thread_entry:\n"
        "    pop %rdi\n"
        "    and $-16, %rsp\n"
        "    call __dosxc_thread_main\n"
        "    hlt\n");
#else
__asm__(".text\n"
        ".type __dosxc_thread_entry,@function\n"
        "__dosxc_thread_entry:\n"
        "    pop %eax\n"
        "    and $-16, %esp\n"
        "    sub $12, %esp\n"
        "    push %eax\n"
        "    call __dosxc_thread_main\n"
        "    hlt\n");
#endif
extern void __dosxc_thread_entry(void) __attribute__((visibility("hidden")));

int pthread_create(pthread_t *out, const pthread_attr_t *attr, void *(*start)(void *), void *argument) {
    if (!thread_slot) thread_slot = (volatile uintptr_t *)dosxc_sys_task(DOSXC_TASK_THREAD_SLOT, 0, 0, 0);
    __dosxc_threaded = 1;
    reap_zombies();

    struct __dosxc_thread *t = calloc(1, sizeof(*t));
    if (!t) return EAGAIN;
    char *stack;
    size_t size = attr && attr->__stack_size ? attr->__stack_size : DEFAULT_STACK;
    if (attr && attr->__stack_given) {
        stack = attr->__stack;
    } else {
        stack = malloc(size);
        if (!stack) {
            free(t);
            return EAGAIN;
        }
        t->stack = stack;
    }
    t->stack_size = size;
    if (attr && attr->__stack_given) t->stack_base = stack;
#ifdef __x86_64__
    if (make_thread_pointer(t)) {
        free_thread(t);
        return EAGAIN;
    }
#endif
    t->start = start;
    t->argument = argument;
    t->alive = 1;
    t->detach_state = attr ? attr->__detached : 0;

    uintptr_t *sp = (uintptr_t *)(((uintptr_t)stack + size) & ~(uintptr_t)15);
    *--sp = (uintptr_t)t;
    __atomic_add_fetch(&running_threads, 1, __ATOMIC_RELAXED);
    *out = t;
    long id = dosxc_sys_task(DOSXC_TASK_THREAD_CREATE, (void *)__dosxc_thread_entry, (long)(uintptr_t)sp, (long)(uintptr_t)t);
    if (id < 0) {
        __atomic_sub_fetch(&running_threads, 1, __ATOMIC_RELAXED);
        free_thread(t);
        return (int)-id;
    }
    return 0;
}

static void run_cleanups(struct __dosxc_thread *t);
static void run_specific_destructors(struct __dosxc_thread *t);
static void free_tls(struct __dosxc_thread *t);

void pthread_exit(void *result) {
    struct __dosxc_thread *t = self();
    run_cleanups(t);
    run_specific_destructors(t);
    if (t == &main_thread) {
        for (int n; (n = running_threads) != 0;) futex_wait(&running_threads, n, 0);
        exit(0);
    }
    free_tls(t);
    t->result = result;
    if (__atomic_exchange_n(&t->detach_state, 2, __ATOMIC_ACQ_REL) == 1) {
        struct __dosxc_thread *head = zombies;
        do t->next_zombie = head;
        while (!__atomic_compare_exchange_n(&zombies, &head, t, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    }
    if (__atomic_sub_fetch(&running_threads, 1, __ATOMIC_ACQ_REL) == 0) futex_wake(&running_threads, INT_MAX);
    dosxc_sys_task(DOSXC_TASK_THREAD_EXIT, (void *)&t->alive, 0, 0);
    __builtin_unreachable();
}

int pthread_join(pthread_t t, void **result) {
    if (t == self()) return EDEADLK;
    if (t->detach_state == 1) return EINVAL;
    for (int alive; (alive = t->alive) != 0;) {
        futex_wait(&t->alive, alive, 0);
        pthread_testcancel();
    }
    if (result) *result = t->result;
    free_thread(t);
    return 0;
}

int pthread_detach(pthread_t t) {
    int previous = __atomic_exchange_n(&t->detach_state, 1, __ATOMIC_ACQ_REL);
    if (previous == 1) return EINVAL;
    if (previous == 2) {
        for (int alive; (alive = t->alive) != 0;) futex_wait(&t->alive, alive, 0);
        free_thread(t);
    }
    return 0;
}

pthread_t pthread_self(void) { return self(); }
int pthread_equal(pthread_t a, pthread_t b) { return a == b; }
int pthread_kill(pthread_t t, int signal) { (void)t; return signal ? ENOSYS : 0; }
int pthread_sigmask(int how, const sigset_t *set, sigset_t *old) { (void)how; (void)set; if (old) *old = 0; return 0; }
int pthread_atfork(void (*a)(void), void (*b)(void), void (*c)(void)) { (void)a; (void)b; (void)c; return 0; }
int pthread_setname_np(pthread_t t, const char *name) { (void)t; (void)name; return 0; }
int pthread_getconcurrency(void) { return 0; }
int pthread_setconcurrency(int n) { (void)n; return 0; }

int sched_yield(void) { dosxc_sys_task(DOSXC_TASK_YIELD, 0, 0, 0); return 0; }
int sched_get_priority_min(int policy) { (void)policy; return 0; }
int sched_get_priority_max(int policy) { (void)policy; return 0; }

/* ---- Attributes ---- */
int pthread_attr_init(pthread_attr_t *a) { memset(a, 0, sizeof(*a)); return 0; }
int pthread_attr_destroy(pthread_attr_t *a) { (void)a; return 0; }
int pthread_attr_setdetachstate(pthread_attr_t *a, int s) { a->__detached = s; return 0; }
int pthread_attr_getdetachstate(const pthread_attr_t *a, int *s) { *s = a->__detached; return 0; }
int pthread_attr_setstacksize(pthread_attr_t *a, size_t n) {
    if (n < PTHREAD_STACK_MIN) return EINVAL;
    a->__stack_size = n;
    return 0;
}
int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *n) { *n = a->__stack_size ? a->__stack_size : DEFAULT_STACK; return 0; }
int pthread_attr_setstack(pthread_attr_t *a, void *s, size_t n) {
    if (n < PTHREAD_STACK_MIN) return EINVAL;
    a->__stack = s;
    a->__stack_size = n;
    a->__stack_given = 1;
    return 0;
}
int pthread_attr_getstack(const pthread_attr_t *a, void **s, size_t *n) { *s = a->__stack; *n = a->__stack_size; return 0; }
int pthread_attr_setguardsize(pthread_attr_t *a, size_t n) { (void)a; (void)n; return 0; }
int pthread_attr_getguardsize(const pthread_attr_t *a, size_t *n) { (void)a; *n = 0; return 0; }
int pthread_attr_setscope(pthread_attr_t *a, int s) { (void)a; return s == PTHREAD_SCOPE_SYSTEM ? 0 : ENOTSUP; }
int pthread_attr_getscope(const pthread_attr_t *a, int *s) { (void)a; *s = PTHREAD_SCOPE_SYSTEM; return 0; }
int pthread_attr_setinheritsched(pthread_attr_t *a, int s) { (void)a; (void)s; return 0; }
int pthread_attr_getinheritsched(const pthread_attr_t *a, int *s) { (void)a; *s = PTHREAD_INHERIT_SCHED; return 0; }
int pthread_attr_setschedpolicy(pthread_attr_t *a, int p) { (void)a; (void)p; return 0; }
int pthread_attr_getschedpolicy(const pthread_attr_t *a, int *p) { (void)a; *p = SCHED_OTHER; return 0; }
int pthread_attr_setschedparam(pthread_attr_t *a, const struct sched_param *p) { (void)a; (void)p; return 0; }
int pthread_attr_getschedparam(const pthread_attr_t *a, struct sched_param *p) { (void)a; p->sched_priority = 0; return 0; }
int pthread_setschedparam(pthread_t t, int policy, const struct sched_param *p) { (void)t; (void)policy; (void)p; return 0; }
int pthread_getschedparam(pthread_t t, int *policy, struct sched_param *p) { (void)t; *policy = SCHED_OTHER; p->sched_priority = 0; return 0; }

/* ---- Mutexes ---- */
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
    memset((void *)m, 0, sizeof(*m));
    m->__type = a ? a->__type : PTHREAD_MUTEX_NORMAL;
    return 0;
}
int pthread_mutex_destroy(pthread_mutex_t *m) { return m->__lock ? EBUSY : 0; }

static int mutex_acquire(pthread_mutex_t *m, const struct timespec *absolute, int try_only) {
    pthread_t me = self();
    if (m->__type != PTHREAD_MUTEX_NORMAL && m->__owner == me) {
        if (m->__type == PTHREAD_MUTEX_ERRORCHECK) return EDEADLK;
        ++m->__count;
        return 0;
    }
    if (try_only) {
        if (!word_try(&m->__lock)) return EBUSY;
    } else {
        int r = word_lock(&m->__lock, absolute);
        if (r) return r;
    }
    m->__owner = me;
    m->__count = 1;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m) { return mutex_acquire(m, 0, 0); }
int pthread_mutex_trylock(pthread_mutex_t *m) { return mutex_acquire(m, 0, 1); }
int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *t) { return mutex_acquire(m, t, 0); }

int pthread_mutex_unlock(pthread_mutex_t *m) {
    if (m->__type != PTHREAD_MUTEX_NORMAL && m->__owner != self()) return EPERM;
    if (m->__type == PTHREAD_MUTEX_RECURSIVE && --m->__count) return 0;
    m->__owner = 0;
    word_unlock(&m->__lock);
    return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t *a) { a->__type = PTHREAD_MUTEX_NORMAL; return 0; }
int pthread_mutexattr_destroy(pthread_mutexattr_t *a) { (void)a; return 0; }
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int t) {
    if (t < 0 || t > PTHREAD_MUTEX_ERRORCHECK) return EINVAL;
    a->__type = t;
    return 0;
}
int pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *t) { *t = a->__type; return 0; }
int pthread_mutexattr_setpshared(pthread_mutexattr_t *a, int s) { (void)a; return s == PTHREAD_PROCESS_PRIVATE ? 0 : ENOTSUP; }
int pthread_mutexattr_getpshared(const pthread_mutexattr_t *a, int *s) { (void)a; *s = PTHREAD_PROCESS_PRIVATE; return 0; }

/* ---- Condition variables ---- */
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) {
    c->__sequence = 0;
    c->__clock = a ? a->__clock : CLOCK_REALTIME;
    return 0;
}
int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }

static int condition_wait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *absolute) {
    int sequence = c->__sequence;
    int count = m->__count;
    m->__owner = 0;
    word_unlock(&m->__lock);
    int r = timed_wait(&c->__sequence, sequence, absolute, c->__clock);
    while (__atomic_exchange_n(&m->__lock, 2, __ATOMIC_ACQUIRE) != 0) futex_wait(&m->__lock, 2, 0);
    m->__owner = self();
    m->__count = count;
    return r;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    condition_wait(c, m, 0);
    pthread_testcancel();
    return 0;
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *absolute) {
    int r = condition_wait(c, m, absolute);
    pthread_testcancel();
    return r;
}

int pthread_cond_signal(pthread_cond_t *c) {
    __atomic_add_fetch(&c->__sequence, 1, __ATOMIC_RELEASE);
    futex_wake(&c->__sequence, 1);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c) {
    __atomic_add_fetch(&c->__sequence, 1, __ATOMIC_RELEASE);
    futex_wake(&c->__sequence, INT_MAX);
    return 0;
}

int pthread_condattr_init(pthread_condattr_t *a) { a->__clock = CLOCK_REALTIME; return 0; }
int pthread_condattr_destroy(pthread_condattr_t *a) { (void)a; return 0; }
int pthread_condattr_setclock(pthread_condattr_t *a, clockid_t c) { a->__clock = c; return 0; }
int pthread_condattr_getclock(const pthread_condattr_t *a, clockid_t *c) { *c = a->__clock; return 0; }
int pthread_condattr_setpshared(pthread_condattr_t *a, int s) { (void)a; return s == PTHREAD_PROCESS_PRIVATE ? 0 : ENOTSUP; }
int pthread_condattr_getpshared(const pthread_condattr_t *a, int *s) { (void)a; *s = PTHREAD_PROCESS_PRIVATE; return 0; }

/* ---- Read-write locks ---- */
int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *a) {
    (void)a;
    memset((void *)l, 0, sizeof(*l));
    return 0;
}
int pthread_rwlock_destroy(pthread_rwlock_t *l) { (void)l; return 0; }

static int rw_read(pthread_rwlock_t *l, const struct timespec *absolute, int try_only) {
    pthread_mutex_lock(&l->__mutex);
    int r = 0;
    while (!r && (l->__writer || l->__waiting_writers)) {
        if (try_only) r = EBUSY;
        else r = condition_wait(&l->__readers, &l->__mutex, absolute);
    }
    if (!r) ++l->__active_readers;
    pthread_mutex_unlock(&l->__mutex);
    return r;
}

static int rw_write(pthread_rwlock_t *l, const struct timespec *absolute, int try_only) {
    pthread_mutex_lock(&l->__mutex);
    int r = 0;
    ++l->__waiting_writers;
    while (!r && (l->__writer || l->__active_readers)) {
        if (try_only) r = EBUSY;
        else r = condition_wait(&l->__writers, &l->__mutex, absolute);
    }
    --l->__waiting_writers;
    if (!r) l->__writer = 1;
    else if (!l->__waiting_writers) pthread_cond_broadcast(&l->__readers);
    pthread_mutex_unlock(&l->__mutex);
    return r;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *l) { return rw_read(l, 0, 0); }
int pthread_rwlock_tryrdlock(pthread_rwlock_t *l) { return rw_read(l, 0, 1); }
int pthread_rwlock_timedrdlock(pthread_rwlock_t *l, const struct timespec *t) { return rw_read(l, t, 0); }
int pthread_rwlock_wrlock(pthread_rwlock_t *l) { return rw_write(l, 0, 0); }
int pthread_rwlock_trywrlock(pthread_rwlock_t *l) { return rw_write(l, 0, 1); }
int pthread_rwlock_timedwrlock(pthread_rwlock_t *l, const struct timespec *t) { return rw_write(l, t, 0); }

int pthread_rwlock_unlock(pthread_rwlock_t *l) {
    pthread_mutex_lock(&l->__mutex);
    if (l->__writer) l->__writer = 0;
    else if (l->__active_readers) --l->__active_readers;
    if (l->__waiting_writers) {
        if (!l->__active_readers) pthread_cond_signal(&l->__writers);
    } else {
        pthread_cond_broadcast(&l->__readers);
    }
    pthread_mutex_unlock(&l->__mutex);
    return 0;
}
int pthread_rwlockattr_init(pthread_rwlockattr_t *a) { (void)a; return 0; }
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *a) { (void)a; return 0; }

/* ---- Once, spin locks, barriers ---- */
int pthread_once(pthread_once_t *once, void (*routine)(void)) {
    for (;;) {
        int state = 0;
        if (__atomic_compare_exchange_n(once, &state, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
            routine();
            __atomic_store_n(once, 2, __ATOMIC_RELEASE);
            futex_wake(once, INT_MAX);
            return 0;
        }
        if (state == 2) return 0;
        futex_wait(once, 1, 0);
    }
}

int pthread_spin_init(pthread_spinlock_t *l, int s) { (void)s; *l = 0; return 0; }
int pthread_spin_destroy(pthread_spinlock_t *l) { (void)l; return 0; }
int pthread_spin_trylock(pthread_spinlock_t *l) { return word_try(l) ? 0 : EBUSY; }
int pthread_spin_lock(pthread_spinlock_t *l) {
    while (!word_try(l)) sched_yield();
    return 0;
}
int pthread_spin_unlock(pthread_spinlock_t *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); return 0; }

int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *a, unsigned count) {
    (void)a;
    if (!count) return EINVAL;
    memset((void *)b, 0, sizeof(*b));
    b->__count = count;
    return 0;
}
int pthread_barrier_destroy(pthread_barrier_t *b) { (void)b; return 0; }
int pthread_barrier_wait(pthread_barrier_t *b) {
    pthread_mutex_lock(&b->__mutex);
    unsigned cycle = b->__cycle;
    if (++b->__waiting == b->__count) {
        b->__waiting = 0;
        ++b->__cycle;
        pthread_cond_broadcast(&b->__cond);
        pthread_mutex_unlock(&b->__mutex);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    while (cycle == b->__cycle) condition_wait(&b->__cond, &b->__mutex, 0);
    pthread_mutex_unlock(&b->__mutex);
    return 0;
}
int pthread_barrierattr_init(pthread_barrierattr_t *a) { (void)a; return 0; }
int pthread_barrierattr_destroy(pthread_barrierattr_t *a) { (void)a; return 0; }

/* ---- Cancellation (deferred) and cleanup handlers ---- */
int pthread_cancel(pthread_t t) { t->cancel_pending = 1; return 0; }
void pthread_testcancel(void) {
    struct __dosxc_thread *t = self();
    if (t->cancel_pending && !t->cancel_disabled) {
        t->cancel_disabled = 1;
        pthread_exit(PTHREAD_CANCELED);
    }
}
int pthread_setcancelstate(int state, int *old) {
    struct __dosxc_thread *t = self();
    if (old) *old = t->cancel_disabled ? PTHREAD_CANCEL_DISABLE : PTHREAD_CANCEL_ENABLE;
    t->cancel_disabled = state == PTHREAD_CANCEL_DISABLE;
    return 0;
}
int pthread_setcanceltype(int type, int *old) { (void)type; if (old) *old = PTHREAD_CANCEL_DEFERRED; return 0; }

void __dosxc_cleanup_push(struct __dosxc_cleanup *c) {
    struct __dosxc_thread *t = self();
    c->__previous = t->cleanup;
    t->cleanup = c;
}
void __dosxc_cleanup_pop(struct __dosxc_cleanup *c, int execute) {
    self()->cleanup = c->__previous;
    if (execute) c->__routine(c->__argument);
}
static void run_cleanups(struct __dosxc_thread *t) {
    while (t->cleanup) {
        struct __dosxc_cleanup *c = t->cleanup;
        t->cleanup = c->__previous;
        c->__routine(c->__argument);
    }
}

static struct { int used; void (*destructor)(void *); } keys[KEY_MAX];
static int key_lock;

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    word_lock(&key_lock, 0);
    for (unsigned i = 0; i < KEY_MAX; ++i)
        if (!keys[i].used) {
            keys[i].used = 1;
            keys[i].destructor = destructor;
            word_unlock(&key_lock);
            self()->specific[i] = 0;
            *key = i;
            return 0;
        }
    word_unlock(&key_lock);
    return EAGAIN;
}

int pthread_key_delete(pthread_key_t key) {
    if (key >= KEY_MAX || !keys[key].used) return EINVAL;
    keys[key].used = 0;
    keys[key].destructor = 0;
    return 0;
}

void *pthread_getspecific(pthread_key_t key) { return key < KEY_MAX ? self()->specific[key] : 0; }

int pthread_setspecific(pthread_key_t key, const void *value) {
    if (key >= KEY_MAX || !keys[key].used) return EINVAL;
    self()->specific[key] = (void *)value;
    return 0;
}

static void run_specific_destructors(struct __dosxc_thread *t) {
    for (int round = 0; round < PTHREAD_DESTRUCTOR_ITERATIONS; ++round) {
        int ran = 0;
        for (unsigned i = 0; i < KEY_MAX; ++i) {
            void *value = t->specific[i];
            if (value && keys[i].destructor) {
                t->specific[i] = 0;
                keys[i].destructor(value);
                ran = 1;
            }
        }
        if (!ran) break;
    }
}

/* ---- __thread variables: GCC's emulated TLS ---- */
struct __emutls_object {
    size_t size, align;
    union { uintptr_t offset; void *pointer; } location;
    void *initial;
};
static uintptr_t tls_next_offset;
static int tls_lock;

void *__emutls_get_address(struct __emutls_object *object) {
    uintptr_t offset = __atomic_load_n(&object->location.offset, __ATOMIC_ACQUIRE);
    if (!offset) {
        word_lock(&tls_lock, 0);
        offset = object->location.offset;
        if (!offset) {
            offset = ++tls_next_offset;
            __atomic_store_n(&object->location.offset, offset, __ATOMIC_RELEASE);
        }
        word_unlock(&tls_lock);
    }
    struct __dosxc_thread *t = self();
    if (offset > t->tls_count) {
        size_t count = offset + 16;
        void **grown = realloc(t->tls, count * sizeof(void *));
        if (!grown) abort();
        memset(grown + t->tls_count, 0, (count - t->tls_count) * sizeof(void *));
        t->tls = grown;
        t->tls_count = count;
    }
    void *value = t->tls[offset - 1];
    if (!value) {
        size_t align = object->align < sizeof(void *) ? sizeof(void *) : object->align;
        char *block = malloc(object->size + align + sizeof(void *));
        if (!block) abort();
        uintptr_t aligned = ((uintptr_t)block + sizeof(void *) + align - 1) & ~(uintptr_t)(align - 1);
        ((void **)aligned)[-1] = block;
        value = (void *)aligned;
        if (object->initial) memcpy(value, object->initial, object->size);
        else memset(value, 0, object->size);
        t->tls[offset - 1] = value;
    }
    return value;
}

void __emutls_register_common(struct __emutls_object *object, size_t size, size_t align, void *initial) {
    if (object->size < size) {
        object->size = size;
        object->initial = 0;
    }
    if (object->align < align) object->align = align;
    if (initial && size == object->size) object->initial = initial;
}

static void free_tls(struct __dosxc_thread *t) {
    for (size_t i = 0; i < t->dtv_count; ++i)
        if (t->dtv[i]) free(((void **)t->dtv[i])[-1]);
    free(t->dtv);
    t->dtv = 0;
    t->dtv_count = 0;
    for (size_t i = 0; i < t->tls_count; ++i)
        if (t->tls[i]) free(((void **)t->tls[i])[-1]);
    free(t->tls);
    t->tls = 0;
    t->tls_count = 0;
}

/* Where a thread's stack is.  There is no guard page. */
int pthread_getattr_np(pthread_t thread, pthread_attr_t *attr) {
    extern uintptr_t __dosxc_stack_base, __dosxc_stack_size;
    struct __dosxc_thread *t = thread;
    memset(attr, 0, sizeof(*attr));
    attr->__stack_given = 1;
    attr->__detached = t != &main_thread && t->detach_state == 1;
    if (t == &main_thread) {
        attr->__stack = (void *)__dosxc_stack_base;
        attr->__stack_size = __dosxc_stack_size;
    } else {
        attr->__stack = t->stack ? t->stack : t->stack_base;
        attr->__stack_size = t->stack_size;
    }
    return 0;
}

/* ---- Semaphores ---- */
int sem_init(sem_t *s, int shared, unsigned value) {
    if (shared) { errno = ENOSYS; return -1; }
    s->__value = (int)value;
    return 0;
}
int sem_destroy(sem_t *s) { (void)s; return 0; }
int sem_trywait(sem_t *s) {
    int value = s->__value;
    while (value > 0)
        if (__atomic_compare_exchange_n(&s->__value, &value, value - 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
    errno = EAGAIN;
    return -1;
}
int sem_timedwait(sem_t *s, const struct timespec *absolute) {
    for (;;) {
        if (!sem_trywait(s)) return 0;
        int r = timed_wait(&s->__value, 0, absolute, CLOCK_REALTIME);
        if (r) { errno = r; return -1; }
    }
}
int sem_wait(sem_t *s) { return sem_timedwait(s, 0); }
int sem_post(sem_t *s) {
    __atomic_add_fetch(&s->__value, 1, __ATOMIC_RELEASE);
    futex_wake(&s->__value, 1);
    return 0;
}
int sem_getvalue(sem_t *s, int *value) { *value = s->__value; return 0; }

#undef errno
int errno;

int *__dosxc_fault_nesting(void) { return &self()->fault_nesting; }

int *__errno_location(void) {
    struct __dosxc_thread *t = self();
    return t == &main_thread ? &errno : &t->error;
}
