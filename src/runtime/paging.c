/*
 * DOS-XC /runtime/paging.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * Page mappings for shared libraries.  Memory is identity mapped (virtual
 * address = physical address) everywhere; the only exception is the
 * writable part of each shared library, which is remapped page by page to
 * the copy that belongs to the running process.
 *
 * 64-bit: the active tables (built by the boot loader or the firmware) are
 * cloned level by level the first time an entry below them must change, so
 * the original tables are never modified.  Large pages are split.
 * 32-bit: paging is off until the first library is loaded; then the
 * runtime builds an identity page directory of 4 MiB pages (PSE) and
 * splits the ones that hold library data into 4 KiB page tables.
 */

#include "runtime.h"

#define POOL 128                        /* page-table pages the runtime may own */
static u8 pool[POOL][4096] __attribute__((aligned(4096)));
static unsigned pool_used;
static int active;
static uptr root;                       /* our CR3 */

static inline __attribute__((unused)) uptr read_cr3(void) { uptr v; __asm__ volatile("mov %%cr3,%0" : "=r"(v)); return v; }
static inline __attribute__((unused)) void write_cr3(uptr v) { __asm__ volatile("mov %0,%%cr3" ::"r"(v) : "memory"); }
static inline __attribute__((unused)) uptr read_cr0(void) { uptr v; __asm__ volatile("mov %%cr0,%0" : "=r"(v)); return v; }
static inline __attribute__((unused)) void write_cr0(uptr v) { __asm__ volatile("mov %0,%%cr0" ::"r"(v) : "memory"); }
static inline __attribute__((unused)) uptr read_cr4(void) { uptr v; __asm__ volatile("mov %%cr4,%0" : "=r"(v)); return v; }
static inline __attribute__((unused)) void write_cr4(uptr v) { __asm__ volatile("mov %0,%%cr4" ::"r"(v) : "memory"); }
static void invalidate(uptr va) { __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory"); }

static void *new_table(void) {
    if (pool_used == POOL) return 0;
    void *t = pool[pool_used++];
    memset(t, 0, 4096);
    return t;
}

static inline __attribute__((unused)) int owned(uptr table) { return table >= (uptr)pool && table < (uptr)pool + sizeof(pool); }

#if __SIZEOF_POINTER__ == 8
#define ADDRESS 0x000ffffffffff000ull
#define PS 0x80ull

/* Make *ENTRY point to a table the runtime owns (cloning or splitting what
   it points to).  LEVEL: 3 = entry in a PDPT (1 GiB pages), 2 = in a PD. */
static u64 *own_child(u64 *entry, int level) {
    if (!(*entry & 1)) return 0;
    if (*entry & PS) {
        u64 *t = new_table();
        if (!t) return 0;
        u64 base = *entry & (level == 3 ? 0x000fffffc0000000ull : 0x000fffffffe00000ull);
        u64 flags = *entry & 0x8000000000000fffull;
        u64 step = level == 3 ? 1ull << 21 : 1ull << 12;
        if (level == 2) {                   /* 4 KiB entries: PAT moves from bit 12 to bit 7 */
            int pat = (*entry >> 12) & 1;
            flags &= ~PS;
            flags = (flags & ~0x1000ull) | (pat ? PS : 0);
        }
        for (int i = 0; i < 512; ++i) t[i] = (base + (u64)i * step) | flags;
        *entry = (u64)(uptr)t | (*entry & 0x8000000000000007ull) | 0x63;
        *entry &= ~PS;
        return t;
    }
    u64 *child = (u64 *)(uptr)(*entry & ADDRESS);
    if (owned((uptr)child)) return child;
    u64 *t = new_table();
    if (!t) return 0;
    memcpy(t, child, 4096);
    *entry = (u64)(uptr)t | (*entry & ~ADDRESS);
    return t;
}

static int enable(void) {
    u64 *pml4 = new_table();
    if (!pml4) return -1;
    memcpy(pml4, (void *)(read_cr3() & ADDRESS), 4096);
    root = (uptr)pml4;
    write_cr3(root);
    return 0;
}

static u64 *pte(uptr va) {
    u64 *pml4 = (u64 *)root;
    u64 *pdpt = own_child(&pml4[(va >> 39) & 511], 4);
    if (!pdpt) return 0;
    u64 *pd = own_child(&pdpt[(va >> 30) & 511], 3);
    if (!pd) return 0;
    u64 *pt = own_child(&pd[(va >> 21) & 511], 2);
    if (!pt) return 0;
    return &pt[(va >> 12) & 511];
}

int paging_map(uptr va, uptr pa) {
    if (!active) {
        if (enable()) return -1;
        active = 1;
    }
    u64 *e = pte(va);
    if (!e) return -1;
    *e = (*e & ~ADDRESS) | ((u64)pa & ADDRESS) | 3;
    invalidate(va);
    return 0;
}

#else

static u32 *directory;

static int enable(void) {
    directory = new_table();
    if (!directory) return -1;
    for (u32 i = 0; i < 1024; ++i) directory[i] = (i << 22) | 0x83;    /* 4 MiB, RW, present */
    root = (uptr)directory;
    write_cr4(read_cr4() | 0x10);                                       /* PSE */
    write_cr3(root);
    write_cr0(read_cr0() | 0x80000000u);
    return 0;
}

int paging_map(uptr va, uptr pa) {
    if (!active) {
        if (enable()) return -1;
        active = 1;
    }
    u32 *pde = &directory[va >> 22];
    if (*pde & 0x80) {
        u32 *t = new_table();
        if (!t) return -1;
        u32 base = *pde & 0xffc00000u;
        for (u32 i = 0; i < 1024; ++i) t[i] = (base + (i << 12)) | 3;
        *pde = (u32)(uptr)t | 3;
        write_cr3(root);
    }
    u32 *t = (u32 *)(uptr)(*pde & 0xfffff000u);
    t[(va >> 12) & 1023] = ((u32)pa & 0xfffff000u) | 3;
    invalidate(va);
    return 0;
}

#endif

/* After a 16-bit program the boot loader rebuilt the processor state. */
void paging_resume(void) {
    if (!active) return;
#if __SIZEOF_POINTER__ == 4
    write_cr4(read_cr4() | 0x10);
    write_cr3(root);
    write_cr0(read_cr0() | 0x80000000u);
#else
    write_cr3(root);
#endif
}
