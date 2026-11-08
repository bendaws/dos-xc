/*
 * DOS-XC C library - malloc.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#ifndef _MALLOC_H
#define _MALLOC_H
#include <stdlib.h>
__BEGIN_DECLS
void *memalign(size_t, size_t) __malloc_like;
void *pvalloc(size_t) __malloc_like;
size_t malloc_usable_size(void *);
struct mallinfo { int arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost; };
struct mallinfo mallinfo(void);
__END_DECLS
#endif
