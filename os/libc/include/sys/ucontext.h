/*
 * DOS-XC /os/libc/include/sys/ucontext.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _SYS_UCONTEXT_H
#define _SYS_UCONTEXT_H
#include <stdint.h>
#ifdef __x86_64__
enum { REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15, REG_RDI, REG_RSI, REG_RBP, REG_RBX,
       REG_RDX, REG_RAX, REG_RCX, REG_RSP, REG_RIP, REG_EFL, REG_CSGSFS, REG_ERR, REG_TRAPNO, REG_OLDMASK, REG_CR2 };
typedef long long greg_t, gregset_t[23];
struct _libc_fpstate {
    unsigned short cwd, swd, ftw, fop;
    unsigned long long rip, rdp;
    unsigned mxcsr, mxcr_mask;
    unsigned char _st[8 * 16], _xmm[16 * 16];
    unsigned __glibc_reserved1[24];
};
typedef struct _libc_fpstate *fpregset_t;
typedef struct { gregset_t gregs; fpregset_t fpregs; unsigned long long __reserved1[8]; } mcontext_t;
#else
typedef int greg_t, gregset_t[19];
typedef struct { gregset_t gregs; void *fpregs; unsigned long oldmask, cr2; } mcontext_t;
#endif
typedef struct ucontext_t {
    unsigned long uc_flags;
    struct ucontext_t *uc_link;
    stack_t uc_stack;
    mcontext_t uc_mcontext;
    sigset_t uc_sigmask;
} ucontext_t;
#endif
