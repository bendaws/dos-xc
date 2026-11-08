/*
 * DOS-XC C library - sys/utsname.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#ifndef _SYS_UTSNAME_H
#define _SYS_UTSNAME_H
#include <sys/cdefs.h>
struct utsname { char sysname[65], nodename[65], release[65], version[65], machine[65]; };
__BEGIN_DECLS
int uname(struct utsname *);
__END_DECLS
#endif
