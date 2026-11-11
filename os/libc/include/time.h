/*
 * DOS-XC /os/libc/include/time.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _TIME_H
#define _TIME_H
#define __NEED_size_t
#define __NEED_time_t
#define __NEED_clock_t
#define __NEED_clockid_t
#define __NEED_pid_t
#define __NEED_locale_t
#define __NEED_struct_timespec
#include <sys/_types.h>

#define CLOCKS_PER_SEC 1000000L
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID 3
#define CLOCK_MONOTONIC_RAW 4
#define CLOCK_REALTIME_COARSE 5
#define CLOCK_MONOTONIC_COARSE 6
#define CLOCK_BOOTTIME 7     /* the same as CLOCK_MONOTONIC: the system never suspends */
#define TIME_UTC 1
#define TIMER_ABSTIME 1

struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};
struct itimerspec { struct timespec it_interval, it_value; };

__BEGIN_DECLS
clock_t clock(void);
double difftime(time_t, time_t);
time_t mktime(struct tm *);
time_t timegm(struct tm *);
time_t time(time_t *);
char *asctime(const struct tm *);
char *asctime_r(const struct tm *__restrict, char *__restrict);
char *ctime(const time_t *);
char *ctime_r(const time_t *, char *);
struct tm *gmtime(const time_t *);
struct tm *gmtime_r(const time_t *__restrict, struct tm *__restrict);
struct tm *localtime(const time_t *);
struct tm *localtime_r(const time_t *__restrict, struct tm *__restrict);
size_t strftime(char *__restrict, size_t, const char *__restrict, const struct tm *__restrict);
size_t strftime_l(char *__restrict, size_t, const char *__restrict, const struct tm *__restrict, locale_t);
char *strptime(const char *__restrict, const char *__restrict, struct tm *__restrict);
int clock_gettime(clockid_t, struct timespec *);
int clock_getres(clockid_t, struct timespec *);
int clock_settime(clockid_t, const struct timespec *);
int clock_nanosleep(clockid_t, int, const struct timespec *, struct timespec *);
int nanosleep(const struct timespec *, struct timespec *);
int timespec_get(struct timespec *, int);
void tzset(void);
extern char *tzname[2];
extern long timezone;
extern int daylight;
__END_DECLS
#endif
