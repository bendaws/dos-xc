/*
 * DOS-XC /os/libc/include/grp.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _GRP_H
#define _GRP_H
#include <sys/cdefs.h>
#define __NEED_gid_t
#include <sys/_types.h>
struct group { char *gr_name, *gr_passwd; gid_t gr_gid; char **gr_mem; };
__BEGIN_DECLS
struct group *getgrnam(const char *);
struct group *getgrgid(gid_t);
__END_DECLS
#endif
