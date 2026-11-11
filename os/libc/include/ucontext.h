/*
 * DOS-XC /os/libc/include/ucontext.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _UCONTEXT_H
#define _UCONTEXT_H
#include <signal.h>
#ifdef __cplusplus
extern "C" {
#endif
int getcontext(ucontext_t *);
int setcontext(const ucontext_t *);
#ifdef __cplusplus
}
#endif
#endif
