/*
 * DOS-XC /os/libc/include/fnmatch.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _FNMATCH_H
#define _FNMATCH_H
#include <sys/cdefs.h>
#define FNM_NOMATCH 1
#define FNM_NOESCAPE 0x1
#define FNM_PATHNAME 0x2
#define FNM_PERIOD 0x4
#define FNM_LEADING_DIR 0x8
#define FNM_CASEFOLD 0x10
#define FNM_FILE_NAME FNM_PATHNAME
#define FNM_IGNORECASE FNM_CASEFOLD
__BEGIN_DECLS
int fnmatch(const char *, const char *, int);
__END_DECLS
#endif
