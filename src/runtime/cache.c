/*
 * DOS-XC /runtime/cache.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * Sector cache for the system volume.  The runtime replaces the volume's
 * sector callbacks, so the shell, command programs and 32/64-bit programs
 * all share it.
 *
 *  - Reads are cached (LRU).  On BIOS boot, misses read a 32 KiB aligned
 *    chunk through the real-mode bridge instead of one sector at a time.
 *  - Writable media: writes go straight to disk and update the cache.
 *  - Read-only media (the boot CD): writes are kept in RAM as pinned
 *    sectors, giving a writable live system whose changes last until
 *    restart.  The cache sits at a fixed address, so the overlay survives
 *    the kernel restart that follows every 16-bit DOS program.
 */

#include "runtime.h"

#define MAGIC 0x48434442u               /* "BDCH" */
#define NIL 0xffffffffu
#define VALID 1
#define PINNED 2
#define CHUNK 64                        /* sectors per BIOS read-ahead */
#define BOUNCE ((u8 *)0x78000)

struct header {
    u32 magic, lines, buckets, readonly;
    u32 partition_lba, total_sectors, serial;
    u32 head, tail, free_head, pinned;
};

static struct header *h;
static u32 *line_lba, *line_prev, *line_next, *line_chain, *bucket;
static u8 *line_flags, *data;
static fat32_read_sector_fn original_read;
static fat32_write_sector_fn original_write;
static void *original_context;
static int bios_chunks;

static u32 hash(u32 lba) { return (lba * 2654435761u) & (h->buckets - 1); }

static u32 lookup(u32 lba) {
    for (u32 i = bucket[hash(lba)]; i != NIL; i = line_chain[i])
        if (line_lba[i] == lba) return i;
    return NIL;
}

static void lru_unlink(u32 i) {
    if (line_prev[i] != NIL) line_next[line_prev[i]] = line_next[i]; else h->head = line_next[i];
    if (line_next[i] != NIL) line_prev[line_next[i]] = line_prev[i]; else h->tail = line_prev[i];
}

static void lru_front(u32 i) {
    line_prev[i] = NIL;
    line_next[i] = h->head;
    if (h->head != NIL) line_prev[h->head] = i; else h->tail = i;
    h->head = i;
}

static void touch(u32 i) {
    if (line_flags[i] & PINNED || h->head == i) return;
    lru_unlink(i);
    lru_front(i);
}

static void chain_remove(u32 i) {
    u32 *link = &bucket[hash(line_lba[i])];
    while (*link != NIL && *link != i) link = &line_chain[*link];
    if (*link == i) *link = line_chain[i];
}

static u32 take_line(void) {
    u32 i = h->free_head;
    if (i != NIL) {
        h->free_head = line_next[i];
        return i;
    }
    i = h->tail;                        /* pinned lines are never on the LRU */
    if (i == NIL) return NIL;
    lru_unlink(i);
    chain_remove(i);
    line_flags[i] = 0;
    return i;
}

static int store(u32 lba, const void *sector, int pin) {
    u32 i = lookup(lba);
    if (i == NIL) {
        if ((i = take_line()) == NIL) return -1;
        line_lba[i] = lba;
        line_flags[i] = VALID;
        line_chain[i] = bucket[hash(lba)];
        bucket[hash(lba)] = i;
        lru_front(i);
    } else {
        touch(i);
    }
    memcpy(data + (uptr)i * 512, sector, 512);
    if (pin && !(line_flags[i] & PINNED)) {
        lru_unlink(i);
        line_flags[i] |= PINNED;
        ++h->pinned;
    }
    return 0;
}

/* One real-mode call reads CHUNK sectors (16 CD blocks) into the bounce buffer. */
static int bios_read_chunk(u32 lba) {
    volatile u8 *rpc = (volatile u8 *)0x7040;
    int cd = *(volatile u32 *)0x7010 == 0x46564443u;   /* "CDVF" */
    rpc[1] = 0xc2;
    rpc[2] = *(volatile u8 *)0x7014;
    *(volatile u32 *)(rpc + 4) = lba;
    *(volatile u16 *)(rpc + 12) = cd ? CHUNK / 4 : CHUNK;
    bios_call();
    return *(volatile int *)(rpc + 8);
}

static int fill(u32 lba) {
    if (bios_chunks) {
        u32 first = lba & ~(u32)(CHUNK - 1);
        if (!bios_read_chunk(first)) {
            for (u32 k = 0; k < CHUNK; ++k)
                if (lookup(first + k) == NIL) store(first + k, BOUNCE + k * 512, 0);
            if (lookup(lba) != NIL) return 0;
        }
    }
    u8 sector[512];
    if (original_read(original_context, lba, sector)) return -1;
    return store(lba, sector, 0);
}

static int cached_read(void *context, uint32_t lba, void *buffer) {
    (void)context;
    u32 i = lookup(lba);
    if (i == NIL) {
        if (fill(lba) || (i = lookup(lba)) == NIL) return -1;
    } else {
        touch(i);
    }
    memcpy(buffer, data + (uptr)i * 512, 512);
    return 0;
}

static int cached_write(void *context, uint32_t lba, const void *buffer) {
    (void)context;
    if (original_write) {
        if (original_write(original_context, lba, buffer)) return -1;
        if (store(lba, buffer, 0)) {
            u32 i = lookup(lba);        /* never keep a stale copy */
            if (i != NIL && !(line_flags[i] & PINNED)) {
                lru_unlink(i);
                chain_remove(i);
                line_next[i] = h->free_head;
                h->free_head = i;
            }
        }
        return 0;
    }
    if (h->pinned + 256 >= h->lines && lookup(lba) == NIL) return -1;   /* RAM disk full */
    return store(lba, buffer, 1);
}

/* The real-mode bridge, for 16-bit programs on read-only media: the RAM
   overlay's copy of sector LBA (1 when there is one), and new overlay data. */
int cache_overlay_read(u32 lba, void *buffer) {
    if (!h || !h->readonly) return 0;
    u32 i = lookup(lba);
    if (i == NIL || !(line_flags[i] & PINNED)) return 0;
    memcpy(buffer, data + (uptr)i * 512, 512);
    return 1;
}

int cache_overlay_write(u32 lba, const void *buffer) {
    if (!h || !h->readonly) return -1;
    if (h->pinned + 256 >= h->lines && lookup(lba) == NIL) return -1;   /* RAM disk full */
    return store(lba, buffer, 1);
}

void cache_init(void) {
    struct fat32_volume *volume = ctx->volume;
    if (!volume || !volume->read_sector) return;
    u64 bytes = memory_total() / 8;
    if (bytes > 128u << 20) bytes = 128u << 20;
    if (bytes < 4u << 20) bytes = 4u << 20;
    u32 lines = (u32)(bytes / 512), buckets = 1;
    while (buckets < lines / 2) buckets <<= 1;
    uptr meta = sizeof(struct header) + (uptr)lines * (5 * 4 + 1) + (uptr)buckets * 4;
    meta = (meta + PAGE - 1) & ~(uptr)(PAGE - 1);
    u8 *base = memory_reserve_front(meta + (uptr)lines * 512);
    if (!base) return;
    h = (struct header *)base;
    line_lba = (u32 *)(h + 1);
    line_prev = line_lba + lines;
    line_next = line_prev + lines;
    line_chain = line_next + lines;
    bucket = line_chain + lines;
    line_flags = (u8 *)(bucket + buckets);
    data = base + meta;

    int readonly = volume->write_sector == 0;
    u32 serial = volume->root_cluster ^ volume->sectors_per_fat ^ volume->data_lba;
    int adopt = readonly && h->magic == MAGIC && h->readonly && h->lines == lines && h->buckets == buckets &&
                h->partition_lba == volume->partition_lba && h->total_sectors == volume->total_sectors &&
                h->serial == serial;
    if (!adopt) {
        memset(h, 0, sizeof(*h));
        h->lines = lines;
        h->buckets = buckets;
        h->readonly = readonly;
        h->partition_lba = volume->partition_lba;
        h->total_sectors = volume->total_sectors;
        h->serial = serial;
        h->head = h->tail = NIL;
        for (u32 b = 0; b < buckets; ++b) bucket[b] = NIL;
        for (u32 i = 0; i < lines; ++i) {
            line_flags[i] = 0;
            line_next[i] = i + 1 < lines ? i + 1 : NIL;
        }
        h->free_head = 0;
        h->magic = MAGIC;
    }
    original_read = volume->read_sector;
    original_write = volume->write_sector;
    original_context = volume->context;
    bios_chunks = ctx->bios_available;
    volume->read_sector = cached_read;
    volume->write_sector = cached_write;
}

/* The kernel mounted the system volume again after a 16-bit program, which
   may have written to the disk: drop what was cached (a read-only medium's
   RAM overlay is kept) and hook the new volume. */
void cache_resume(void) {
    struct fat32_volume *volume = ctx->volume;
    if (!h || !volume || !volume->read_sector) return;
    if (!h->readonly) {
        h->head = h->tail = NIL;
        h->pinned = 0;
        for (u32 b = 0; b < h->buckets; ++b) bucket[b] = NIL;
        for (u32 i = 0; i < h->lines; ++i) {
            line_flags[i] = 0;
            line_next[i] = i + 1 < h->lines ? i + 1 : NIL;
        }
        h->free_head = 0;
    }
    original_read = volume->read_sector;
    original_write = volume->write_sector;
    original_context = volume->context;
    volume->read_sector = cached_read;
    volume->write_sector = cached_write;
}
