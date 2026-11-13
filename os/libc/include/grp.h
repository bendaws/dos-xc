/*
 * DOS-XC /os/libc/include/grp.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _GRP_H
#define _GRP_H
#include <sys/cdefs.h>
#define __NEED_uid_t
#include <sys/_types.h>
struct group { char *gr_name, *gr_passwd; gid_t gr_gid; char **gr_mem; };
__BEGIN_DECLS
struct group *getgrnam(const char *);
struct group *getgrgid(gid_t);
struct group *getgrent(void);
void setgrent(void);
void endgrent(void);
int getgrnam_r(const char *, struct group *, char *, __SIZE_TYPE__, struct group **);
int getgrgid_r(gid_t, struct group *, char *, __SIZE_TYPE__, struct group **);
int getgrouplist(const char *, gid_t, gid_t *, int *);
int initgroups(const char *, gid_t);
int setgroups(__SIZE_TYPE__, const gid_t *);
__END_DECLS
#endif
