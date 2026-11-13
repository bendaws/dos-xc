/*
 * DOS-XC /os/runtime/paging.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "runtime.h"

#define POOL 128                        /* page-table pages the runtime may own */
static u8 pool[POOL][4096] __attribute__((aligned(4096)));
static unsigned pool_used;
static int active;
static uptr root;                       /* our CR3 */
u64 paging_user;                        /* 4 (the U bit) when programs run in ring 3 */
int paging_isolated;                    /* ring 3 sees only what aspace_user gave it */

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
#define MAX_SPACES 64
#define MAX_OWNED 1024

/* A page-table tree.  Process trees start as a copy of the base tree's top
   level and own only the tables they changed (library data). */
struct aspace {
    int used;
    u64 *pml4;
    u64 *owned[MAX_OWNED];          /* tables this tree may change; pml4 first */
    int owned_count;
};
static struct aspace spaces[MAX_SPACES];
static struct aspace *base, *current_space;

static u64 *arena_table(void) { return memory_alloc(PAGE, PAGE, OWNER_KERNEL, 1); }

static int owns(const struct aspace *a, const u64 *t) {
    for (int i = 0; i < a->owned_count; ++i)
        if (a->owned[i] == t) return 1;
    return 0;
}

static u64 *adopt(struct aspace *a, u64 *t) {
    if (!t || a->owned_count == MAX_OWNED) return 0;
    a->owned[a->owned_count++] = t;
    return t;
}

/* LEVEL: 3 = ENTRY is in a PDPT (1 GiB pages), 2 = in a PD (2 MiB pages). */
static u64 *own_child(struct aspace *a, u64 *entry, int level) {
    if (!(*entry & 1)) return 0;
    if (*entry & PS) {
        u64 *t = adopt(a, a == base ? new_table() : arena_table());
        if (!t) return 0;
        u64 address = *entry & (level == 3 ? 0x000fffffc0000000ull : 0x000fffffffe00000ull);
        u64 flags = *entry & 0x8000000000000fffull;
        u64 step = level == 3 ? 1ull << 21 : 1ull << 12;
        if (level == 2) {                   /* 4 KiB entries: PAT moves from bit 12 to bit 7 */
            int pat = (*entry >> 12) & 1;
            flags &= ~PS;
            flags = (flags & ~0x1000ull) | (pat ? PS : 0);
        }
        for (int i = 0; i < 512; ++i) t[i] = (address + (u64)i * step) | flags;
        *entry = (u64)(uptr)t | (*entry & 0x8000000000000007ull) | 0x63 | paging_user;
        *entry &= ~PS;
        return t;
    }
    u64 *child = (u64 *)(uptr)(*entry & ADDRESS);
    if (owns(a, child)) return child;
    u64 *t = adopt(a, a == base ? new_table() : arena_table());
    if (!t) return 0;
    memcpy(t, child, 4096);
    *entry = (u64)(uptr)t | (*entry & ~ADDRESS);
    return t;
}

static int make_base(void) {
    if (base) return 0;
    u64 *pml4 = new_table();
    if (!pml4) return -1;
    memcpy(pml4, (void *)(read_cr3() & ADDRESS), 4096);
    base = &spaces[0];
    base->used = 1;
    base->pml4 = pml4;
    base->owned[0] = pml4;
    base->owned_count = 1;
    current_space = base;
    active = 1;
    root = (uptr)pml4;
    write_cr3(root);
    return 0;
}

/* Ring 3: the base tree maps the first 4 GiB itself with 2 MiB pages ring 3
   may use (every page, for now).  The firmware's mappings above that are
   kept for the kernel only (UEFI maps its memory and devices there). */
int paging_user_identity(int isolated) {
    if (base || pool_used + 6 > POOL) return -1;
    u64 *pml4 = new_table(), *pdpt = new_table();
    u64 *old = (u64 *)(read_cr3() & ADDRESS);
    for (int i = 1; i < 512; ++i) pml4[i] = old[i] & ~4ull;
    if ((old[0] & 1) && !(old[0] & PS)) {
        u64 *old_pdpt = (u64 *)(uptr)(old[0] & ADDRESS);
        for (int i = 4; i < 512; ++i) pdpt[i] = old_pdpt[i] & ~4ull;
    }
    base = &spaces[0];
    base->used = 1;
    base->pml4 = pml4;
    base->owned[0] = pml4;
    base->owned[1] = pdpt;
    base->owned_count = 2;
    for (int g = 0; g < 4; ++g) {
        u64 *pd = adopt(base, new_table());
        for (int i = 0; i < 512; ++i) pd[i] = ((u64)g << 30) + ((u64)i << 21) + (isolated ? 0x83 : 0x87);
        pdpt[g] = (u64)(uptr)pd | 7;
    }
    pml4[0] = (u64)(uptr)pdpt | 7;
    current_space = base;
    active = 1;
    root = (uptr)pml4;
    paging_user = 4;
    paging_isolated = isolated;
    write_cr3(root);
    return 0;
}

static u64 *pte(struct aspace *a, uptr va) {
    u64 *pdpt = own_child(a, &a->pml4[(va >> 39) & 511], 4);
    if (!pdpt) return 0;
    u64 *pd = own_child(a, &pdpt[(va >> 30) & 511], 3);
    if (!pd) return 0;
    u64 *pt = own_child(a, &pd[(va >> 21) & 511], 2);
    if (!pt) return 0;
    return &pt[(va >> 12) & 511];
}

struct aspace *aspace_new(void) {
    if (make_base()) return 0;
    for (int i = 1; i < MAX_SPACES; ++i) {
        struct aspace *a = &spaces[i];
        if (a->used) continue;
        memset(a, 0, sizeof(*a));
        a->pml4 = adopt(a, arena_table());
        if (!a->pml4) return 0;
        memcpy(a->pml4, base->pml4, 4096);
        for (int k = 32; k < 254; ++k) a->pml4[k] = 0;
        a->used = 1;
        return a;
    }
    return 0;
}

void aspace_activate(struct aspace *a) {
    if (!a) a = base;
    if (!a || a == current_space) return;
    current_space = a;
    write_cr3((uptr)a->pml4);
}

void aspace_free(struct aspace *a) {
    if (!a || a == base || !a->used) return;
    if (current_space == a) aspace_activate(base);
    vm_tables_free(a->pml4);
    for (int i = 0; i < a->owned_count; ++i) memory_free(a->owned[i], OWNER_KERNEL);
    a->used = 0;
    a->owned_count = 0;
}

int aspace_map(struct aspace *a, uptr va, uptr pa) {
    if (!a) a = base;
    if (!a && make_base()) return -1;
    if (!a) a = base;
    u64 *e = pte(a, va);
    if (!e) return -1;
    u64 old = *e;
    *e = (*e & ~ADDRESS) | ((u64)pa & ADDRESS) | 3 | paging_user;
    if (a == current_space) invalidate(va);
    if (old & 1) smp_tlb_flush(a == base ? 0 : a->pml4);
    return 0;
}

void paging_share_top(unsigned index) {
    for (int i = 1; i < MAX_SPACES; ++i)
        if (spaces[i].used) spaces[i].pml4[index] = base->pml4[index];
}

/* Ring 3's access to [VA, VA + SIZE) of the first 4 GiB in tree A: MODE 0
   none, 1 read, 2 read and write.  The pages keep their frames. */
static void user_rights(struct aspace *a, uptr va, uptr size, int mode);
void aspace_user(struct aspace *a, uptr va, uptr size, int mode) {
    if (!a || !paging_isolated || !size) return;
    user_rights(a, va, size, mode);
    smp_tlb_flush(a == base ? 0 : a->pml4);
}

static void user_rights(struct aspace *a, uptr va, uptr size, int mode) {
    uptr end = (va + size + PAGE - 1) & ~(uptr)(PAGE - 1);
    va &= ~(uptr)(PAGE - 1);
    if (end > 0x100000000ull) end = 0x100000000ull;
    while (va < end) {
        u64 *pdpt = (u64 *)(uptr)(a->pml4[0] & ADDRESS);
        u64 *pd = (u64 *)(uptr)(pdpt[(va >> 30) & 511] & ADDRESS);
        u64 pde = pd[(va >> 21) & 511];
        if (!mode) {
            if (!(pde & 1) || (pde & PS) || !owns(a, (u64 *)(uptr)(pde & ADDRESS))) {
                va = (va | ((1ull << 21) - 1)) + 1;
                continue;
            }
            u64 *pt = (u64 *)(uptr)(pde & ADDRESS);
            if ((pt[(va >> 12) & 511] & ADDRESS) == va) pt[(va >> 12) & 511] = (pt[(va >> 12) & 511] & ~4ull) | 2;
            if (a == current_space) invalidate(va);
            va += PAGE;
            continue;
        }
        u64 *e = pte(a, va);
        if (!e) return;
        u64 flags = mode == 2 ? 6 : mode == 1 ? 4 : 2;
        if ((*e & ADDRESS) == va) *e = (*e & ~6ull) | flags;     /* not frames a fork copied */
        if (a == current_space) invalidate(va);
        va += PAGE;
    }
}

void aspace_user_all(uptr va, uptr size, int mode) {
    for (int i = 0; i < MAX_SPACES; ++i)
        if (spaces[i].used) aspace_user(&spaces[i], va, size, mode);
}

u64 *aspace_root(const struct aspace *a) { return a ? a->pml4 : 0; }

static int pat_ready(void) {
    static int state;
    if (state) return state > 0;
    u32 a = 1, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    state = -1;
    if (!(d & (1u << 16))) return 0;
    u32 low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0x277u));
    high = (high & ~0xffu) | 0x01;          /* PAT entry 4 (PAT bit, no PCD/PWT): write-combining */
    __asm__ volatile("wbinvd" ::: "memory");
    __asm__ volatile("wrmsr" ::"c"(0x277u), "a"(low), "d"(high));
    __asm__ volatile("wbinvd" ::: "memory");
    write_cr3(read_cr3());
    smp_set_pat(((u64)high << 32) | low);
    state = 1;
    return 1;
}

static void combine_in(u64 *pml4, uptr va, uptr end) {
    if (!(pml4[0] & 1)) return;
    u64 *pdpt = (u64 *)(uptr)(pml4[0] & ADDRESS);
    while (va < end) {
        u64 pdpte = pdpt[(va >> 30) & 511];
        if (!(pdpte & 1) || (pdpte & PS)) { va = (va | ((1ull << 30) - 1)) + 1; continue; }
        u64 *pde = &((u64 *)(uptr)(pdpte & ADDRESS))[(va >> 21) & 511];
        if (!(*pde & 1)) { va = (va | ((1ull << 21) - 1)) + 1; continue; }
        if (*pde & PS) {
            *pde = (*pde & ~0x18ull) | 0x1000ull;
            va = (va | ((1ull << 21) - 1)) + 1;
            continue;
        }
        u64 *e = &((u64 *)(uptr)(*pde & ADDRESS))[(va >> 12) & 511];
        if (*e & 1) *e = (*e & ~0x18ull) | PS;
        va += PAGE;
    }
}

/* Write-combining for [VA, VA + SIZE) below 4 GiB (a framebuffer), in every tree. */
void paging_write_combining(uptr va, uptr size) {
    if (!base || !size || va >= 0x100000000ull || !pat_ready()) return;
    uptr end = va + size;
    if (end > 0x100000000ull) end = 0x100000000ull;
    for (int i = 0; i < MAX_SPACES; ++i)
        if (spaces[i].used) combine_in(spaces[i].pml4, va & ~(uptr)(PAGE - 1), end);
    __asm__ volatile("wbinvd" ::: "memory");
    write_cr3(read_cr3());
    smp_tlb_flush(0);
}

/* May ring 3 code of the running process use [P, P + N) (writing too, when
   WRITE)?  Mappings the process has not touched yet count when vm.c would
   fault them in. */
int paging_user_ok(uptr p, uptr n, int write) {
    if (!paging_isolated) return 1;
    if (!n) n = 1;
    if (p + n < p) return 0;
    uptr end = p + n;
    u64 need = write ? 7 : 5;
    u64 *pml4 = (u64 *)(read_cr3() & ADDRESS);
    for (uptr va = p & ~(uptr)(PAGE - 1); va < end; va += PAGE) {
        if (va >= 0x100000000000ull) return vm_user_ok(va, end, write);
        u64 e = pml4[(va >> 39) & 511];
        if ((e & 5) != 5) return 0;
        e = ((u64 *)(uptr)(e & ADDRESS))[(va >> 30) & 511];
        if ((e & 5) != 5) return 0;
        if (e & PS) { if ((e & need) != need) return 0; continue; }
        e = ((u64 *)(uptr)(e & ADDRESS))[(va >> 21) & 511];
        if ((e & 5) != 5) return 0;
        if (!(e & PS)) e = ((u64 *)(uptr)(e & ADDRESS))[(va >> 12) & 511];
        if ((e & need) != need) return 0;
    }
    return 1;
}

int paging_user_string(const char *s) {
    if (!paging_isolated) return 1;
    for (uptr at = (uptr)s, limit = (uptr)s + 65536; at < limit;) {
        if (!paging_user_ok(at, 1, 0)) return 0;
        uptr page_end = (at | (PAGE - 1)) + 1;
        for (; at < page_end; ++at)
            if (!*(const char *)at) return 1;
    }
    return 0;
}

void paging_user_base(uptr va, uptr size, int mode) {
    if (base) aspace_user(base, va, size, mode);
}
struct aspace *aspace_at(int index) { return index > 0 && index < MAX_SPACES && spaces[index].used ? &spaces[index] : 0; }

int paging_map(uptr va, uptr pa) {
    if (make_base()) return -1;
    return aspace_map(current_space, va, pa);
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

#if __SIZEOF_POINTER__ == 8
uptr paging_root(void) {
    if (make_base()) return 0;
    return (uptr)base->pml4;
}
#else
uptr paging_root(void) { return 0; }
int paging_user_identity(int isolated) { (void)isolated; return -1; }
void aspace_user(struct aspace *a, uptr va, uptr size, int mode) { (void)a; (void)va; (void)size; (void)mode; }
void aspace_user_all(uptr va, uptr size, int mode) { (void)va; (void)size; (void)mode; }
u64 *aspace_root(const struct aspace *a) { (void)a; return 0; }
struct aspace *aspace_at(int index) { (void)index; return 0; }
void paging_user_base(uptr va, uptr size, int mode) { (void)va; (void)size; (void)mode; }
void paging_write_combining(uptr va, uptr size) { (void)va; (void)size; }
int paging_user_ok(uptr p, uptr n, int write) { (void)p; (void)n; (void)write; return 1; }
int paging_user_string(const char *s) { (void)s; return 1; }
struct aspace *aspace_new(void) { return 0; }
void aspace_free(struct aspace *a) { (void)a; }
void aspace_activate(struct aspace *a) { (void)a; }
int aspace_map(struct aspace *a, uptr va, uptr pa) { (void)a; return paging_map(va, pa); }
void paging_share_top(unsigned index) { (void)index; }
#endif

void paging_resume(void) {
    if (!active) return;
#if __SIZEOF_POINTER__ == 4
    write_cr4(read_cr4() | 0x10);
    write_cr3(root);
    write_cr0(read_cr0() | 0x80000000u);
#else
    write_cr3((uptr)current_space->pml4);
#endif
    vm_resume();
}
