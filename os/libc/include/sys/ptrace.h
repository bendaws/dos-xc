/*
 * DOS-XC /os/libc/include/sys/ptrace.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _SYS_PTRACE_H
#define _SYS_PTRACE_H
#define PTRACE_TRACEME 0
#define PTRACE_PEEKDATA 2
#define PTRACE_POKEDATA 5
#define PTRACE_CONT 7
#define PTRACE_GETREGS 12
#define PTRACE_ATTACH 16
#define PTRACE_DETACH 17
#ifdef __cplusplus
extern "C" {
#endif
long ptrace(int, ...);
#ifdef __cplusplus
}
#endif
#endif
