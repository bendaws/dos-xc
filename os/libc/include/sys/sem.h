/*
 * DOS-XC /os/libc/include/sys/sem.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _SYS_SEM_H
#define _SYS_SEM_H
#include <sys/types.h>
typedef int key_t;
#define IPC_CREAT 01000
#define IPC_EXCL 02000
#define IPC_NOWAIT 04000
#define IPC_RMID 0
#define IPC_SET 1
#define IPC_STAT 2
#define IPC_PRIVATE ((key_t)0)
#define SETVAL 16
#define GETVAL 12
#define SEM_UNDO 0x1000
struct ipc_perm { key_t __key; unsigned uid, gid, cuid, cgid; unsigned short mode, __seq; };
struct semid_ds { struct ipc_perm sem_perm; time_t sem_otime, sem_ctime; unsigned long sem_nsems; };
struct sembuf { unsigned short sem_num; short sem_op, sem_flg; };
#ifdef __cplusplus
extern "C" {
#endif
key_t ftok(const char *, int);
int semget(key_t, int, int);
int semop(int, struct sembuf *, size_t);
int semctl(int, int, int, ...);
#ifdef __cplusplus
}
#endif

#endif
