/*
 * DOS-XC /os/libc/include/mntent.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _MNTENT_H
#define _MNTENT_H
#include <stdio.h>
#define MOUNTED "/etc/mtab"
struct mntent { char *mnt_fsname, *mnt_dir, *mnt_type, *mnt_opts; int mnt_freq, mnt_passno; };
#ifdef __cplusplus
extern "C" {
#endif
FILE *setmntent(const char *, const char *);
struct mntent *getmntent(FILE *);
struct mntent *getmntent_r(FILE *, struct mntent *, char *, int);
int endmntent(FILE *);
char *hasmntopt(const struct mntent *, const char *);
#ifdef __cplusplus
}
#endif
#endif
