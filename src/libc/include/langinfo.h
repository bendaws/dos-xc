/*
 * DOS-XC C library - langinfo.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#ifndef _LANGINFO_H
#define _LANGINFO_H
#define __NEED_locale_t
#include <sys/_types.h>
typedef int nl_item;
#define CODESET 14
#define RADIXCHAR 0x10000
#define THOUSEP 0x10001
#define D_T_FMT 0x20028
#define D_FMT 0x20029
#define T_FMT 0x2002A
#define T_FMT_AMPM 0x2002B
#define AM_STR 0x20026
#define PM_STR 0x20027
#define YESEXPR 0x50000
#define NOEXPR 0x50001
#define CRNCYSTR 0x4000F
__BEGIN_DECLS
char *nl_langinfo(nl_item);
char *nl_langinfo_l(nl_item, locale_t);
__END_DECLS
#endif
