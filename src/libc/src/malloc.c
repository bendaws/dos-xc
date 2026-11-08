/*
 * DOS-XC C library - memory allocator
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * Segregated free lists with boundary tags.  Memory comes from the
 * system in growing regions (the alloc system call); requests of 1 MiB or more
 * get a region of their own, which free() returns to the system.
 *
 * Chunk layout (16-byte aligned, 16-byte header):
 *   prev_size  size of the previous chunk when it is free
 *   size       chunk size | INUSE | PREV_INUSE | MAPPED
 *   payload    (next/prev free-list links while the chunk is free)
 */
#include "libc.h"
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#define ALIGNMENT 16
#define HEADER 16
#define MIN_CHUNK 32
#define INUSE 1
#define PREV_INUSE 2
#define MAPPED 4
#define FLAGS 7
#define MAP_THRESHOLD (1u << 20)
#define NBINS 128

typedef struct chunk {
    size_t prev_size;
    size_t size;
#if __SIZEOF_POINTER__ == 4
    size_t pad[2];
#endif
    struct chunk *next, *prev;
} chunk;

_Static_assert(__builtin_offsetof(chunk, next) == HEADER, "chunk header must be 16 bytes");

#define SIZE(c) ((c)->size & ~(size_t)FLAGS)
#define NEXT(c) ((chunk *)((char *)(c) + SIZE(c)))
#define MEM(c) ((void *)((char *)(c) + HEADER))
#define CHUNK(p) ((chunk *)((char *)(p) - HEADER))

static chunk *bins[NBINS];
static unsigned bitmap[NBINS / 32];
static size_t next_region = 1u << 20;
static size_t total_mapped;

static unsigned bin_index(size_t size) {
    if (size < 1024) return (unsigned)(size >> 4);
    unsigned e = (unsigned)(sizeof(size_t) * 8 - 1 - (unsigned)__builtin_clzl((unsigned long)size));
    unsigned index = 64 + (e - 10) * 4 + (unsigned)((size >> (e - 2)) & 3);
    return index < NBINS ? index : NBINS - 1;
}

static void bin_insert(chunk *c) {
    unsigned i = bin_index(SIZE(c));
    c->prev = 0;
    c->next = bins[i];
    if (bins[i]) bins[i]->prev = c;
    bins[i] = c;
    bitmap[i / 32] |= 1u << (i % 32);
}

static void bin_remove(chunk *c) {
    unsigned i = bin_index(SIZE(c));
    if (c->prev) c->prev->next = c->next;
    else bins[i] = c->next;
    if (c->next) c->next->prev = c->prev;
    if (!bins[i]) bitmap[i / 32] &= ~(1u << (i % 32));
}

/* Mark C (of SIZE bytes) free, coalesce with free neighbours, and bin it. */
static void release_chunk(chunk *c, size_t size) {
    size_t flags = c->size & PREV_INUSE;
    c->size = size | flags;
    if (!(c->size & PREV_INUSE)) {
        chunk *prev = (chunk *)((char *)c - c->prev_size);
        bin_remove(prev);
        size += SIZE(prev);
        prev->size = size | (prev->size & PREV_INUSE);
        c = prev;
    }
    chunk *next = NEXT(c);
    if (!(next->size & INUSE)) {
        bin_remove(next);
        size += SIZE(next);
        c->size = size | (c->size & PREV_INUSE);
    }
    next = NEXT(c);
    next->prev_size = size;
    next->size &= ~(size_t)PREV_INUSE;
    bin_insert(c);
}

/* Give the tail of C beyond SIZE back to the free lists. */
static void trim(chunk *c, size_t size) {
    size_t total = SIZE(c);
    if (total - size < MIN_CHUNK) return;
    c->size = size | (c->size & (PREV_INUSE | INUSE));
    chunk *rest = NEXT(c);
    rest->size = (total - size) | PREV_INUSE | INUSE;
    NEXT(rest)->size |= PREV_INUSE;
    release_chunk(rest, total - size);
}

static int grow(size_t size) {
    size_t want = size + 2 * HEADER;
    if (want < next_region) want = next_region;
    want = (want + 4095) & ~(size_t)4095;
    char *region = dosxc_sys_alloc(want);
    if (!region) {
        want = (size + 2 * HEADER + 4095) & ~(size_t)4095;
        region = dosxc_sys_alloc(want);
        if (!region) return -1;
    } else if (next_region < (64u << 20)) {
        next_region *= 2;
    }
    /* [chunk ... ][fence: size 0, INUSE] */
    chunk *c = (chunk *)region;
    size_t body = want - HEADER;
    c->prev_size = 0;
    c->size = body | PREV_INUSE | INUSE;
    chunk *fence = (chunk *)(region + body);
    fence->prev_size = 0;
    fence->size = INUSE;
    release_chunk(c, body);
    return 0;
}

static chunk *find(size_t size) {
    unsigned i = bin_index(size);
    for (chunk *c = bins[i]; c; c = c->next)
        if (SIZE(c) >= size) return c;
    for (unsigned word = (i + 1) / 32; word < NBINS / 32; ++word) {
        unsigned bits = bitmap[word];
        if (word == (i + 1) / 32) bits &= ~0u << ((i + 1) % 32);
        if (bits) return bins[word * 32 + (unsigned)__builtin_ctz(bits)];
    }
    return 0;
}

static size_t request_size(size_t n) {
    if (n > (size_t)-1 / 2) return 0;
    size_t size = (n + HEADER + ALIGNMENT - 1) & ~(size_t)(ALIGNMENT - 1);
    return size < MIN_CHUNK ? MIN_CHUNK : size;
}

static void *map_large(size_t size) {
    size_t total = (size + 4095) & ~(size_t)4095;
    chunk *c = dosxc_sys_alloc(total);
    if (!c) return 0;
    c->prev_size = 0;
    c->size = total | MAPPED | INUSE | PREV_INUSE;
    total_mapped += total;
    return MEM(c);
}

void *malloc(size_t n) {
    size_t size = request_size(n);
    if (!size) {
        errno = ENOMEM;
        return 0;
    }
    if (size >= MAP_THRESHOLD) {
        void *p = map_large(size);
        if (!p) errno = ENOMEM;
        return p;
    }
    chunk *c = find(size);
    if (!c) {
        if (grow(size)) {
            errno = ENOMEM;
            return 0;
        }
        c = find(size);
    }
    bin_remove(c);
    c->size |= INUSE;
    NEXT(c)->size |= PREV_INUSE;
    trim(c, size);
    return MEM(c);
}

void free(void *p) {
    if (!p) return;
    chunk *c = CHUNK(p);
    if (c->size & MAPPED) {
        total_mapped -= SIZE(c) + c->prev_size;
        dosxc_sys_release((char *)c - c->prev_size);
        return;
    }
    if (!(c->size & INUSE)) {
        static const char message[] = "free(): invalid or double free\n";
        dosxc_sys_write(2, message, sizeof(message) - 1);
        abort();
    }
    release_chunk(c, SIZE(c));
}

void *calloc(size_t count, size_t size) {
    if (size && count > (size_t)-1 / size) {
        errno = ENOMEM;
        return 0;
    }
    size_t total = count * size;
    void *p = malloc(total);
    if (p && !(CHUNK(p)->size & MAPPED)) memset(p, 0, total);
    return p;
}

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (!n) {
        free(p);
        return 0;
    }
    size_t size = request_size(n);
    if (!size) {
        errno = ENOMEM;
        return 0;
    }
    chunk *c = CHUNK(p);
    size_t have = SIZE(c);
    if (!(c->size & MAPPED)) {
        if (size <= have) {
            trim(c, size);
            return p;
        }
        chunk *next = NEXT(c);
        if (!(next->size & INUSE) && have + SIZE(next) >= size && size < MAP_THRESHOLD) {
            bin_remove(next);
            c->size = (have + SIZE(next)) | (c->size & (INUSE | PREV_INUSE));
            NEXT(c)->size |= PREV_INUSE;
            trim(c, size);
            return p;
        }
    } else if (size <= have && size * 2 > have) {
        return p;
    }
    void *q = malloc(n);
    if (!q) return 0;
    size_t keep = have - HEADER;
    memcpy(q, p, keep < n ? keep : n);
    free(p);
    return q;
}

void *reallocarray(void *p, size_t count, size_t size) {
    if (size && count > (size_t)-1 / size) {
        errno = ENOMEM;
        return 0;
    }
    return realloc(p, count * size);
}

void *memalign(size_t alignment, size_t n) {
    if (alignment <= ALIGNMENT) return malloc(n);
    if (alignment & (alignment - 1)) {
        errno = EINVAL;
        return 0;
    }
    size_t size = request_size(n);
    if (!size || size + alignment + MIN_CHUNK < size) {
        errno = ENOMEM;
        return 0;
    }
    char *raw = malloc(size + alignment + MIN_CHUNK);
    if (!raw) return 0;
    chunk *c = CHUNK(raw);
    uintptr_t aligned = ((uintptr_t)raw + MIN_CHUNK + alignment - 1) & ~(uintptr_t)(alignment - 1);
    if ((uintptr_t)raw % alignment == 0) aligned = (uintptr_t)raw;
    if (aligned == (uintptr_t)raw) return raw;
    chunk *a = CHUNK(aligned);
    size_t lead = (size_t)((char *)a - (char *)c);
    if (c->size & MAPPED) {
        /* A mapped chunk records in prev_size its offset from the start of
           its system block; free() releases the block from there. */
        size_t offset = c->prev_size + lead;
        a->prev_size = offset;
        a->size = (SIZE(c) - lead) | MAPPED | INUSE | PREV_INUSE;
        return MEM(a);
    }
    size_t total = SIZE(c);
    a->size = (total - lead) | INUSE;
    c->size = lead | (c->size & PREV_INUSE) | INUSE;
    release_chunk(c, lead);
    trim(a, size);
    return MEM(a);
}

int posix_memalign(void **out, size_t alignment, size_t n) {
    if (alignment < sizeof(void *) || (alignment & (alignment - 1))) return EINVAL;
    void *p = memalign(alignment, n);
    if (!p) return ENOMEM;
    *out = p;
    return 0;
}
void *aligned_alloc(size_t alignment, size_t n) { return memalign(alignment, n); }
void *valloc(size_t n) { return memalign(4096, n); }
void *pvalloc(size_t n) { return memalign(4096, (n + 4095) & ~(size_t)4095); }

size_t malloc_usable_size(void *p) { return p ? SIZE(CHUNK(p)) - HEADER : 0; }

struct mallinfo mallinfo(void) {
    struct mallinfo m;
    memset(&m, 0, sizeof(m));
    m.hblkhd = (int)total_mapped;
    return m;
}
