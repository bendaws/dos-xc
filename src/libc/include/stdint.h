/*
 * DOS-XC C library - stdint.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#ifndef _DOSXC_STDINT_H
#define _DOSXC_STDINT_H

typedef __INT8_TYPE__ int8_t;
typedef __INT16_TYPE__ int16_t;
typedef __INT32_TYPE__ int32_t;
typedef __INT64_TYPE__ int64_t;
typedef __UINT8_TYPE__ uint8_t;
typedef __UINT16_TYPE__ uint16_t;
typedef __UINT32_TYPE__ uint32_t;
typedef __UINT64_TYPE__ uint64_t;

typedef int8_t int_least8_t;
typedef int16_t int_least16_t;
typedef int32_t int_least32_t;
typedef int64_t int_least64_t;
typedef uint8_t uint_least8_t;
typedef uint16_t uint_least16_t;
typedef uint32_t uint_least32_t;
typedef uint64_t uint_least64_t;

/* "Fast" types follow the System V convention: word sized above 8 bits. */
typedef signed char int_fast8_t;
typedef unsigned char uint_fast8_t;
#if __SIZEOF_LONG__ == 8
typedef long int_fast16_t;
typedef long int_fast32_t;
typedef unsigned long uint_fast16_t;
typedef unsigned long uint_fast32_t;
#define INT_FAST16_MAX __LONG_MAX__
#define INT_FAST32_MAX __LONG_MAX__
#define UINT_FAST16_MAX (__LONG_MAX__ * 2UL + 1UL)
#define UINT_FAST32_MAX (__LONG_MAX__ * 2UL + 1UL)
#else
typedef int int_fast16_t;
typedef int int_fast32_t;
typedef unsigned int uint_fast16_t;
typedef unsigned int uint_fast32_t;
#define INT_FAST16_MAX __INT_MAX__
#define INT_FAST32_MAX __INT_MAX__
#define UINT_FAST16_MAX (__INT_MAX__ * 2U + 1U)
#define UINT_FAST32_MAX (__INT_MAX__ * 2U + 1U)
#endif
typedef int64_t int_fast64_t;
typedef uint64_t uint_fast64_t;

#ifndef __DEFINED_intptr_t
typedef __INTPTR_TYPE__ intptr_t;
#define __DEFINED_intptr_t
#endif
typedef __UINTPTR_TYPE__ uintptr_t;
typedef __INTMAX_TYPE__ intmax_t;
typedef __UINTMAX_TYPE__ uintmax_t;

#define INT8_MAX __INT8_MAX__
#define INT16_MAX __INT16_MAX__
#define INT32_MAX __INT32_MAX__
#define INT64_MAX __INT64_MAX__
#define INT8_MIN (-INT8_MAX - 1)
#define INT16_MIN (-INT16_MAX - 1)
#define INT32_MIN (-INT32_MAX - 1)
#define INT64_MIN (-INT64_MAX - 1)
#define UINT8_MAX __UINT8_MAX__
#define UINT16_MAX __UINT16_MAX__
#define UINT32_MAX __UINT32_MAX__
#define UINT64_MAX __UINT64_MAX__
#define INT_LEAST8_MIN INT8_MIN
#define INT_LEAST16_MIN INT16_MIN
#define INT_LEAST32_MIN INT32_MIN
#define INT_LEAST64_MIN INT64_MIN
#define INT_LEAST8_MAX INT8_MAX
#define INT_LEAST16_MAX INT16_MAX
#define INT_LEAST32_MAX INT32_MAX
#define INT_LEAST64_MAX INT64_MAX
#define UINT_LEAST8_MAX UINT8_MAX
#define UINT_LEAST16_MAX UINT16_MAX
#define UINT_LEAST32_MAX UINT32_MAX
#define UINT_LEAST64_MAX UINT64_MAX
#define INT_FAST8_MIN INT8_MIN
#define INT_FAST8_MAX INT8_MAX
#define UINT_FAST8_MAX UINT8_MAX
#define INT_FAST16_MIN (-INT_FAST16_MAX - 1)
#define INT_FAST32_MIN (-INT_FAST32_MAX - 1)
#define INT_FAST64_MIN INT64_MIN
#define INT_FAST64_MAX INT64_MAX
#define UINT_FAST64_MAX UINT64_MAX
#define INTPTR_MAX __INTPTR_MAX__
#define INTPTR_MIN (-INTPTR_MAX - 1)
#define UINTPTR_MAX __UINTPTR_MAX__
#define INTMAX_MAX __INTMAX_MAX__
#define INTMAX_MIN (-INTMAX_MAX - 1)
#define UINTMAX_MAX __UINTMAX_MAX__
#define PTRDIFF_MAX __PTRDIFF_MAX__
#define PTRDIFF_MIN (-PTRDIFF_MAX - 1)
#define SIZE_MAX __SIZE_MAX__
#define SIG_ATOMIC_MAX __SIG_ATOMIC_MAX__
#define SIG_ATOMIC_MIN (-SIG_ATOMIC_MAX - 1)
#define WCHAR_MAX __WCHAR_MAX__
#ifdef __WCHAR_MIN__
#define WCHAR_MIN __WCHAR_MIN__
#else
#define WCHAR_MIN (-WCHAR_MAX - 1)
#endif
#define WINT_MAX __WINT_MAX__
#ifdef __WINT_MIN__
#define WINT_MIN __WINT_MIN__
#else
#define WINT_MIN 0u
#endif

#ifdef __INT64_C
#define INT8_C(c) __INT8_C(c)
#define INT16_C(c) __INT16_C(c)
#define INT32_C(c) __INT32_C(c)
#define INT64_C(c) __INT64_C(c)
#define UINT8_C(c) __UINT8_C(c)
#define UINT16_C(c) __UINT16_C(c)
#define UINT32_C(c) __UINT32_C(c)
#define UINT64_C(c) __UINT64_C(c)
#define INTMAX_C(c) __INTMAX_C(c)
#define UINTMAX_C(c) __UINTMAX_C(c)
#else
#define __DOSXC_PASTE2(a, b) a##b
#define __DOSXC_PASTE(a, b) __DOSXC_PASTE2(a, b)
#define INT8_C(c) c
#define INT16_C(c) c
#define INT32_C(c) c
#define INT64_C(c) __DOSXC_PASTE(c, __INT64_C_SUFFIX__)
#define UINT8_C(c) c
#define UINT16_C(c) c
#define UINT32_C(c) c##U
#define UINT64_C(c) __DOSXC_PASTE(c, __UINT64_C_SUFFIX__)
#define INTMAX_C(c) __DOSXC_PASTE(c, __INTMAX_C_SUFFIX__)
#define UINTMAX_C(c) __DOSXC_PASTE(c, __UINTMAX_C_SUFFIX__)
#endif
#endif
