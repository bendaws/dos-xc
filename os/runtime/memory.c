/*
 * DOS-XC /os/runtime/memory.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "runtime.h"

#define ARENA_FLOOR 0x5000000u          /* above RUNTIME_BASE + RUNTIME_LIMIT */
#define MAX_REGIONS 16384
#define FREE (-1)

struct region { uptr base, size; int owner; };
static struct region regions[MAX_REGIONS];
static unsigned count;
static uptr arena_start, arena_end, reserve_cursor;

static uptr align_up(uptr value, uptr align) { return (value + align - 1) & ~(align - 1); }

#define HIGH_START 0x100000000ull
#define MAX_HIGH_RAM 8
static struct { u64 base, length; } high_ram[MAX_HIGH_RAM];
static unsigned high_count;

int memory_high(unsigned index, u64 *base, u64 *length) {
    if (index >= high_count) return -1;
    *base = high_ram[index].base;
    *length = high_ram[index].length;
    return 0;
}

struct __attribute__((packed)) e820 { u64 base, length; u32 type, extended; };

/* Usable RAM above ARENA_FLOOR, from the BIOS E820 map (CMOS as fallback). */
static int detect_bios(uptr *start, uptr *end) {
    volatile u8 *rpc = (volatile u8 *)0x7040;
    rpc[1] = 0xe8;
    bios_call();
    int entries = *(volatile int *)(rpc + 8);
    u64 best_end = 0;
    if (entries > 0 && entries <= 1024) {
        struct e820 *map = (struct e820 *)0x78000;
        for (int i = 0; i < entries; ++i) {
            if (map[i].type != 1 || !(map[i].extended & 1)) continue;
            u64 base = map[i].base, top = map[i].base + map[i].length;
            if (base <= ARENA_FLOOR && top > ARENA_FLOOR && top > best_end) best_end = top;
            if (top > HIGH_START && high_count < MAX_HIGH_RAM) {
                if (base < HIGH_START) base = HIGH_START;
                u64 first = (base + PAGE - 1) & ~(u64)(PAGE - 1);
                high_ram[high_count].base = first;
                high_ram[high_count].length = (top & ~(u64)(PAGE - 1)) - first;
                ++high_count;
            }
        }
    }
    if (!best_end) {
        rt_outb(0x70, 0x34);
        u32 low = rt_inb(0x71);
        rt_outb(0x70, 0x35);
        u32 blocks = low | ((u32)rt_inb(0x71) << 8);
        best_end = 0x1000000ull + (u64)blocks * 65536;
    }
    if (best_end > 0xfffff000ull) best_end = 0xfffff000ull;
    if (best_end <= ARENA_FLOOR + 0x400000) return -1;
    *start = ARENA_FLOOR;
    *end = (uptr)(best_end & ~(u64)(PAGE - 1));
    return 0;
}

int memory_init(void) {
    uptr start, end;
    if (ctx->memory_size) {
        start = align_up((uptr)ctx->memory_base, PAGE);
        end = (uptr)(ctx->memory_base + ctx->memory_size) & ~(uptr)(PAGE - 1);
    } else if (ctx->bios_available) {
        if (detect_bios(&start, &end)) return -1;
    } else {
        return -1;
    }
    arena_start = reserve_cursor = start;
    arena_end = end;
    count = 0;
    return 0;
}

/* Permanent kernel memory carved from the front of the arena (the disk
   cache), at a fixed address across reboots of the shell. */
void *memory_reserve_front(uptr size) {
    size = align_up(size, PAGE);
    if (count || reserve_cursor + size + 0x1000000 > arena_end) return 0;
    void *block = (void *)reserve_cursor;
    reserve_cursor += size;
    return block;
}

static void start_allocator(void) {
    if (count) return;
    regions[0].base = reserve_cursor;
    regions[0].size = arena_end - reserve_cursor;
    regions[0].owner = FREE;
    count = 1;
}

static int insert(unsigned at, uptr base, uptr size, int owner) {
    if (count == MAX_REGIONS) return -1;
    memmove(&regions[at + 1], &regions[at], (count - at) * sizeof(regions[0]));
    regions[at].base = base;
    regions[at].size = size;
    regions[at].owner = owner;
    ++count;
    return 0;
}

void *memory_alloc(uptr size, uptr align, int owner, int zero) {
    start_allocator();
    if (!size) size = 1;
    if (size > arena_end - arena_start) return 0;
    size = align_up(size, PAGE);
    if (align < PAGE) align = PAGE;
    for (unsigned i = 0; i < count; ++i) {
        struct region *r = &regions[i];
        if (r->owner != FREE) continue;
        uptr base = align_up(r->base, align);
        if (base < r->base || base - r->base > r->size || r->size - (base - r->base) < size) continue;
        uptr lead = base - r->base, tail = r->size - lead - size;
        if (count + (lead ? 1 : 0) + (tail ? 1 : 0) > MAX_REGIONS) return 0;
        if (lead) {
            r->size = lead;
            insert(++i, base, size, owner);
        } else {
            r->size = size;
            r->owner = owner;
        }
        if (tail) insert(i + 1, base + size, tail, FREE);
        if (zero) memset((void *)base, 0, size);
        if (owner > 0) process_memory_added(base, size, owner);
        return (void *)base;
    }
    return 0;
}

/* What a freed region becomes: free, or held by a forked process that still
   uses its addresses (so nothing else is given them meanwhile). */
static int released(const struct region *r) {
    int keeper = fork_keeper(r->base, r->size);
    return keeper > 0 ? keeper : FREE;
}

static void coalesce(void) {
    unsigned out = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (out && regions[out - 1].owner == FREE && regions[i].owner == FREE &&
            regions[out - 1].base + regions[out - 1].size == regions[i].base)
            regions[out - 1].size += regions[i].size;
        else
            regions[out++] = regions[i];
    }
    count = out;
}

int memory_free(void *block, int owner) {
    for (unsigned i = 0; i < count; ++i) {
        if (regions[i].base != (uptr)block || regions[i].owner == FREE) continue;
        if (owner >= 0 && regions[i].owner != owner) return -1;
        if (regions[i].owner > 0) process_memory_removed(regions[i].base, regions[i].size);
        regions[i].owner = released(&regions[i]);
        coalesce();
        return 0;
    }
    return -1;
}

/* Whether OWNER holds every page of the range. */
int memory_owns(uptr base, uptr size, int owner) {
    uptr at = base, end = base + size;
    for (unsigned i = 0; i < count && at < end; ++i) {
        const struct region *r = &regions[i];
        if (r->base + r->size <= at || r->base > at) continue;
        if (r->owner != owner) return 0;
        at = r->base + r->size;
    }
    return at >= end;
}

/* Free part of what OWNER holds; the rest of its regions stays allocated. */
int memory_free_range(uptr base, uptr size, int owner) {
    if (!memory_owns(base, size, owner)) return -1;
    uptr end = base + size;
    for (unsigned i = 0; i < count; ++i) {
        struct region *r = &regions[i];
        if (r->owner != owner || r->base + r->size <= base || r->base >= end) continue;
        uptr r_end = r->base + r->size;
        if (r->base < base) {
            if (count == MAX_REGIONS) return -1;
            r->size = base - r->base;
            insert(i + 1, base, r_end - base, owner);
            continue;
        }
        if (r_end > end) {
            if (count == MAX_REGIONS) return -1;
            r->size = end - r->base;
            insert(i + 1, end, r_end - end, owner);
        }
        if (owner > 0) process_memory_removed(regions[i].base, regions[i].size);
        regions[i].owner = released(&regions[i]);
    }
    coalesce();
    return 0;
}

void memory_free_owner(int owner) {
    for (unsigned i = 0; i < count; ++i)
        if (regions[i].owner == owner) {
            if (owner > 0) process_memory_removed(regions[i].base, regions[i].size);
            regions[i].owner = released(&regions[i]);
        }
    coalesce();
}

void memory_swap_owner(int a, int b) {
    for (unsigned i = 0; i < count; ++i)
        if (regions[i].owner == a) regions[i].owner = b;
        else if (regions[i].owner == b) regions[i].owner = a;
}

u64 memory_owned(int owner) {
    u64 total = 0;
    for (unsigned i = 0; i < count; ++i)
        if (regions[i].owner == owner) total += regions[i].size;
    return total;
}

u64 memory_total(void) { return arena_end - arena_start; }

u64 memory_available(void) {
    start_allocator();
    u64 total = 0;
    for (unsigned i = 0; i < count; ++i)
        if (regions[i].owner == FREE) total += regions[i].size;
    return total;
}

/* Calls FN for each region OWNER holds. */
void memory_each(int owner, void (*fn)(uptr base, uptr size, void *context), void *context) {
    for (unsigned i = 0; i < count; ++i)
        if (regions[i].owner == owner) fn(regions[i].base, regions[i].size, context);
}
