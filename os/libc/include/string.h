/*
 * DOS-XC /os/libc/include/string.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _STRING_H
#define _STRING_H
#define __NEED_size_t
#define __NEED_locale_t
#include <sys/_types.h>
#include <strings.h>   /* ffs, strcasecmp ... as on glibc and the BSDs */
__BEGIN_DECLS
void *memchr(const void *, int, size_t);
int memcmp(const void *, const void *, size_t);
void *memcpy(void *__restrict, const void *__restrict, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
void *memccpy(void *__restrict, const void *__restrict, int, size_t);
void *mempcpy(void *__restrict, const void *__restrict, size_t);
void *memmem(const void *, size_t, const void *, size_t);
void *memrchr(const void *, int, size_t);
void *rawmemchr(const void *, int);
char *strcat(char *__restrict, const char *__restrict);
char *strchr(const char *, int);
char *strchrnul(const char *, int);
int strcmp(const char *, const char *);
int strcoll(const char *, const char *);
int strcoll_l(const char *, const char *, locale_t);
char *strcpy(char *__restrict, const char *__restrict);
size_t strcspn(const char *, const char *);
char *strerror(int);
int strerror_r(int, char *, size_t);
char *strerror_l(int, locale_t);
size_t strlen(const char *);
size_t strnlen(const char *, size_t);
char *strncat(char *__restrict, const char *__restrict, size_t);
int strncmp(const char *, const char *, size_t);
char *strncpy(char *__restrict, const char *__restrict, size_t);
char *strpbrk(const char *, const char *);
char *strrchr(const char *, int);
size_t strspn(const char *, const char *);
char *strstr(const char *, const char *);
char *strcasestr(const char *, const char *);
char *strtok(char *__restrict, const char *__restrict);
char *strtok_r(char *__restrict, const char *__restrict, char **__restrict);
char *strsep(char **__restrict, const char *__restrict);
size_t strxfrm(char *__restrict, const char *__restrict, size_t);
size_t strxfrm_l(char *__restrict, const char *__restrict, size_t, locale_t);
char *strdup(const char *) __malloc_like;
char *strndup(const char *, size_t) __malloc_like;
char *stpcpy(char *__restrict, const char *__restrict);
char *stpncpy(char *__restrict, const char *__restrict, size_t);
size_t strlcpy(char *__restrict, const char *__restrict, size_t);
size_t strlcat(char *__restrict, const char *__restrict, size_t);
char *strsignal(int);
int strcasecmp(const char *, const char *);
int strncasecmp(const char *, const char *, size_t);
char *strnstr(const char *haystack, const char *needle, size_t length);
__END_DECLS
#endif
