/*
 * DOS-XC /os/libc/include/glob.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _GLOB_H
#define _GLOB_H
#include <sys/cdefs.h>
#define __NEED_size_t
#include <sys/_types.h>
typedef struct {
    size_t gl_pathc;
    char **gl_pathv;
    size_t gl_offs;
    int __capacity;
} glob_t;
#define GLOB_ERR 0x1
#define GLOB_MARK 0x2
#define GLOB_NOSORT 0x4
#define GLOB_DOOFFS 0x8
#define GLOB_NOCHECK 0x10
#define GLOB_APPEND 0x20
#define GLOB_NOESCAPE 0x40
#define GLOB_PERIOD 0x80
#define GLOB_ONLYDIR 0x100
#define GLOB_TILDE 0x200
#define GLOB_NOSPACE 1
#define GLOB_ABORTED 2
#define GLOB_NOMATCH 3
#define GLOB_NOSYS 4
__BEGIN_DECLS
int glob(const char *__restrict, int, int (*)(const char *, int), glob_t *__restrict);
void globfree(glob_t *);
__END_DECLS
#endif
