/*
 * DOS-XC /os/libc/include/syslog.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _SYSLOG_H
#define _SYSLOG_H
#define LOG_EMERG 0
#define LOG_ALERT 1
#define LOG_CRIT 2
#define LOG_ERR 3
#define LOG_WARNING 4
#define LOG_NOTICE 5
#define LOG_INFO 6
#define LOG_DEBUG 7
#define LOG_PID 1
#define LOG_CONS 2
#define LOG_NDELAY 8
#define LOG_USER (1 << 3)
#define LOG_DAEMON (3 << 3)
#define LOG_AUTH (4 << 3)
#define LOG_AUTHPRIV (10 << 3)
#define LOG_LOCAL1 (17 << 3)
#define LOG_LOCAL2 (18 << 3)
#define LOG_LOCAL3 (19 << 3)
#define LOG_LOCAL4 (20 << 3)
#define LOG_LOCAL5 (21 << 3)
#define LOG_LOCAL6 (22 << 3)
#define LOG_LOCAL7 (23 << 3)
#define LOG_LOCAL0 (16 << 3)
#define LOG_MASK(p) (1 << (p))
#define LOG_UPTO(p) ((1 << ((p) + 1)) - 1)
#ifdef __cplusplus
extern "C" {
#endif
static __inline__ int setlogmask(int mask) { return mask; }
void openlog(const char *, int, int);
void syslog(int, const char *, ...);
void vsyslog(int, const char *, __builtin_va_list);
void closelog(void);
#ifdef __cplusplus
}
#endif

#endif
