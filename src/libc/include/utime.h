/*
 * DOS-XC C library - utime.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#ifndef _UTIME_H
#define _UTIME_H
#define __NEED_time_t
#include <sys/_types.h>
struct utimbuf { time_t actime, modtime; };
__BEGIN_DECLS
int utime(const char *, const struct utimbuf *);
__END_DECLS
#endif
