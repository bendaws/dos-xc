/*
 * DOS-XC /os/runtime/vm.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "runtime.h"

#if __SIZEOF_POINTER__ == 8

#define VM_BASE 0x100000000000ull
#define VM_TOP 0x7f0000000000ull
#define VM_END (VM_TOP - (2u << 20))   /* mappings end here; above: copy windows */
#define WINDOW_FROM (VM_TOP - 2 * PAGE)
#define WINDOW_TO (VM_TOP - PAGE)
#define OWNER (-2)
#define MAX_VMAS 32768
#define MAX_HIGH 8
#define CHUNK (2u << 20)
#define ADDRESS 0x000ffffffffff000ull
#define P 1ull
#define RW 2ull
#define KEPT (1ull << 9)                /* holds a page but PROT_NONE: not present */

#define PROT_WRITE_BIT 2

struct vm_object {
    int used, refs;                  /* open files and mappings */
    int named;
    char name[64];
    u64 size;
    u64 *frames;                     /* per page: its frame, or 0 until first touched */
    uptr slots;
};
#define MAX_OBJECTS 4096
static struct vm_object objects[MAX_OBJECTS];

struct vma {
    uptr base, end;
    int prot;
    struct vm_object *object;        /* shared memory, or 0 for private memory */
    u64 offset;                      /* of base in the object */
};
/* A process's mappings, sorted.  Each process has its own, so a forked child
   keeps its parent's addresses; new mappings get addresses no process uses. */
struct vm_table { int owner; unsigned count, capacity; struct vma *v; };
#define MAX_TABLES (MAX_DEPTH + 8)
static struct vm_table tables[MAX_TABLES];
static struct vm_table *T;              /* the table the helpers below work on */
#define vmas (T->v)
#define vma_count (T->count)

struct high { u64 base, pages, *bitmap, rotor, used; };
static struct high highs[MAX_HIGH];
static unsigned high_count;
static uptr low_free;                   /* linked through the free pages */
static u64 low_free_pages;
static int ready;

static void flush_all(void) {
    uptr v;
    __asm__ volatile("mov %%cr3,%0; mov %0,%%cr3" : "=r"(v) :: "memory");
}
static void flush_page(uptr va) { __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory"); }
static void flush_range(uptr start, uptr end) {
    if (end - start > 64 * PAGE) { flush_all(); return; }
    for (uptr va = start; va < end; va += PAGE) flush_page(va);
}
static int is_active(const u64 *pml4);
/* [start, end) of TREE lost entries or rights: here and on other processors. */
static void flush_tree(const u64 *tree, uptr start, uptr end) {
    if (is_active(tree)) flush_range(start, end);
    smp_tlb_flush(tree);
}

void vm_resume(void) {
    if (!ready) return;
    uptr cr0;
    __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
    if (!(cr0 & (1u << 16))) __asm__ volatile("mov %0,%%cr0" ::"r"(cr0 | (1u << 16)) : "memory");
    cpu_page_fault_stack();
}

static int init(void) {
    if (ready) return 0;
    if (!paging_root()) return -1;
    u64 base, length;
    for (unsigned i = 0; high_count < MAX_HIGH && !memory_high(i, &base, &length); ++i) {
        u64 pages = length >> 12, words = (pages + 63) / 64;
        u64 *bitmap = memory_alloc(words * 8, PAGE, OWNER, 1);
        if (!bitmap) continue;
        highs[high_count++] = (struct high){base, pages, bitmap, 0, 0};
    }
    ready = 1;
    vm_resume();
    return 0;
}

static u64 *tree_of(int owner) {
    struct process *p = process_find(owner);
    return p ? aspace_root(p->space) : 0;
}

static u64 *current_tree(void) { return current ? aspace_root(current->space) : 0; }

static struct vm_table *table_for(int owner, int create) {
    struct vm_table *spare = 0;
    for (int i = 0; i < MAX_TABLES; ++i) {
        if (tables[i].v && tables[i].owner == owner) return &tables[i];
        if (!tables[i].v && !spare) spare = &tables[i];
    }
    if (!create || !spare) return 0;
    struct vma *v = memory_alloc(PAGE, PAGE, OWNER, 0);
    if (!v) return 0;
    *spare = (struct vm_table){owner, 0, PAGE / sizeof(struct vma), v};
    return spare;
}

static int reserve_vmas(struct vm_table *t, unsigned needed) {
    if (needed <= t->capacity) return 0;
    if (needed > MAX_VMAS) return -1;
    unsigned capacity = t->capacity;
    while (capacity < needed) capacity *= 2;
    struct vma *v = memory_alloc(capacity * sizeof(*v), PAGE, OWNER, 0);
    if (!v) return -1;
    memcpy(v, t->v, t->count * sizeof(*v));
    memory_free(t->v, OWNER);
    t->v = v;
    t->capacity = capacity;
    return 0;
}

static int is_active(const u64 *pml4) {
    uptr cr3;
    __asm__ volatile("mov %%cr3,%0" : "=r"(cr3));
    return (cr3 & ADDRESS) == (uptr)pml4;
}

static uptr low_frame(void) {
    if (!low_free) {
        uptr size = CHUNK;
        void *chunk = memory_alloc(size, PAGE, OWNER, 0);
        while (!chunk && size > PAGE) {
            size >>= 2;
            if (size < PAGE) size = PAGE;
            chunk = memory_alloc(size, PAGE, OWNER, 0);
        }
        if (!chunk) return 0;
        for (uptr off = 0; off < size; off += PAGE) {
            *(uptr *)((uptr)chunk + off) = low_free;
            low_free = (uptr)chunk + off;
            ++low_free_pages;
        }
    }
    uptr page = low_free;
    low_free = *(uptr *)page;
    --low_free_pages;
    return page;
}

static u64 frame_alloc(void) {
    for (unsigned h = 0; h < high_count; ++h) {
        struct high *r = &highs[h];
        if (r->used == r->pages) continue;
        u64 words = (r->pages + 63) / 64;
        for (u64 n = 0; n <= words; ++n) {
            u64 w = (r->rotor + n) % words;
            if (r->bitmap[w] == ~0ull) continue;
            unsigned bit = (unsigned)__builtin_ctzll(~r->bitmap[w]);
            u64 index = w * 64 + bit;
            if (index >= r->pages) continue;
            r->bitmap[w] |= 1ull << bit;
            r->rotor = w;
            ++r->used;
            return r->base + (index << 12);
        }
    }
    return low_frame();
}

static void frame_free(u64 pa) {
    for (unsigned h = 0; h < high_count; ++h) {
        struct high *r = &highs[h];
        if (pa < r->base || pa >= r->base + (r->pages << 12)) continue;
        u64 index = (pa - r->base) >> 12;
        r->bitmap[index / 64] &= ~(1ull << (index % 64));
        --r->used;
        return;
    }
    *(uptr *)(uptr)pa = low_free;
    low_free = (uptr)pa;
    ++low_free_pages;
}

static u64 *table_of(u64 *entry, int create) {
    if (*entry & P) return (u64 *)(uptr)(*entry & ADDRESS);
    if (!create) return 0;
    uptr t = low_frame();
    if (!t) return 0;
    memset((void *)t, 0, PAGE);
    *entry = (u64)t | P | RW | paging_user;
    return (u64 *)t;
}

static u64 *pte_of(u64 *pml4, uptr va, int create) {
    u64 *t = table_of(&pml4[(va >> 39) & 511], create);
    if (t) t = table_of(&t[(va >> 30) & 511], create);
    if (t) t = table_of(&t[(va >> 21) & 511], create);
    return t ? &t[(va >> 12) & 511] : 0;
}

/* The next page in [*va, end) that has a frame; advances *va to it.  Skips
   whole absent tables, so huge empty reservations cost almost nothing. */
static u64 *next_page(u64 *pml4, uptr *va, uptr end) {
    while (*va < end) {
        uptr v = *va;
        u64 e = pml4[(v >> 39) & 511];
        if (!(e & P)) { *va = (v | ((1ull << 39) - 1)) + 1; continue; }
        e = ((u64 *)(uptr)(e & ADDRESS))[(v >> 30) & 511];
        if (!(e & P)) { *va = (v | ((1ull << 30) - 1)) + 1; continue; }
        e = ((u64 *)(uptr)(e & ADDRESS))[(v >> 21) & 511];
        if (!(e & P)) { *va = (v | ((1ull << 21) - 1)) + 1; continue; }
        u64 *pte = &((u64 *)(uptr)(e & ADDRESS))[(v >> 12) & 511];
        if (*pte) return pte;
        *va = v + PAGE;
    }
    return 0;
}

/* Other trees' views of [start, end) of OWNER_TREE's mappings: dropped, to
   be faulted in again from the owner's. */
static unsigned find(uptr address);
static void forget_elsewhere(const u64 *owner_tree, uptr start, uptr end) {
    struct vm_table *saved = T;
    for (int i = 0; i < MAX_DEPTH; ++i) {
        struct process *p = process_at(i);
        u64 *t = p ? aspace_root(p->space) : 0;
        if (!t || t == owner_tree) continue;
        struct vm_table *own = table_for(p->pid, 0);
        uptr va = start;
        int any = 0;
        for (u64 *pte; (pte = next_page(t, &va, end)); va += PAGE) {
            if (own) {                  /* a forked process's own page there stays */
                T = own;
                unsigned k = find(va);
                if (k < vma_count && vmas[k].base <= va) continue;
            }
            *pte = 0;
            any = 1;
        }
        if (any) flush_tree(t, start, end);
    }
    T = saved;
}

static void release_pages(u64 *pml4, uptr start, uptr end) {
    uptr va = start;
    for (u64 *pte; (pte = next_page(pml4, &va, end)); va += PAGE) {
        frame_free(*pte & ADDRESS);
        *pte = 0;
    }
    flush_tree(pml4, start, end);
    forget_elsewhere(pml4, start, end);
}

void vm_tables_free(u64 *pml4) {
    if (!ready || !pml4) return;
    for (uptr i = VM_BASE >> 39; i < (VM_TOP >> 39); ++i) {
        if (!(pml4[i] & P)) continue;
        u64 *pdpt = (u64 *)(uptr)(pml4[i] & ADDRESS);
        for (int j = 0; j < 512; ++j) {
            if (!(pdpt[j] & P)) continue;
            u64 *pd = (u64 *)(uptr)(pdpt[j] & ADDRESS);
            for (int k = 0; k < 512; ++k)
                if (pd[k] & P) frame_free(pd[k] & ADDRESS);
            frame_free((u64)(uptr)pd);
        }
        frame_free((u64)(uptr)pdpt);
        pml4[i] = 0;
    }
}

static u64 page_flags(int prot) {
    if (prot & PROT_WRITE_BIT) return P | RW | paging_user;
    return prot ? P | paging_user : KEPT;
}

/* First mapping that ends after ADDRESS. */
static unsigned find(uptr address) {
    unsigned low = 0, high = vma_count;
    while (low < high) {
        unsigned mid = (low + high) / 2;
        if (vmas[mid].end > address) high = mid; else low = mid + 1;
    }
    return low;
}

/* Out of memory, once in the event log for each WHAT. */
static long no_memory(const char *what) {
    static const char *said[8];
    for (int i = 0; i < 8; ++i) {
        if (said[i] == what) return -12;
        if (!said[i]) { said[i] = what; event_log(DOSXC_EVENT_ERROR, "Memory", "%s (process %d)", what, current ? current->pid : 0); break; }
    }
    return -12;
}

static int insert(unsigned at, uptr base, uptr end, int prot) {
    if (reserve_vmas(T, vma_count + 1)) { no_memory("The mapping table is full"); return -1; }
    memmove(&vmas[at + 1], &vmas[at], (vma_count - at) * sizeof(*vmas));
    vmas[at] = (struct vma){base, end, prot, 0, 0};
    ++vma_count;
    return 0;
}

static void erase(unsigned at, unsigned n) {
    memmove(&vmas[at], &vmas[at + n], (vma_count - at - n) * sizeof(*vmas));
    vma_count -= n;
}

/* Make ADDRESS a boundary between mappings. */
static int split(uptr address) {
    unsigned i = find(address);
    if (i == vma_count || vmas[i].base >= address) return 0;
    struct vma v = vmas[i];
    if (insert(i + 1, address, v.end, v.prot)) return -1;
    vmas[i].end = address;
    if (v.object) {
        vmas[i + 1].object = v.object;
        vmas[i + 1].offset = v.offset + (address - v.base);
        ++v.object->refs;
    }
    return 0;
}

static void merge_around(uptr start, uptr end) {
    unsigned i = find(start);
    if (i) --i;
    while (i + 1 < vma_count && vmas[i].base <= end) {
        if (vmas[i].end == vmas[i + 1].base && vmas[i].prot == vmas[i + 1].prot &&
            !vmas[i].object && !vmas[i + 1].object) {
            vmas[i].end = vmas[i + 1].end;
            erase(i + 1, 1);
        } else {
            ++i;
        }
    }
}

static int overlap(uptr start, uptr end) {
    unsigned i = find(start);
    return i < vma_count && vmas[i].base < end;
}

static int used_anywhere(uptr start, uptr end) {
    struct vm_table *saved = T;
    int used = 0;
    for (int i = 0; i < MAX_TABLES && !used; ++i) {
        if (!tables[i].v) continue;
        T = &tables[i];
        used = overlap(start, end);
    }
    T = saved;
    return used;
}

/* Another process's mapping at ADDRESS that PID may use. */
static const struct vma *foreign(uptr address, int pid, int *owner) {
    struct vm_table *saved = T;
    const struct vma *found = 0;
    for (int i = 0; i < MAX_TABLES && !found; ++i) {
        if (!tables[i].v || tables[i].owner == pid) continue;
        T = &tables[i];
        unsigned k = find(address);
        if (k < vma_count && vmas[k].base <= address && process_may_access(tables[i].owner, pid, address)) {
            found = &vmas[k];
            *owner = tables[i].owner;
        }
    }
    T = saved;
    return found;
}

static int covered(uptr start, uptr end) {
    uptr at = start;
    for (unsigned i = find(start); i < vma_count && vmas[i].base <= at && at < end; ++i) at = vmas[i].end;
    return at >= end;
}

static void object_unref(struct vm_object *o) {
    if (!o || --o->refs > 0) return;
    for (uptr i = 0; i < o->slots; ++i)
        if (o->frames[i]) frame_free(o->frames[i]);
    if (o->frames) memory_free(o->frames, OWNER_KERNEL);
    memset(o, 0, sizeof(*o));
}

/* An object's pages are its own: the trees only lose their entries. */
static void clear_pages(u64 *pml4, uptr start, uptr end) {
    uptr va = start;
    for (u64 *pte; (pte = next_page(pml4, &va, end)); va += PAGE) *pte = 0;
    flush_tree(pml4, start, end);
    forget_elsewhere(pml4, start, end);
}

static void drop_vma(u64 *tree, const struct vma *v) {
    if (v->object) {
        if (tree) clear_pages(tree, v->base, v->end);
        else forget_elsewhere(0, v->base, v->end);
        object_unref(v->object);
    } else if (tree) {
        release_pages(tree, v->base, v->end);
    } else {
        forget_elsewhere(0, v->base, v->end);
    }
}

static void remove_range(uptr start, uptr end) {
    u64 *tree = current_tree();
    unsigned i = find(start), n = 0;
    while (i + n < vma_count && vmas[i + n].base < end) {
        drop_vma(tree, &vmas[i + n]);
        ++n;
    }
    if (n) erase(i, n);
}

/* The lowest addresses free in every process's table. */
static uptr find_gap(uptr size) {
    struct vm_table *saved = T;
    uptr at = VM_BASE;
    for (int moved = 1; moved && at;) {
        moved = 0;
        for (int i = 0; i < MAX_TABLES && at; ++i) {
            if (!tables[i].v) continue;
            T = &tables[i];
            unsigned k = find(at);
            if (k < vma_count && vmas[k].base < at + size) {
                at = vmas[k].end;
                moved = 1;
            }
            if (VM_END - at < size) at = 0;
        }
    }
    T = saved;
    return at;
}

static int in_range(uptr address, uptr size) {
    return address >= VM_BASE && address < VM_END && size <= VM_END - address;
}

/* Give [start, end) frames in PML4, which must be the active tree. */
static int touch(u64 *pml4, uptr start, uptr end, int prot) {
    for (uptr va = start; va < end; va += PAGE) {
        u64 *pte = pte_of(pml4, va, 1);
        if (!pte) return -1;
        if (*pte) continue;
        u64 pa = frame_alloc();
        if (!pa) return -1;
        *pte = pa | P | RW;
        flush_page(va);
        memset((void *)va, 0, PAGE);
        *pte = pa | page_flags(prot);
        flush_page(va);
    }
    return 0;
}

/* The object's page behind VA of V (zeroed when new), mapped in PML4, the active tree. */
static int touch_object(u64 *pml4, const struct vma *v, uptr va) {
    struct vm_object *o = v->object;
    u64 index = (v->offset + (va - v->base)) / PAGE;
    if (index >= o->slots) return -1;
    u64 *pte = pte_of(pml4, va, 1);
    if (!pte) return -1;
    if (!o->frames[index]) {
        u64 pa = frame_alloc();
        if (!pa) return -1;
        *pte = pa | P | RW;
        flush_page(va);
        memset((void *)va, 0, PAGE);
        o->frames[index] = pa;
    }
    *pte = o->frames[index] | page_flags(v->prot);
    flush_page(va);
    return 0;
}

static int keep_separate;

static long map(uptr hint, uptr size, int prot, int flags) {
    if (!size || size > VM_TOP - VM_BASE) return -22;
    size = (size + PAGE - 1) & ~(uptr)(PAGE - 1);
    uptr base = 0;
    if (flags & (DOSXC_VM_FIXED | DOSXC_VM_NOREPLACE)) {
        if ((hint & (PAGE - 1)) || !in_range(hint, size)) return -22;
        int state = overlap(hint, hint + size);
        if (state && (flags & DOSXC_VM_NOREPLACE)) return -17;
        if (state) {
            if (split(hint) || split(hint + size)) return -12;
            remove_range(hint, hint + size);
        }
        base = hint;
    } else if (hint && !(hint & (PAGE - 1)) && in_range(hint, size) && !used_anywhere(hint, hint + size)) {
        base = hint;
    } else {
        base = find_gap(size);
        if (!base) return no_memory("No address space left for a mapping");
    }
    if (insert(find(base), base, base + size, prot)) return -12;
    if ((flags & DOSXC_VM_POPULATE) && touch(current_tree(), base, base + size, prot ? prot : 3)) {
        remove_range(base, base + size);
        return no_memory("No memory to fill a new mapping");
    }
    if (!keep_separate) merge_around(base, base + size);
    return (long)base;
}

static long unmap(uptr start, uptr size) {
    if ((start & (PAGE - 1)) || !size) return -22;
    size = (size + PAGE - 1) & ~(uptr)(PAGE - 1);
    if (!in_range(start, size)) return -22;
    uptr end = start + size;
    if (!overlap(start, end)) return 0;
    if (split(start) || split(end)) return -12;
    remove_range(start, end);
    return 0;
}

static long protect(uptr start, uptr size, int prot) {
    if (start & (PAGE - 1)) return -22;
    if (!size) return 0;
    size = (size + PAGE - 1) & ~(uptr)(PAGE - 1);
    if (start + size <= VM_BASE || start >= VM_TOP) return 0;
    if (!in_range(start, size)) return -22;
    uptr end = start + size;
    if (!covered(start, end)) return -12;
    if (split(start) || split(end)) return -12;
    for (unsigned i = find(start); i < vma_count && vmas[i].base < end; ++i) vmas[i].prot = prot;
    u64 flags = page_flags(prot), *tree = current_tree();
    uptr va = start;
    for (u64 *pte; tree && (pte = next_page(tree, &va, end)); va += PAGE) *pte = (*pte & ADDRESS) | flags;
    if (tree) flush_tree(tree, start, end);
    forget_elsewhere(tree, start, end);
    merge_around(start, end);
    return 0;
}

static long discard(uptr start, uptr size) {
    if (start & (PAGE - 1)) return -22;
    if (!size) return 0;
    size = (size + PAGE - 1) & ~(uptr)(PAGE - 1);
    if (start + size <= VM_BASE || start >= VM_TOP) return 0;
    if (!in_range(start, size)) return -22;
    u64 *tree = current_tree();
    if (!tree) return 0;
    for (unsigned i = find(start); i < vma_count && vmas[i].base < start + size; ++i) {
        uptr from = vmas[i].base > start ? vmas[i].base : start, to = vmas[i].end < start + size ? vmas[i].end : start + size;
        if (vmas[i].object) clear_pages(tree, from, to);
        else release_pages(tree, from, to);
    }
    return 0;
}

/* ---- shared memory objects ---------------------------------------------- */

struct vm_object *vm_object_open(const char *name, int create, int exclusive) {
    if (init()) return 0;
    struct vm_object *free_slot = 0;
    for (int i = 0; i < MAX_OBJECTS; ++i) {
        struct vm_object *o = &objects[i];
        if (o->used && name && o->named && !rt_strcmp(o->name, name)) {
            if (exclusive) return 0;
            ++o->refs;
            return o;
        }
        if (!o->used && !free_slot) free_slot = o;
    }
    if (create && !free_slot) {
        static int warned;
        if (!warned++) event_log(DOSXC_EVENT_ERROR, "Memory", "No shared memory objects left (%d in use)", MAX_OBJECTS);
    }
    if (!create || !free_slot) return 0;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->used = 1;
    free_slot->refs = 1;
    if (name) {
        uptr n = rt_strlen(name);
        if (n >= sizeof(free_slot->name)) n = sizeof(free_slot->name) - 1;
        memcpy(free_slot->name, name, n);
        free_slot->named = 1;
        ++free_slot->refs;               /* the name keeps it too */
    }
    return free_slot;
}

int vm_object_unlink(const char *name) {
    for (int i = 0; i < MAX_OBJECTS; ++i) {
        struct vm_object *o = &objects[i];
        if (o->used && o->named && !rt_strcmp(o->name, name)) {
            o->named = 0;
            object_unref(o);
            return 0;
        }
    }
    return -2;
}

void vm_object_close(struct vm_object *o) { object_unref(o); }

u64 vm_object_size(const struct vm_object *o) { return o->size; }

int vm_object_resize(struct vm_object *o, u64 size) {
    uptr pages = (uptr)((size + PAGE - 1) / PAGE);
    if (pages > o->slots) {
        uptr slots = o->slots ? o->slots : 16;
        while (slots < pages) slots *= 2;
        u64 *frames = memory_alloc(slots * sizeof(u64), PAGE, OWNER_KERNEL, 1);
        if (!frames) return no_memory("No memory to grow a shared memory object");
        if (o->frames) {
            memcpy(frames, o->frames, o->slots * sizeof(u64));
            memory_free(o->frames, OWNER_KERNEL);
        }
        o->frames = frames;
        o->slots = slots;
    } else {
        for (uptr i = pages; i < o->slots; ++i)
            if (o->frames[i]) { frame_free(o->frames[i]); o->frames[i] = 0; }
    }
    o->size = size;
    return 0;
}

static long map_object(uptr hint, uptr size, int prot, int flags, struct vm_object *o, u64 offset) {
    if (offset & (PAGE - 1)) return -22;
    keep_separate = 1;
    long r = map(hint, size, prot, flags & ~DOSXC_VM_POPULATE);
    keep_separate = 0;
    if (r < 0) return r;
    unsigned i = find((uptr)r);
    vmas[i].object = o;
    vmas[i].offset = offset;
    ++o->refs;
    return r;
}

long vm_syscall(int op, uptr address, uptr size, long arg, int fd, u64 offset) {
    if (!current || !current_tree()) return -1;
    if (init()) return -12;
    if (!(T = table_for(current->pid, 1))) return no_memory("No mapping table left");
    switch (op) {
    case DOSXC_VM_MAP_OBJECT: {
        struct vm_object *o = files_object(fd);
        if (!o) return -19;
        return map_object(address, size, (int)arg & 7, (int)(arg >> 8), o, offset);
    }
    case DOSXC_VM_MAP: return map(address, size, (int)arg & 7, (int)(arg >> 8));
    case DOSXC_VM_UNMAP: return unmap(address, size);
    case DOSXC_VM_PROTECT: return protect(address, size, (int)arg & 7);
    case DOSXC_VM_DISCARD: return discard(address, size);
    default: return -22;
    }
}

int vm_contains(uptr address) { return address >= VM_BASE && address < VM_TOP; }

/* Can the runtime read ADDRESS without faulting?  Identity-mapped memory
   always can; a mapping only where a page is present. */
int vm_present(uptr address) {
    if (address < VM_BASE || address >= VM_TOP) return 1;
    u64 *tree = current_tree();
    if (!ready || !tree) return 0;
    u64 *e = pte_of(tree, address & ~(uptr)(PAGE - 1), 0);
    return e && (*e & P);
}

/* Called from the page fault gate; 1 means the access can be retried.  A
   process may also reach another's mappings it was given (process_grant,
   process_share): the frame comes from the owner's tree. */
static int fault(uptr address, uptr error) {
    u64 *mine = current_tree();
    if (!mine) return 0;
    uptr va = address & ~(uptr)(PAGE - 1);
    const struct vma *v = 0;
    int owner = current->pid;
    if ((T = table_for(current->pid, 0))) {
        unsigned i = find(address);
        if (i < vma_count && vmas[i].base <= address) v = &vmas[i];
    }
    if (!v) v = foreign(address, current->pid, &owner);
    if (!v) return 0;
    int prot = v->prot;
    if (!prot || ((error & 2) && !(prot & PROT_WRITE_BIT))) return 0;
    if (v->object) return !touch_object(mine, v, va);
    if (owner == current->pid) {
        if (touch(mine, va, va + PAGE, prot)) {
            rt_print("\r\nOut of memory for a mapped page.");
            return 0;
        }
        return 1;
    }
    struct process *o = process_find(owner);
    u64 *theirs = o ? aspace_root(o->space) : 0;
    if (!theirs) return 0;
    u64 *e = pte_of(theirs, va, 0);
    if (!e || !*e) {
        aspace_activate(o->space);
        int failed = touch(theirs, va, va + PAGE, prot);
        aspace_activate(current->space);
        if (failed) return 0;
        e = pte_of(theirs, va, 0);
        if (!e) return 0;
    }
    u64 *here = pte_of(mine, va, 1);
    if (!here) return 0;
    u64 old = *here;
    *here = (*e & ADDRESS) | page_flags(prot);
    flush_page(va);
    if (old) smp_tlb_flush(mine);
    return 1;
}

/* A program's fault on a present page: another processor may have caught
   the entry while this one was filling it in.  Nonzero if it now allows the
   access. */
static int settled(uptr address, uptr error) {
    u64 *mine = current_tree(), *e = mine ? pte_of(mine, address & ~(uptr)(PAGE - 1), 0) : 0;
    return e && (*e & (P | paging_user)) == (P | paging_user) && (!(error & 2) || (*e & RW));
}

int vm_fault(uptr address, uptr error) {
    sched_kernel_enter();
    if (!ready || !current || address < VM_BASE || address >= VM_TOP) return 0;
    if (error & 1) return paging_user && (error & 4) && !(error & 16) && settled(address, error);
    struct vm_table *saved = T;
    int r = fault(address, error);
    T = saved;
    return r;
}

int vm_user_ok(uptr start, uptr end, int write) {
    if (!ready || !current || start < VM_BASE || end > VM_TOP) return 0;
    struct vm_table *saved = T, *own = table_for(current->pid, 0);
    uptr at = start;
    int ok = 1;
    while (ok && at < end) {
        const struct vma *v = 0;
        int owner;
        if (own) {
            T = own;
            unsigned i = find(at);
            if (i < vma_count && vmas[i].base <= at) v = &vmas[i];
        }
        if (!v) v = foreign(at, current->pid, &owner);
        if (!v || !v->prot || (write && !(v->prot & PROT_WRITE_BIT))) ok = 0;
        else at = v->end;
    }
    T = saved;
    return ok;
}

void vm_release(int owner) {
    if (!ready) return;
    struct vm_table *t = table_for(owner, 0), *saved = T;
    if (!t) return;
    u64 *tree = tree_of(owner);
    T = t;
    for (unsigned i = 0; i < vma_count; ++i) drop_vma(tree, &vmas[i]);
    memory_free(t->v, OWNER);
    memset(t, 0, sizeof(*t));
    T = saved == t ? 0 : saved;
}

/* CHILD gets PARENT's mappings: shared memory stays shared, private pages are
   copied (through two windows in PARENT's tree, which is the active one). */
int vm_fork(struct process *child, struct process *parent) {
    if (!ready) return 0;
    struct vm_table *from = table_for(parent->pid, 0), *to;
    if (!from || !from->count) return 0;
    if (!(to = table_for(child->pid, 1)) || reserve_vmas(to, from->count)) return -12;
    memcpy(to->v, from->v, from->count * sizeof(struct vma));
    to->count = from->count;
    for (unsigned i = 0; i < to->count; ++i)
        if (to->v[i].object) ++to->v[i].object->refs;
    u64 *mine = aspace_root(parent->space), *theirs = aspace_root(child->space);
    if (!mine || !theirs || !is_active(mine)) return -12;
    u64 *source = pte_of(mine, WINDOW_FROM, 1), *target = pte_of(mine, WINDOW_TO, 1);
    if (!source || !target) return -12;
    int r = 0;
    for (unsigned i = 0; i < to->count && !r; ++i) {
        const struct vma *v = &to->v[i];
        if (v->object) continue;
        uptr va = v->base;
        for (u64 *pte; !r && (pte = next_page(mine, &va, v->end)); va += PAGE) {
            u64 pa = frame_alloc(), *copy = pa ? pte_of(theirs, va, 1) : 0;
            if (!copy) {
                if (pa) frame_free(pa);
                r = no_memory("No memory to copy a process");
                break;
            }
            *source = (*pte & ADDRESS) | P;
            *target = pa | P | RW;
            flush_page(WINDOW_FROM);
            flush_page(WINDOW_TO);
            memcpy((void *)WINDOW_TO, (const void *)WINDOW_FROM, PAGE);
            *copy = pa | (*pte & ~ADDRESS);
        }
    }
    *source = *target = 0;
    flush_page(WINDOW_FROM);
    flush_page(WINDOW_TO);
    return r;
}

void vm_swap_owner(int a, int b) {
    for (int i = 0; i < MAX_TABLES; ++i) {
        if (!tables[i].v) continue;
        if (tables[i].owner == a) tables[i].owner = b;
        else if (tables[i].owner == b) tables[i].owner = a;
    }
}

u64 vm_total(void) {
    u64 total = 0, base, length;
    for (unsigned i = 0; !memory_high(i, &base, &length); ++i) total += length;
    return total;
}

u64 vm_free(void) {
    u64 used = 0;
    for (unsigned h = 0; h < high_count; ++h) used += highs[h].used;
    return vm_total() - (used << 12) + (low_free_pages << 12);
}

#else

/* The 32-bit edition has no address space to spare for this: a mapping is
   simply memory from the program arena, allocated at once.  Enough for the
   common use, allocators asking for anonymous memory. */
struct vm_object *vm_object_open(const char *name, int create, int exclusive) { (void)name; (void)create; (void)exclusive; return 0; }
int vm_object_unlink(const char *name) { (void)name; return -2; }
void vm_object_close(struct vm_object *o) { (void)o; }
u64 vm_object_size(const struct vm_object *o) { (void)o; return 0; }
int vm_object_resize(struct vm_object *o, u64 size) { (void)o; (void)size; return -22; }

long vm_syscall(int op, uptr address, uptr size, long arg, int fd, u64 offset) {
    (void)fd; (void)offset;
    if (!current) return -1;
    if (size > (uptr)-PAGE) return -22;
    size = (size + PAGE - 1) & ~(uptr)(PAGE - 1);
    switch (op) {
    case DOSXC_VM_MAP: {
        if (!size || ((arg >> 8) & (DOSXC_VM_FIXED | DOSXC_VM_NOREPLACE))) return -22;
        void *block = memory_alloc(size, PAGE, current->pid, 1);
        return block ? (long)block : -12;
    }
    case DOSXC_VM_UNMAP:
        if ((address & (PAGE - 1)) || !size) return -22;
        return memory_free_range(address, size, current->pid) ? -22 : 0;
    case DOSXC_VM_PROTECT:
        return address & (PAGE - 1) ? -22 : 0;
    case DOSXC_VM_DISCARD:
        if (address & (PAGE - 1)) return -22;
        if (memory_owns(address, size, current->pid)) memset((void *)address, 0, size);
        return 0;
    default:
        return -22;
    }
}
int vm_fault(uptr address, uptr error) { (void)address; (void)error; return 0; }
void vm_tables_free(u64 *pml4) { (void)pml4; }
int vm_user_ok(uptr start, uptr end, int write) { (void)start; (void)end; (void)write; return 1; }
void vm_release(int owner) { (void)owner; }
int vm_fork(struct process *child, struct process *parent) { (void)child; (void)parent; return -38; }
void vm_swap_owner(int a, int b) { (void)a; (void)b; }
void vm_resume(void) {}
int vm_contains(uptr address) { (void)address; return 0; }
int vm_present(uptr address) { (void)address; return 1; }
u64 vm_total(void) { return 0; }
u64 vm_free(void) { return 0; }

#endif
