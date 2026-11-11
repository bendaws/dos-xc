/*
 * DOS-XC /os/libc/include/alloca.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _ALLOCA_H
#define _ALLOCA_H
#define __NEED_size_t
#include <sys/_types.h>
__BEGIN_DECLS
void *alloca(size_t);
__END_DECLS
#define alloca(size) __builtin_alloca(size)
#endif
