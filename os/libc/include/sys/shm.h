/*
 * DOS-XC /os/libc/include/sys/shm.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _SYS_SHM_H
#define _SYS_SHM_H
#include <sys/ipc.h>
#include <sys/types.h>
#define SHM_RDONLY 010000
#define SHM_HUGETLB 04000
#define SHMLBA 4096
struct shmid_ds { size_t shm_segsz; long __pad[16]; };
#ifdef __cplusplus
extern "C" {
#endif
int shmget(key_t, size_t, int);
void *shmat(int, const void *, int);
int shmdt(const void *);
int shmctl(int, int, struct shmid_ds *);
#ifdef __cplusplus
}
#endif
#endif
