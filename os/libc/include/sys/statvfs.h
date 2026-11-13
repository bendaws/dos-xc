/*
 * DOS-XC /os/libc/include/sys/statvfs.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _SYS_STATVFS_H
#define _SYS_STATVFS_H
#include <sys/types.h>
typedef unsigned long fsblkcnt_t;
typedef unsigned long fsfilcnt_t;
struct statvfs {
    unsigned long f_bsize, f_frsize;
    fsblkcnt_t f_blocks, f_bfree, f_bavail;
    fsfilcnt_t f_files, f_ffree, f_favail;
    unsigned long f_fsid, f_flag, f_namemax;
};
#define ST_RDONLY 1
#define ST_NOSUID 2
#define statvfs64 statvfs
#define fstatvfs64 fstatvfs
#ifdef __cplusplus
extern "C" {
#endif
int statvfs(const char *__restrict, struct statvfs *__restrict);
int fstatvfs(int, struct statvfs *);
#ifdef __cplusplus
}
#endif
#endif
