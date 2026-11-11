/*
 * DOS-XC /os/libc/src/search.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include <search.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Unbalanced binary trees: the node's first member is the key, as POSIX expects. */
struct node { const void *key; struct node *left, *right; };

void *tfind(const void *key, void *const *root, int (*compare)(const void *, const void *)) {
    struct node *n = root ? *root : 0;
    while (n) {
        int c = compare(key, n->key);
        if (!c) return n;
        n = c < 0 ? n->left : n->right;
    }
    return 0;
}

void *tsearch(const void *key, void **root, int (*compare)(const void *, const void *)) {
    if (!root) return 0;
    struct node **at = (struct node **)root;
    while (*at) {
        int c = compare(key, (*at)->key);
        if (!c) return *at;
        at = c < 0 ? &(*at)->left : &(*at)->right;
    }
    struct node *n = calloc(1, sizeof(*n));
    if (!n) return 0;
    n->key = key;
    *at = n;
    return n;
}

void *tdelete(const void *key, void **root, int (*compare)(const void *, const void *)) {
    if (!root || !*root) return 0;
    struct node **at = (struct node **)root, *parent = 0;
    while (*at) {
        int c = compare(key, (*at)->key);
        if (!c) break;
        parent = *at;
        at = c < 0 ? &(*at)->left : &(*at)->right;
    }
    struct node *n = *at;
    if (!n) return 0;
    if (!n->left) *at = n->right;
    else if (!n->right) *at = n->left;
    else {
        struct node **min = &n->right;
        while ((*min)->left) min = &(*min)->left;
        struct node *m = *min;
        *min = m->right;
        m->left = n->left;
        m->right = n->right;
        *at = m;
    }
    free(n);
    return parent ? (void *)parent : (void *)*root ? *root : (void *)root;
}

static void walk(const struct node *n, void (*action)(const void *, VISIT, int), int depth) {
    if (!n) return;
    if (!n->left && !n->right) { action(n, leaf, depth); return; }
    action(n, preorder, depth);
    walk(n->left, action, depth + 1);
    action(n, postorder, depth);
    walk(n->right, action, depth + 1);
    action(n, endorder, depth);
}

void twalk(const void *root, void (*action)(const void *, VISIT, int)) { walk(root, action, 0); }

void tdestroy(void *root, void (*free_key)(void *)) {
    struct node *n = root;
    if (!n) return;
    tdestroy(n->left, free_key);
    tdestroy(n->right, free_key);
    if (free_key) free_key((void *)n->key);
    free(n);
}

void *lfind(const void *key, const void *base, size_t *count, size_t size, int (*compare)(const void *, const void *)) {
    for (size_t i = 0; i < *count; ++i)
        if (!compare(key, (const char *)base + i * size)) return (char *)base + i * size;
    return 0;
}

void *lsearch(const void *key, void *base, size_t *count, size_t size, int (*compare)(const void *, const void *)) {
    void *found = lfind(key, base, count, size, compare);
    if (found) return found;
    void *slot = (char *)base + *count * size;
    memcpy(slot, key, size);
    ++*count;
    return slot;
}

void swab(const void *from, void *to, ssize_t n) {
    const unsigned char *a = from;
    unsigned char *b = to;
    for (ssize_t i = 0; i + 1 < n; i += 2) { unsigned char t = a[i]; b[i] = a[i + 1]; b[i + 1] = t; }
}

char *strnstr(const char *haystack, const char *needle, size_t length) {
    size_t n = strlen(needle);
    if (!n) return (char *)haystack;
    for (size_t i = 0; i + n <= length && haystack[i]; ++i)
        if (!strncmp(haystack + i, needle, n)) return (char *)haystack + i;
    return 0;
}

#include <sched.h>
int __sched_cpucount(size_t size, const cpu_set_t *set) {
    int n = 0;
    for (size_t i = 0; i < size / sizeof(unsigned long); ++i) n += __builtin_popcountl(set->__bits[i]);
    return n;
}
int sched_setaffinity(int pid, size_t size, const void *set) { (void)pid; (void)size; (void)set; return 0; }
