/*
 * DOS-XC C library - sys/cdefs.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#ifndef _SYS_CDEFS_H
#define _SYS_CDEFS_H

#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }
#define __NOTHROW throw()
#define __restrict_arr
#else
#define __BEGIN_DECLS
#define __END_DECLS
#define __NOTHROW
#endif

#if defined(__cplusplus) || !defined(__STDC_VERSION__) || __STDC_VERSION__ < 199901L
#define __restrict __restrict__
#else
#define __restrict restrict
#endif

#define __noreturn __attribute__((__noreturn__))
#define __printflike(f, a) __attribute__((__format__(__printf__, f, a)))
#define __scanflike(f, a) __attribute__((__format__(__scanf__, f, a)))
#define __deprecated __attribute__((__deprecated__))
#define __malloc_like __attribute__((__malloc__))
#define __DOSXC_LIBC 1

#endif
