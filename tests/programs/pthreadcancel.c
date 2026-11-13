/*
 * DOS-XC /tests/programs/pthreadcancel.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

/* Verify cancellation of unsignalled condition waits and mutex cleanup. */
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond;
static int ready, cleaned, timed;

static void cleanup(void *unused)
{
    (void)unused;
    /* POSIX requires cancellation to reacquire the condition's mutex. */
    cleaned = pthread_mutex_trylock(&lock) == EBUSY;
    pthread_mutex_unlock(&lock);
}

static void *waiter(void *unused)
{
    (void)unused;
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 10;
    pthread_mutex_lock(&lock);
    pthread_cleanup_push(cleanup, NULL);
    ready = 1;
    for (;;) {
        if (timed) pthread_cond_timedwait(&cond, &lock, &deadline);
        else pthread_cond_wait(&cond, &lock);
    }
    pthread_cleanup_pop(1);
    return NULL;
}

int main(void)
{
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&cond, &attr);
    for (timed = 0; timed <= 1; ++timed) {
        pthread_t thread;
        void *result = NULL;
        ready = cleaned = 0;
        if (pthread_create(&thread, NULL, waiter, NULL)) return 1;
        for (;;) {
            pthread_mutex_lock(&lock);
            int waiting = ready;
            pthread_mutex_unlock(&lock);
            if (waiting) break;
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
        struct timespec before, after;
        clock_gettime(CLOCK_MONOTONIC, &before);
        pthread_cancel(thread);
        pthread_join(thread, &result);
        clock_gettime(CLOCK_MONOTONIC, &after);
        long long elapsed = (after.tv_sec - before.tv_sec) * 1000000000LL + after.tv_nsec - before.tv_nsec;
        if (result != PTHREAD_CANCELED || !cleaned || elapsed > 2000000000LL) {
            printf("FAIL timed=%d cleaned=%d elapsed=%lld\n", timed, cleaned, elapsed);
            return 1;
        }
        printf("PASS: %s condition wait cancelled with mutex held\n", timed ? "timed" : "untimed");
    }
    pthread_mutex_lock(&lock);
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += 120000000;
    if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
    int r;
    do r = pthread_cond_timedwait(&cond, &lock, &deadline); while (!r);
    pthread_mutex_unlock(&lock);
    if (r != ETIMEDOUT) return 1;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec < deadline.tv_sec || (now.tv_sec == deadline.tv_sec && now.tv_nsec < deadline.tv_nsec)) return 1;
    puts("PASS: timed condition wait keeps its deadline");
    return 0;
}
