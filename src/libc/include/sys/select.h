/*
 * DOS-XC C library - sys/select.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H
#define __NEED_time_t
#define __NEED_suseconds_t
#define __NEED_struct_timeval
#define __NEED_sigset_t
#include <sys/_types.h>
#define FD_SETSIZE 64
typedef struct { unsigned long fds_bits[FD_SETSIZE / (8 * sizeof(unsigned long))]; } fd_set;
#define FD_ZERO(s) do { for (unsigned __i = 0; __i < sizeof((s)->fds_bits) / sizeof(long); ++__i) (s)->fds_bits[__i] = 0; } while (0)
#define FD_SET(d, s) ((s)->fds_bits[(d) / (8 * sizeof(long))] |= 1UL << ((d) % (8 * sizeof(long))))
#define FD_CLR(d, s) ((s)->fds_bits[(d) / (8 * sizeof(long))] &= ~(1UL << ((d) % (8 * sizeof(long)))))
#define FD_ISSET(d, s) (((s)->fds_bits[(d) / (8 * sizeof(long))] >> ((d) % (8 * sizeof(long)))) & 1)
__BEGIN_DECLS
int select(int, fd_set *__restrict, fd_set *__restrict, fd_set *__restrict, struct timeval *__restrict);
__END_DECLS
#endif
