/*
 * DOS-XC /tests/programs/smptest.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include <dosxc/syscall.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#define THREADS 4
#define ROUNDS 100000

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long locked_total;
static long atomic_total;
static pthread_t selves[THREADS];
static __thread long mine;
static int failures;

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static unsigned long spin(unsigned long n) {
    volatile unsigned long x = 1;
    for (unsigned long i = 0; i < n; ++i) x = x * 6364136223846793005ul + 1442695040888963407ul;
    return x;
}

static void *work(void *argument) {
    long index = (long)argument;
    selves[index] = pthread_self();
    for (int i = 0; i < ROUNDS; ++i) {
        pthread_mutex_lock(&lock);
        ++locked_total;
        pthread_mutex_unlock(&lock);
        __atomic_fetch_add(&atomic_total, 1, __ATOMIC_RELAXED);
        ++mine;
    }
    spin(200000000ul);
    return (void *)mine;
}

static void check(int ok, const char *what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++failures;
}

int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    struct dosxc_task_stats stats = {0};
    dosxc_sys_task(DOSXC_TASK_STATS, &stats, 0, 0);
    printf("processors: %u\n", stats.cpus ? stats.cpus : 1);

    double start = now();
    spin(200000000ul);
    double one = now() - start;

    pthread_t threads[THREADS];
    start = now();
    for (long i = 0; i < THREADS; ++i) pthread_create(&threads[i], 0, work, (void *)i);
    int tls_ok = 1;
    for (int i = 0; i < THREADS; ++i) {
        void *result;
        pthread_join(threads[i], &result);
        tls_ok &= (long)result == ROUNDS;
    }
    double all = now() - start;

    dosxc_sys_task(DOSXC_TASK_STATS, &stats, 0, 0);
    printf("processors: %u\n", stats.cpus ? stats.cpus : 1);
    printf("1 thread: %.2f s, %d threads: %.2f s, speedup %.2f\n", one, THREADS, all, one * THREADS / all);
    check(locked_total == (long)THREADS * ROUNDS, "mutex counter");
    check(atomic_total == (long)THREADS * ROUNDS, "atomic counter");
    check(tls_ok, "thread-local counters");
    int distinct = 1;
    for (int i = 0; i < THREADS; ++i)
        for (int j = i + 1; j < THREADS; ++j) distinct &= !pthread_equal(selves[i], selves[j]);
    check(distinct && !pthread_equal(selves[0], pthread_self()), "pthread_self");
    printf("%s\n", failures ? "SMPTEST FAILED" : "SMPTEST PASSED");
    return failures != 0;
}
