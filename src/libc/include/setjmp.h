/*
 * DOS-XC C library - setjmp.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#ifndef _SETJMP_H
#define _SETJMP_H
#include <sys/cdefs.h>
__BEGIN_DECLS
#ifdef __x86_64__
typedef unsigned long __jmp_registers[8];
#else
typedef unsigned long __jmp_registers[6];
#endif
typedef struct { __jmp_registers __registers; unsigned long __saved, __mask; } jmp_buf[1];
typedef jmp_buf sigjmp_buf;
int setjmp(jmp_buf) __attribute__((__returns_twice__));
int _setjmp(jmp_buf) __attribute__((__returns_twice__));
int sigsetjmp(sigjmp_buf, int) __attribute__((__returns_twice__));
void longjmp(jmp_buf, int) __noreturn;
void _longjmp(jmp_buf, int) __noreturn;
void siglongjmp(sigjmp_buf, int) __noreturn;
__END_DECLS
#define setjmp setjmp
#endif
