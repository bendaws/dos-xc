/*
 * DOS-XC /os/libc/include/search.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _SEARCH_H
#define _SEARCH_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { preorder, postorder, endorder, leaf } VISIT;
void *tsearch(const void *key, void **root, int (*compare)(const void *, const void *));
void *tfind(const void *key, void *const *root, int (*compare)(const void *, const void *));
void *tdelete(const void *key, void **root, int (*compare)(const void *, const void *));
void twalk(const void *root, void (*action)(const void *node, VISIT order, int depth));
void tdestroy(void *root, void (*free_key)(void *key));
void *lfind(const void *key, const void *base, size_t *count, size_t size, int (*compare)(const void *, const void *));
void *lsearch(const void *key, void *base, size_t *count, size_t size, int (*compare)(const void *, const void *));
#ifdef __cplusplus
}
#endif
#endif
