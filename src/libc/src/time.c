/*
 * DOS-XC C library - time
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * The PC clock keeps local time and DOS-XC has no time zone database,
 * so localtime() and gmtime() agree (TZ is treated as UTC).
 */
#include "libc.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/resource.h>
#include <unistd.h>

char *tzname[2] = {"UTC", "UTC"};
long timezone;
int daylight;
static unsigned long long start_ns;

__attribute__((constructor)) static void record_start(void) { start_ns = dosxc_sys_nanoseconds(); }

time_t time(time_t *out) {
    time_t now = dosxc_sys_time();
    if (out) *out = now;
    return now;
}

static unsigned long long cpu_ns(void) { return dosxc_sys_nanoseconds() - start_ns; }

clock_t clock(void) { return (clock_t)(cpu_ns() / 1000); }
double difftime(time_t a, time_t b) { return (double)(a - b); }

int gettimeofday(struct timeval *tv, void *tz) {
    if (tv) {
        tv->tv_sec = dosxc_sys_time();
        tv->tv_usec = (long)((dosxc_sys_nanoseconds() / 1000) % 1000000);
    }
    if (tz) memset(tz, 0, sizeof(struct timezone));
    return 0;
}
int settimeofday(const struct timeval *tv, const struct timezone *tz) { (void)tv; (void)tz; errno = EPERM; return -1; }

int clock_gettime(clockid_t clock, struct timespec *ts) {
    unsigned long long ns;
    switch (clock) {
    case CLOCK_REALTIME:
        ts->tv_sec = dosxc_sys_time();
        ts->tv_nsec = (long)(dosxc_sys_nanoseconds() % 1000000000ull);
        return 0;
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW: ns = dosxc_sys_nanoseconds(); break;
    case CLOCK_PROCESS_CPUTIME_ID:
    case CLOCK_THREAD_CPUTIME_ID: ns = cpu_ns(); break;
    default: errno = EINVAL; return -1;
    }
    ts->tv_sec = (long long)(ns / 1000000000ull);
    ts->tv_nsec = (long)(ns % 1000000000ull);
    return 0;
}
int clock_getres(clockid_t clock, struct timespec *ts) {
    (void)clock;
    if (ts) {
        ts->tv_sec = 0;
        ts->tv_nsec = 1000;
    }
    return 0;
}
int clock_settime(clockid_t clock, const struct timespec *ts) { (void)clock; (void)ts; errno = EPERM; return -1; }
int timespec_get(struct timespec *ts, int base) {
    if (base != TIME_UTC || clock_gettime(CLOCK_REALTIME, ts)) return 0;
    return base;
}

int nanosleep(const struct timespec *request, struct timespec *remaining) {
    if (request->tv_sec < 0 || request->tv_nsec < 0 || request->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    dosxc_sys_sleep((unsigned long long)request->tv_sec * 1000000000ull + (unsigned long long)request->tv_nsec);
    if (remaining) remaining->tv_sec = remaining->tv_nsec = 0;
    return 0;
}
int clock_nanosleep(clockid_t clock, int flags, const struct timespec *request, struct timespec *remaining) {
    if (flags & TIMER_ABSTIME) {
        struct timespec now;
        clock_gettime(clock, &now);
        long long ns = (request->tv_sec - now.tv_sec) * 1000000000ll + (request->tv_nsec - now.tv_nsec);
        if (ns > 0) dosxc_sys_sleep((unsigned long long)ns);
        return 0;
    }
    return nanosleep(request, remaining) ? errno : 0;
}
unsigned sleep(unsigned seconds) {
    dosxc_sys_sleep((unsigned long long)seconds * 1000000000ull);
    return 0;
}
int usleep(useconds_t microseconds) {
    dosxc_sys_sleep((unsigned long long)microseconds * 1000ull);
    return 0;
}

clock_t times(struct tms *t) {
    clock_t ticks = (clock_t)(cpu_ns() / 10000000ull);
    if (t) {
        t->tms_utime = ticks;
        t->tms_stime = t->tms_cutime = t->tms_cstime = 0;
    }
    return (clock_t)(dosxc_sys_nanoseconds() / 10000000ull);
}

int getrusage(int who, struct rusage *usage) {
    memset(usage, 0, sizeof(*usage));
    if (who == RUSAGE_SELF) {
        unsigned long long us = cpu_ns() / 1000;
        usage->ru_utime.tv_sec = (long long)(us / 1000000);
        usage->ru_utime.tv_usec = (long)(us % 1000000);
    }
    return 0;
}

int getitimer(int which, struct itimerval *value) { (void)which; memset(value, 0, sizeof(*value)); return 0; }
int setitimer(int which, const struct itimerval *value, struct itimerval *old) {
    (void)which;
    (void)value;
    if (old) memset(old, 0, sizeof(*old));
    return 0;
}

/* ---- calendar ---- */

/* Days since 1970-01-01 for a proleptic Gregorian date (month 0-11). */
static long long days_from_civil(long long y, int m, int d) {
    y -= m < 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long mp = (m + 10) % 12;
    long long doy = (153 * mp + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

struct tm *gmtime_r(const time_t *t, struct tm *tm) {
    long long seconds = *t, days = seconds / 86400, rem = seconds % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    tm->tm_hour = (int)(rem / 3600);
    tm->tm_min = (int)(rem % 3600 / 60);
    tm->tm_sec = (int)(rem % 60);
    tm->tm_wday = (int)((days % 7 + 11) % 7);
    long long z = days + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = yoe + era * 400;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153;
    int d = (int)(doy - (153 * mp + 2) / 5 + 1);
    int m = (int)(mp < 10 ? mp + 2 : mp - 10);
    if (m < 2) ++y;
    tm->tm_year = (int)(y - 1900);
    tm->tm_mon = m;
    tm->tm_mday = d;
    tm->tm_yday = (int)(days - days_from_civil(y, 0, 1));
    tm->tm_isdst = 0;
    tm->tm_gmtoff = 0;
    tm->tm_zone = "UTC";
    return tm;
}
struct tm *localtime_r(const time_t *t, struct tm *tm) { return gmtime_r(t, tm); }
struct tm *gmtime(const time_t *t) {
    static struct tm tm;
    return gmtime_r(t, &tm);
}
struct tm *localtime(const time_t *t) { return gmtime(t); }

time_t timegm(struct tm *tm) {
    long long year = 1900LL + tm->tm_year + tm->tm_mon / 12;
    int month = tm->tm_mon % 12;
    if (month < 0) {
        month += 12;
        --year;
    }
    long long days = days_from_civil(year, month, 1) + tm->tm_mday - 1;
    time_t t = days * 86400 + tm->tm_hour * 3600LL + tm->tm_min * 60LL + tm->tm_sec;
    gmtime_r(&t, tm);
    return t;
}
time_t mktime(struct tm *tm) { return timegm(tm); }
void tzset(void) {}

static const char *const weekdays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const months[] = {"January", "February", "March", "April", "May", "June", "July",
                                     "August", "September", "October", "November", "December"};

char *asctime_r(const struct tm *tm, char *buffer) {
    snprintf(buffer, 26, "%.3s %.3s%3d %.2d:%.2d:%.2d %d\n", weekdays[tm->tm_wday % 7], months[tm->tm_mon % 12],
             tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, 1900 + tm->tm_year);
    return buffer;
}
char *asctime(const struct tm *tm) {
    static char buffer[32];
    return asctime_r(tm, buffer);
}
char *ctime_r(const time_t *t, char *buffer) {
    struct tm tm;
    return asctime_r(localtime_r(t, &tm), buffer);
}
char *ctime(const time_t *t) {
    static char buffer[32];
    return ctime_r(t, buffer);
}

size_t strftime(char *out, size_t size, const char *format, const struct tm *tm) {
    size_t used = 0;
    char item[64];
    for (; *format; ++format) {
        const char *text = item;
        if (*format != '%') {
            item[0] = *format;
            item[1] = 0;
        } else {
            ++format;
            while (*format == 'E' || *format == 'O') ++format;
            int year = 1900 + tm->tm_year;
            switch (*format) {
            case 'a': snprintf(item, sizeof(item), "%.3s", weekdays[tm->tm_wday % 7]); break;
            case 'A': text = weekdays[tm->tm_wday % 7]; break;
            case 'b': case 'h': snprintf(item, sizeof(item), "%.3s", months[tm->tm_mon % 12]); break;
            case 'B': text = months[tm->tm_mon % 12]; break;
            case 'c': snprintf(item, sizeof(item), "%.3s %.3s %2d %.2d:%.2d:%.2d %d", weekdays[tm->tm_wday % 7],
                               months[tm->tm_mon % 12], tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, year); break;
            case 'C': snprintf(item, sizeof(item), "%02d", year / 100); break;
            case 'd': snprintf(item, sizeof(item), "%02d", tm->tm_mday); break;
            case 'D': snprintf(item, sizeof(item), "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday, year % 100); break;
            case 'e': snprintf(item, sizeof(item), "%2d", tm->tm_mday); break;
            case 'F': snprintf(item, sizeof(item), "%d-%02d-%02d", year, tm->tm_mon + 1, tm->tm_mday); break;
            case 'g': snprintf(item, sizeof(item), "%02d", year % 100); break;
            case 'G': snprintf(item, sizeof(item), "%d", year); break;
            case 'H': snprintf(item, sizeof(item), "%02d", tm->tm_hour); break;
            case 'I': snprintf(item, sizeof(item), "%02d", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12); break;
            case 'j': snprintf(item, sizeof(item), "%03d", tm->tm_yday + 1); break;
            case 'm': snprintf(item, sizeof(item), "%02d", tm->tm_mon + 1); break;
            case 'M': snprintf(item, sizeof(item), "%02d", tm->tm_min); break;
            case 'n': text = "\n"; break;
            case 'p': text = tm->tm_hour < 12 ? "AM" : "PM"; break;
            case 'r': snprintf(item, sizeof(item), "%02d:%02d:%02d %s", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12,
                               tm->tm_min, tm->tm_sec, tm->tm_hour < 12 ? "AM" : "PM"); break;
            case 'R': snprintf(item, sizeof(item), "%02d:%02d", tm->tm_hour, tm->tm_min); break;
            case 's': {
                struct tm copy = *tm;
                snprintf(item, sizeof(item), "%lld", (long long)timegm(&copy));
                break;
            }
            case 'S': snprintf(item, sizeof(item), "%02d", tm->tm_sec); break;
            case 't': text = "\t"; break;
            case 'T': snprintf(item, sizeof(item), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
            case 'u': snprintf(item, sizeof(item), "%d", tm->tm_wday ? tm->tm_wday : 7); break;
            case 'U': snprintf(item, sizeof(item), "%02d", (tm->tm_yday + 7 - tm->tm_wday) / 7); break;
            case 'V': snprintf(item, sizeof(item), "%02d", (tm->tm_yday + 7 - (tm->tm_wday + 6) % 7) / 7 ? (tm->tm_yday + 7 - (tm->tm_wday + 6) % 7) / 7 : 1); break;
            case 'w': snprintf(item, sizeof(item), "%d", tm->tm_wday); break;
            case 'W': snprintf(item, sizeof(item), "%02d", (tm->tm_yday + 7 - (tm->tm_wday + 6) % 7) / 7); break;
            case 'x': snprintf(item, sizeof(item), "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday, year % 100); break;
            case 'X': snprintf(item, sizeof(item), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
            case 'y': snprintf(item, sizeof(item), "%02d", year % 100); break;
            case 'Y': snprintf(item, sizeof(item), "%d", year); break;
            case 'z': text = "+0000"; break;
            case 'Z': text = "UTC"; break;
            case '%': text = "%"; break;
            default: item[0] = '%'; item[1] = *format; item[2] = 0; if (!*format) --format; break;
            }
        }
        size_t n = strlen(text);
        if (used + n >= size) return 0;
        memcpy(out + used, text, n);
        used += n;
    }
    if (used >= size) return 0;
    out[used] = 0;
    return used;
}
size_t strftime_l(char *out, size_t size, const char *format, const struct tm *tm, locale_t locale) {
    (void)locale;
    return strftime(out, size, format, tm);
}

static int parse_number(const char **s, int digits, int *out) {
    int value = 0, n = 0;
    while (**s == ' ') ++*s;
    while (n < digits && **s >= '0' && **s <= '9') value = value * 10 + (*(*s)++ - '0'), ++n;
    if (!n) return -1;
    *out = value;
    return 0;
}

char *strptime(const char *s, const char *format, struct tm *tm) {
    for (; *format; ++format) {
        if (*format != '%') {
            if (*format == ' ') { while (*s == ' ') ++s; continue; }
            if (*s++ != *format) return 0;
            continue;
        }
        int v;
        switch (*++format) {
        case 'Y': if (parse_number(&s, 4, &v)) return 0; tm->tm_year = v - 1900; break;
        case 'm': if (parse_number(&s, 2, &v)) return 0; tm->tm_mon = v - 1; break;
        case 'd': case 'e': if (parse_number(&s, 2, &v)) return 0; tm->tm_mday = v; break;
        case 'H': if (parse_number(&s, 2, &v)) return 0; tm->tm_hour = v; break;
        case 'M': if (parse_number(&s, 2, &v)) return 0; tm->tm_min = v; break;
        case 'S': if (parse_number(&s, 2, &v)) return 0; tm->tm_sec = v; break;
        case 'y': if (parse_number(&s, 2, &v)) return 0; tm->tm_year = v < 69 ? v + 100 : v; break;
        case 'j': if (parse_number(&s, 3, &v)) return 0; tm->tm_yday = v - 1; break;
        case '%': if (*s++ != '%') return 0; break;
        default: return 0;
        }
    }
    return (char *)s;
}
