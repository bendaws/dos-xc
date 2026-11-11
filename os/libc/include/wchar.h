/*
 * DOS-XC /os/libc/include/wchar.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _WCHAR_H
#define _WCHAR_H
#define __NEED_size_t
#define __NEED_wchar_t
#define __NEED_wint_t
#define __NEED_mbstate_t
#define __NEED_FILE
#define __NEED_locale_t
#define __NEED_va_list
#include <sys/_types.h>
#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif
#ifndef WCHAR_MAX
#define WCHAR_MAX __WCHAR_MAX__
#ifdef __WCHAR_MIN__
#define WCHAR_MIN __WCHAR_MIN__
#else
#define WCHAR_MIN (-WCHAR_MAX - 1)
#endif
#endif
struct tm;
__BEGIN_DECLS
wint_t btowc(int);
int wctob(wint_t);
int mbsinit(const mbstate_t *);
size_t mbrlen(const char *__restrict, size_t, mbstate_t *__restrict);
size_t mbrtowc(wchar_t *__restrict, const char *__restrict, size_t, mbstate_t *__restrict);
size_t wcrtomb(char *__restrict, wchar_t, mbstate_t *__restrict);
size_t mbsrtowcs(wchar_t *__restrict, const char **__restrict, size_t, mbstate_t *__restrict);
size_t mbsnrtowcs(wchar_t *__restrict, const char **__restrict, size_t, size_t, mbstate_t *__restrict);
size_t wcsrtombs(char *__restrict, const wchar_t **__restrict, size_t, mbstate_t *__restrict);
size_t wcsnrtombs(char *__restrict, const wchar_t **__restrict, size_t, size_t, mbstate_t *__restrict);
size_t wcslen(const wchar_t *);
size_t wcsnlen(const wchar_t *, size_t);
int wcscmp(const wchar_t *, const wchar_t *);
int wcsncmp(const wchar_t *, const wchar_t *, size_t);
int wcscoll(const wchar_t *, const wchar_t *);
size_t wcsxfrm(wchar_t *__restrict, const wchar_t *__restrict, size_t);
wchar_t *wcscpy(wchar_t *__restrict, const wchar_t *__restrict);
wchar_t *wcsncpy(wchar_t *__restrict, const wchar_t *__restrict, size_t);
wchar_t *wcscat(wchar_t *__restrict, const wchar_t *__restrict);
wchar_t *wcsncat(wchar_t *__restrict, const wchar_t *__restrict, size_t);
wchar_t *wcschr(const wchar_t *, wchar_t);
wchar_t *wcsrchr(const wchar_t *, wchar_t);
size_t wcsspn(const wchar_t *, const wchar_t *);
size_t wcscspn(const wchar_t *, const wchar_t *);
wchar_t *wcspbrk(const wchar_t *, const wchar_t *);
wchar_t *wcsstr(const wchar_t *__restrict, const wchar_t *__restrict);
wchar_t *wcstok(wchar_t *__restrict, const wchar_t *__restrict, wchar_t **__restrict);
wchar_t *wcsdup(const wchar_t *);
wchar_t *wmemchr(const wchar_t *, wchar_t, size_t);
int wmemcmp(const wchar_t *, const wchar_t *, size_t);
wchar_t *wmemcpy(wchar_t *__restrict, const wchar_t *__restrict, size_t);
wchar_t *wmemmove(wchar_t *, const wchar_t *, size_t);
wchar_t *wmemset(wchar_t *, wchar_t, size_t);
long wcstol(const wchar_t *__restrict, wchar_t **__restrict, int);
unsigned long wcstoul(const wchar_t *__restrict, wchar_t **__restrict, int);
long long wcstoll(const wchar_t *__restrict, wchar_t **__restrict, int);
unsigned long long wcstoull(const wchar_t *__restrict, wchar_t **__restrict, int);
double wcstod(const wchar_t *__restrict, wchar_t **__restrict);
float wcstof(const wchar_t *__restrict, wchar_t **__restrict);
long double wcstold(const wchar_t *__restrict, wchar_t **__restrict);
int wcwidth(wchar_t);
int wcswidth(const wchar_t *, size_t);
size_t wcsftime(wchar_t *__restrict, size_t, const wchar_t *__restrict, const struct tm *__restrict);
wint_t fgetwc(FILE *);
wint_t getwc(FILE *);
wint_t getwchar(void);
wint_t fputwc(wchar_t, FILE *);
wint_t putwc(wchar_t, FILE *);
wint_t putwchar(wchar_t);
wint_t ungetwc(wint_t, FILE *);
wchar_t *fgetws(wchar_t *__restrict, int, FILE *__restrict);
int fputws(const wchar_t *__restrict, FILE *__restrict);
int fwide(FILE *, int);
int fwprintf(FILE *__restrict, const wchar_t *__restrict, ...);
int swprintf(wchar_t *__restrict, size_t, const wchar_t *__restrict, ...);
int wprintf(const wchar_t *__restrict, ...);
int vfwprintf(FILE *__restrict, const wchar_t *__restrict, __dosxc_va_list);
int vswprintf(wchar_t *__restrict, size_t, const wchar_t *__restrict, __dosxc_va_list);
int vwprintf(const wchar_t *__restrict, __dosxc_va_list);
int fwscanf(FILE *__restrict, const wchar_t *__restrict, ...);
int swscanf(const wchar_t *__restrict, const wchar_t *__restrict, ...);
int wscanf(const wchar_t *__restrict, ...);
int vfwscanf(FILE *__restrict, const wchar_t *__restrict, __dosxc_va_list);
int vswscanf(const wchar_t *__restrict, const wchar_t *__restrict, __dosxc_va_list);
int vwscanf(const wchar_t *__restrict, __dosxc_va_list);
__END_DECLS
#endif
