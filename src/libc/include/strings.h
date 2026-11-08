/*
 * DOS-XC C library - strings.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#ifndef _STRINGS_H
#define _STRINGS_H
#define __NEED_size_t
#include <sys/_types.h>
__BEGIN_DECLS
int bcmp(const void *, const void *, size_t);
void bcopy(const void *, void *, size_t);
void bzero(void *, size_t);
void explicit_bzero(void *, size_t);
char *index(const char *, int);
char *rindex(const char *, int);
int ffs(int);
int ffsl(long);
int ffsll(long long);
int strcasecmp(const char *, const char *);
int strncasecmp(const char *, const char *, size_t);
__END_DECLS
#endif
