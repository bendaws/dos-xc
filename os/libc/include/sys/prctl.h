/*
 * DOS-XC /os/libc/include/sys/prctl.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _SYS_PRCTL_H
#define _SYS_PRCTL_H
#define PR_SET_PDEATHSIG 1
#define PR_GET_PDEATHSIG 2
#define PR_GET_DUMPABLE 3
#define PR_SET_DUMPABLE 4
#define PR_SET_NAME 15
#define PR_GET_NAME 16
#define PR_CAPBSET_READ 23
#define PR_CAPBSET_DROP 24
#define PR_SET_SECCOMP 22
#define PR_GET_SECCOMP 21
#define PR_SET_TIMERSLACK 29
#define PR_SET_NO_NEW_PRIVS 38
#define PR_GET_NO_NEW_PRIVS 39
#define PR_SET_PTRACER 0x59616d61
#define PR_SET_VMA 0x53564d41
#define PR_SET_VMA_ANON_NAME 0
#ifdef __cplusplus
extern "C" {
#endif
int prctl(int option, ...);
#ifdef __cplusplus
}
#endif

#endif
