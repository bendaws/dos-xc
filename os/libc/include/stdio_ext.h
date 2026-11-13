/*
 * DOS-XC /os/libc/include/stdio_ext.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _STDIO_EXT_H
#define _STDIO_EXT_H
#include <stdio.h>
#define FSETLOCKING_QUERY 0
#define FSETLOCKING_INTERNAL 1
#define FSETLOCKING_BYCALLER 2
__BEGIN_DECLS
size_t __fbufsize(FILE *);
size_t __fpending(FILE *);
int __flbf(FILE *);
int __freadable(FILE *);
int __fwritable(FILE *);
int __freading(FILE *);
int __fwriting(FILE *);
int __fsetlocking(FILE *, int);
int __fpurge(FILE *);
void _flushlbf(void);
size_t __freadahead(FILE *);
const char *__freadptr(FILE *, size_t *);
void __freadptrinc(FILE *, size_t);
void __fseterr(FILE *);
__END_DECLS
#endif
