/*
 * DOS-XC C library - wctype.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#ifndef _WCTYPE_H
#define _WCTYPE_H
#define __NEED_wint_t
#define __NEED_locale_t
#include <sys/_types.h>
typedef unsigned long wctype_t;
typedef const int *wctrans_t;
#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif
__BEGIN_DECLS
int iswalnum(wint_t);
int iswalpha(wint_t);
int iswblank(wint_t);
int iswcntrl(wint_t);
int iswdigit(wint_t);
int iswgraph(wint_t);
int iswlower(wint_t);
int iswprint(wint_t);
int iswpunct(wint_t);
int iswspace(wint_t);
int iswupper(wint_t);
int iswxdigit(wint_t);
int iswctype(wint_t, wctype_t);
wint_t towlower(wint_t);
wint_t towupper(wint_t);
wint_t towctrans(wint_t, wctrans_t);
wctrans_t wctrans(const char *);
wctype_t wctype(const char *);
__END_DECLS
#endif
