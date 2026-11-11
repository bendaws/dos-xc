/*
 * DOS-XC /os/libc/include/sys/statfs.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _SYS_STATFS_H
#define _SYS_STATFS_H
#include <sys/types.h>
typedef struct { int __val[2]; } fsid_t;
struct statfs {
    long f_type, f_bsize;
    unsigned long f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    fsid_t f_fsid;
    long f_namelen, f_frsize, f_flags, f_spare[4];
};
#ifdef __cplusplus
extern "C" {
#endif
int statfs(const char *, struct statfs *);
int fstatfs(int, struct statfs *);
#ifdef __cplusplus
}
#endif
#endif
