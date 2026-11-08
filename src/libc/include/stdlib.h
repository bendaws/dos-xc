/*
 * DOS-XC C library - stdlib.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#ifndef _STDLIB_H
#define _STDLIB_H
#define __NEED_size_t
#define __NEED_wchar_t
#include <sys/_types.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7fffffff
#define MB_CUR_MAX ((size_t)__dosxc_mb_cur_max())

__BEGIN_DECLS
typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;

size_t __dosxc_mb_cur_max(void);

double atof(const char *);
int atoi(const char *);
long atol(const char *);
long long atoll(const char *);
double strtod(const char *__restrict, char **__restrict);
float strtof(const char *__restrict, char **__restrict);
long double strtold(const char *__restrict, char **__restrict);
long strtol(const char *__restrict, char **__restrict, int);
long long strtoll(const char *__restrict, char **__restrict, int);
unsigned long strtoul(const char *__restrict, char **__restrict, int);
unsigned long long strtoull(const char *__restrict, char **__restrict, int);

int rand(void);
void srand(unsigned);
int rand_r(unsigned *);
long random(void);
void srandom(unsigned);
double drand48(void);
long lrand48(void);
void srand48(long);

void *malloc(size_t) __malloc_like;
void *calloc(size_t, size_t) __malloc_like;
void *realloc(void *, size_t);
void *reallocarray(void *, size_t, size_t);
void free(void *);
void *aligned_alloc(size_t, size_t) __malloc_like;
int posix_memalign(void **, size_t, size_t);
void *valloc(size_t) __malloc_like;

void abort(void) __noreturn;
int atexit(void (*)(void));
int at_quick_exit(void (*)(void));
void exit(int) __noreturn;
void _Exit(int) __noreturn;
void quick_exit(int) __noreturn;
char *getenv(const char *);
char *secure_getenv(const char *);
int setenv(const char *, const char *, int);
int unsetenv(const char *);
int putenv(char *);
int clearenv(void);
int system(const char *);

void *bsearch(const void *, const void *, size_t, size_t, int (*)(const void *, const void *));
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
void qsort_r(void *, size_t, size_t, int (*)(const void *, const void *, void *), void *);

int abs(int) __attribute__((__const__));
long labs(long) __attribute__((__const__));
long long llabs(long long) __attribute__((__const__));
div_t div(int, int);
ldiv_t ldiv(long, long);
lldiv_t lldiv(long long, long long);

int mblen(const char *, size_t);
int mbtowc(wchar_t *__restrict, const char *__restrict, size_t);
int wctomb(char *, wchar_t);
size_t mbstowcs(wchar_t *__restrict, const char *__restrict, size_t);
size_t wcstombs(char *__restrict, const wchar_t *__restrict, size_t);

int mkstemp(char *);
int mkstemps(char *, int);
int mkostemp(char *, int);
char *mkdtemp(char *);
char *mktemp(char *);
char *realpath(const char *__restrict, char *__restrict);
const char *getprogname(void);
int getloadavg(double *, int);
__END_DECLS
#endif
