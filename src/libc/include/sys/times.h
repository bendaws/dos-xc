/*
 * DOS-XC C library - sys/times.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#ifndef _SYS_TIMES_H
#define _SYS_TIMES_H
#define __NEED_clock_t
#include <sys/_types.h>
struct tms { clock_t tms_utime, tms_stime, tms_cutime, tms_cstime; };
__BEGIN_DECLS
clock_t times(struct tms *);
__END_DECLS
#endif
