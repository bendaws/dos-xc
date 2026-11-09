/*
 * DOS-XC C library - sys/random.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#ifndef _SYS_RANDOM_H
#define _SYS_RANDOM_H
#define __NEED_size_t
#define __NEED_ssize_t
#include <sys/_types.h>
#define GRND_NONBLOCK 1
#define GRND_RANDOM 2
__BEGIN_DECLS
ssize_t getrandom(void *, size_t, unsigned);
int getentropy(void *, size_t);
__END_DECLS
#endif
