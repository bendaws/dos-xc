/*
 * DOS-XC /runtime/memory.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * The program arena: one block of physical RAM above the system area,
 * divided into page-granular regions tagged with the owning process.
 * Programs nest strictly (a parent waits for its child), so releasing a
 * finished process returns every region it owned.
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
            if (map[i].type != 1 || !(map[i].extended & 1)) continue;   /* ACPI 3 "ignore" */
            u64 base = map[i].base, top = map[i].base + map[i].length;
            if (base <= ARENA_FLOOR && top > ARENA_FLOOR && top > best_end) best_end = top;
        }
    }
    if (!best_end) {
        /* QEMU/Bochs: CMOS 0x34/0x35 = 64 KiB blocks above 16 MiB. */
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
        /* Reserve slots first so a full table cannot leave a split half-made. */
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
        return (void *)base;
    }
    return 0;
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
        regions[i].owner = FREE;
        coalesce();
        return 0;
    }
    return -1;
}

void memory_free_owner(int owner) {
    for (unsigned i = 0; i < count; ++i)
        if (regions[i].owner == owner) regions[i].owner = FREE;
    coalesce();
}

u64 memory_total(void) { return arena_end - arena_start; }

u64 memory_available(void) {
    start_allocator();
    u64 total = 0;
    for (unsigned i = 0; i < count; ++i)
        if (regions[i].owner == FREE) total += regions[i].size;
    return total;
}
