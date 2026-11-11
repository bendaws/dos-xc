/*
 * DOS-XC /os/libc/include/assert.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include <sys/cdefs.h>
#undef assert
#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
__BEGIN_DECLS
void __assert_fail(const char *expression, const char *file, unsigned line, const char *function) __noreturn;
__END_DECLS
#define assert(expression) ((expression) ? (void)0 : __assert_fail(#expression, __FILE__, __LINE__, __func__))
#endif
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && __STDC_VERSION__ < 202311L && !defined(__cplusplus)
#undef static_assert
#define static_assert _Static_assert
#endif
