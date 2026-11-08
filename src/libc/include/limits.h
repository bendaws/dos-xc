/*
 * DOS-XC C library - limits.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#ifndef _DOSXC_LIMITS_H
#define _DOSXC_LIMITS_H

#define CHAR_BIT 8
#define SCHAR_MIN (-128)
#define SCHAR_MAX 127
#define UCHAR_MAX 255
#ifdef __CHAR_UNSIGNED__
#define CHAR_MIN 0
#define CHAR_MAX UCHAR_MAX
#else
#define CHAR_MIN SCHAR_MIN
#define CHAR_MAX SCHAR_MAX
#endif
#define MB_LEN_MAX 4
#define SHRT_MAX __SHRT_MAX__
#define SHRT_MIN (-SHRT_MAX - 1)
#define USHRT_MAX (SHRT_MAX * 2 + 1)
#define INT_MAX __INT_MAX__
#define INT_MIN (-INT_MAX - 1)
#define UINT_MAX (INT_MAX * 2U + 1U)
#define LONG_MAX __LONG_MAX__
#define LONG_MIN (-LONG_MAX - 1L)
#define ULONG_MAX (LONG_MAX * 2UL + 1UL)
#define LLONG_MAX __LONG_LONG_MAX__
#define LLONG_MIN (-LLONG_MAX - 1LL)
#define ULLONG_MAX (LLONG_MAX * 2ULL + 1ULL)
#define LONG_LONG_MAX LLONG_MAX
#define LONG_LONG_MIN LLONG_MIN
#define ULONG_LONG_MAX ULLONG_MAX
#define LONG_BIT (__SIZEOF_LONG__ * 8)
#define WORD_BIT 32
#define SSIZE_MAX __PTRDIFF_MAX__

#define PATH_MAX 260
#define NAME_MAX 255
#define PIPE_BUF 4096
#define ARG_MAX 131072
#define OPEN_MAX 64
#define CHILD_MAX 16
#define LINE_MAX 2048
#define HOST_NAME_MAX 64
#define LOGIN_NAME_MAX 32
#define IOV_MAX 1024
#define NGROUPS_MAX 0
#define TZNAME_MAX 8
#define NL_ARGMAX 9
#define NL_TEXTMAX 2048
#define ATEXIT_MAX 64

#define _POSIX_ARG_MAX 4096
#define _POSIX_CHILD_MAX 25
#define _POSIX_NAME_MAX 14
#define _POSIX_OPEN_MAX 20
#define _POSIX_PATH_MAX 256
#define _POSIX_PIPE_BUF 512
#define _POSIX_SSIZE_MAX 32767
#endif
