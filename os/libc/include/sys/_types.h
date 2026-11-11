/*
 * DOS-XC /os/libc/include/sys/_types.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include <sys/cdefs.h>

#if defined(__NEED_size_t) && !defined(__DEFINED_size_t)
typedef __SIZE_TYPE__ size_t;
#define __DEFINED_size_t
#endif
#if defined(__NEED_ssize_t) && !defined(__DEFINED_ssize_t)
typedef __PTRDIFF_TYPE__ ssize_t;
#define __DEFINED_ssize_t
#endif
#if defined(__NEED_off_t) && !defined(__DEFINED_off_t)
typedef __INT64_TYPE__ off_t;
#define __DEFINED_off_t
#endif
#if defined(__NEED_pid_t) && !defined(__DEFINED_pid_t)
typedef int pid_t;
#define __DEFINED_pid_t
#endif
#if defined(__NEED_mode_t) && !defined(__DEFINED_mode_t)
typedef unsigned int mode_t;
#define __DEFINED_mode_t
#endif
#if defined(__NEED_uid_t) && !defined(__DEFINED_uid_t)
typedef unsigned int uid_t;
typedef unsigned int gid_t;
#define __DEFINED_uid_t
#endif
#if defined(__NEED_dev_t) && !defined(__DEFINED_dev_t)
typedef unsigned long long dev_t;
typedef unsigned long long ino_t;
typedef unsigned int nlink_t;
typedef long blksize_t;
typedef long long blkcnt_t;
#define __DEFINED_dev_t
#endif
#if defined(__NEED_time_t) && !defined(__DEFINED_time_t)
typedef long long time_t;
#define __DEFINED_time_t
#endif
#if defined(__NEED_clock_t) && !defined(__DEFINED_clock_t)
typedef long clock_t;
#define __DEFINED_clock_t
#endif
#if defined(__NEED_suseconds_t) && !defined(__DEFINED_suseconds_t)
typedef long suseconds_t;
typedef unsigned int useconds_t;
#define __DEFINED_suseconds_t
#endif
#if defined(__NEED_clockid_t) && !defined(__DEFINED_clockid_t)
typedef int clockid_t;
typedef void *timer_t;
#define __DEFINED_clockid_t
#endif
#if defined(__NEED_struct_timespec) && !defined(__DEFINED_struct_timespec)
struct timespec { long long tv_sec; long tv_nsec; };
#define __DEFINED_struct_timespec
#endif
#if defined(__NEED_struct_timeval) && !defined(__DEFINED_struct_timeval)
struct timeval { long long tv_sec; long tv_usec; };
#define __DEFINED_struct_timeval
#endif
#if defined(__NEED_wchar_t) && !defined(__DEFINED_wchar_t) && !defined(__cplusplus)
typedef __WCHAR_TYPE__ wchar_t;
#define __DEFINED_wchar_t
#endif
#if defined(__NEED_wint_t) && !defined(__DEFINED_wint_t)
typedef __WINT_TYPE__ wint_t;
#define __DEFINED_wint_t
#endif
#if defined(__NEED_mbstate_t) && !defined(__DEFINED_mbstate_t)
typedef struct { unsigned __count; unsigned __value; } mbstate_t;
#define __DEFINED_mbstate_t
#endif
#if defined(__NEED_locale_t) && !defined(__DEFINED_locale_t)
typedef struct __dosxc_locale *locale_t;
#define __DEFINED_locale_t
#endif
#if defined(__NEED_FILE) && !defined(__DEFINED_FILE)
typedef struct __dosxc_file FILE;
#define __DEFINED_FILE
#endif
#if defined(__NEED_va_list) && !defined(__DEFINED_va_list)
typedef __builtin_va_list __dosxc_va_list;
#define __DEFINED_va_list
#endif
#if defined(__NEED_sigset_t) && !defined(__DEFINED_sigset_t)
typedef unsigned long sigset_t;
#define __DEFINED_sigset_t
#endif
#if defined(__NEED_intptr_t) && !defined(__DEFINED_intptr_t)
typedef __INTPTR_TYPE__ intptr_t;
#define __DEFINED_intptr_t
#endif

#ifndef NULL
#ifdef __cplusplus
#define NULL __null
#else
#define NULL ((void *)0)
#endif
#endif
