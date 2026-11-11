/*
 * DOS-XC /os/libc/include/pwd.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _PWD_H
#define _PWD_H
#include <sys/cdefs.h>
#define __NEED_uid_t
#define __NEED_gid_t
#define __NEED_size_t
#include <sys/_types.h>
struct passwd { char *pw_name, *pw_passwd; uid_t pw_uid; gid_t pw_gid; char *pw_gecos, *pw_dir, *pw_shell; };
__BEGIN_DECLS
struct passwd *getpwnam(const char *);
struct passwd *getpwuid(uid_t);
int getpwnam_r(const char *, struct passwd *, char *, size_t, struct passwd **);
int getpwuid_r(uid_t, struct passwd *, char *, size_t, struct passwd **);
struct passwd *getpwent(void);
void setpwent(void);
void endpwent(void);
__END_DECLS
#endif
