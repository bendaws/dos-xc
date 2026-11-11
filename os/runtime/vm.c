/*
 * DOS-XC /os/runtime/vm.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "runtime.h"

#if __SIZEOF_POINTER__ == 8

#define VM_BASE 0x100000000000ull
#define VM_TOP 0x7f0000000000ull
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
#define MAX_OBJECTS 256
static struct vm_object objects[MAX_OBJECTS];

struct vma {
    uptr base, end;
    int prot, owner;
    struct vm_object *object;        /* shared memory, or 0 for private memory */
    u64 offset;                      /* of base in the object */
};
static struct vma *vmas;
static unsigned vma_count;

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
    vmas = memory_alloc(MAX_VMAS * sizeof(struct vma), PAGE, OWNER, 0);
    if (!vmas) return -1;
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
static void forget_elsewhere(const u64 *owner_tree, uptr start, uptr end) {
    for (int i = 1; i < 64; ++i) {
        u64 *t = aspace_root(aspace_at(i));
        if (!t || t == owner_tree) continue;
        uptr va = start;
        int any = 0;
        for (u64 *pte; (pte = next_page(t, &va, end)); va += PAGE) { *pte = 0; any = 1; }
        if (any && is_active(t)) flush_range(start, end);
    }
}

static void release_pages(u64 *pml4, uptr start, uptr end) {
    uptr va = start;
    for (u64 *pte; (pte = next_page(pml4, &va, end)); va += PAGE) {
        frame_free(*pte & ADDRESS);
        *pte = 0;
    }
    if (is_active(pml4)) flush_range(start, end);
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

static int insert(unsigned at, uptr base, uptr end, int prot, int owner) {
    if (vma_count == MAX_VMAS) return -1;
    memmove(&vmas[at + 1], &vmas[at], (vma_count - at) * sizeof(*vmas));
    vmas[at] = (struct vma){base, end, prot, owner, 0, 0};
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
    if (insert(i + 1, address, v.end, v.prot, v.owner)) return -1;
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
        if (vmas[i].end == vmas[i + 1].base && vmas[i].prot == vmas[i + 1].prot && vmas[i].owner == vmas[i + 1].owner &&
            !vmas[i].object && !vmas[i + 1].object) {
            vmas[i].end = vmas[i + 1].end;
            erase(i + 1, 1);
        } else {
            ++i;
        }
    }
}

/* 0: nothing there.  1: only this owner's mappings.  -1: someone else's. */
static int overlap(uptr start, uptr end, int owner) {
    int found = 0;
    for (unsigned i = find(start); i < vma_count && vmas[i].base < end; ++i) {
        if (vmas[i].owner != owner) return -1;
        found = 1;
    }
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
    if (is_active(pml4)) flush_range(start, end);
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

static uptr find_gap(uptr size) {
    uptr at = VM_BASE;
    for (unsigned i = 0; i < vma_count; ++i) {
        if (vmas[i].base - at >= size) return at;
        at = vmas[i].end;
    }
    return VM_TOP - at >= size ? at : 0;
}

static int in_range(uptr address, uptr size) {
    return address >= VM_BASE && address < VM_TOP && size <= VM_TOP - address;
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

static long map(uptr hint, uptr size, int prot, int flags, int owner) {
    if (!size || size > VM_TOP - VM_BASE) return -22;
    size = (size + PAGE - 1) & ~(uptr)(PAGE - 1);
    uptr base = 0;
    if (flags & (DOSXC_VM_FIXED | DOSXC_VM_NOREPLACE)) {
        if ((hint & (PAGE - 1)) || !in_range(hint, size)) return -22;
        int state = overlap(hint, hint + size, owner);
        if (state < 0 || (state && (flags & DOSXC_VM_NOREPLACE))) return -17;
        if (state) {
            if (split(hint) || split(hint + size)) return -12;
            remove_range(hint, hint + size);
        }
        base = hint;
    } else if (hint && !(hint & (PAGE - 1)) && in_range(hint, size) && !overlap(hint, hint + size, -3)) {
        base = hint;
    } else {
        base = find_gap(size);
        if (!base) return -12;
    }
    if (insert(find(base), base, base + size, prot, owner)) return -12;
    if ((flags & DOSXC_VM_POPULATE) && touch(current_tree(), base, base + size, prot ? prot : 3)) {
        remove_range(base, base + size);
        return -12;
    }
    if (!keep_separate) merge_around(base, base + size);
    return (long)base;
}

static long unmap(uptr start, uptr size, int owner) {
    if ((start & (PAGE - 1)) || !size) return -22;
    size = (size + PAGE - 1) & ~(uptr)(PAGE - 1);
    if (!in_range(start, size)) return -22;
    uptr end = start + size;
    int state = overlap(start, end, owner);
    if (state < 0) return -22;
    if (!state) return 0;
    if (split(start) || split(end)) return -12;
    remove_range(start, end);
    return 0;
}

static long protect(uptr start, uptr size, int prot, int owner) {
    if (start & (PAGE - 1)) return -22;
    if (!size) return 0;
    size = (size + PAGE - 1) & ~(uptr)(PAGE - 1);
    if (start + size <= VM_BASE || start >= VM_TOP) return 0;
    if (!in_range(start, size)) return -22;
    uptr end = start + size;
    if (!covered(start, end)) return -12;
    if (overlap(start, end, owner) < 0) return -13;
    if (split(start) || split(end)) return -12;
    for (unsigned i = find(start); i < vma_count && vmas[i].base < end; ++i) vmas[i].prot = prot;
    u64 flags = page_flags(prot), *tree = current_tree();
    uptr va = start;
    for (u64 *pte; tree && (pte = next_page(tree, &va, end)); va += PAGE) *pte = (*pte & ADDRESS) | flags;
    flush_range(start, end);
    forget_elsewhere(tree, start, end);
    merge_around(start, end);
    return 0;
}

static long discard(uptr start, uptr size, int owner) {
    if (start & (PAGE - 1)) return -22;
    if (!size) return 0;
    size = (size + PAGE - 1) & ~(uptr)(PAGE - 1);
    if (start + size <= VM_BASE || start >= VM_TOP) return 0;
    if (!in_range(start, size)) return -22;
    if (overlap(start, start + size, owner) < 0) return -22;
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
        if (!frames) return -12;
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
    long r = map(hint, size, prot, flags & ~DOSXC_VM_POPULATE, current->pid);
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
    switch (op) {
    case DOSXC_VM_MAP_OBJECT: {
        struct vm_object *o = files_object(fd);
        if (!o) return -19;
        return map_object(address, size, (int)arg & 7, (int)(arg >> 8), o, offset);
    }
    case DOSXC_VM_MAP: return map(address, size, (int)arg & 7, (int)(arg >> 8), current->pid);
    case DOSXC_VM_UNMAP: return unmap(address, size, current->pid);
    case DOSXC_VM_PROTECT: return protect(address, size, (int)arg & 7, current->pid);
    case DOSXC_VM_DISCARD: return discard(address, size, current->pid);
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
int vm_fault(uptr address, uptr error) {
    if (!ready || !current || address < VM_BASE || address >= VM_TOP || (error & 1)) return 0;
    u64 *mine = current_tree();
    if (!mine) return 0;
    unsigned i = find(address);
    if (i == vma_count || vmas[i].base > address) return 0;
    int prot = vmas[i].prot, owner = vmas[i].owner;
    if (!prot || ((error & 2) && !(prot & PROT_WRITE_BIT))) return 0;
    uptr va = address & ~(uptr)(PAGE - 1);
    if (vmas[i].object && (owner == current->pid || process_may_access(owner, current->pid, address)))
        return !touch_object(mine, &vmas[i], va);
    if (owner == current->pid) {
        if (touch(mine, va, va + PAGE, prot)) {
            rt_print("\r\nOut of memory for a mapped page.");
            return 0;
        }
        return 1;
    }
    if (!process_may_access(owner, current->pid, address)) return 0;
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
    *here = (*e & ADDRESS) | page_flags(prot);
    flush_page(va);
    return 1;
}

int vm_user_ok(uptr start, uptr end, int write) {
    if (!ready || !current || start < VM_BASE || end > VM_TOP) return 0;
    uptr at = start;
    for (unsigned i = find(start); i < vma_count && at < end; ++i) {
        if (vmas[i].base > at) return 0;
        if (!vmas[i].prot || (write && !(vmas[i].prot & PROT_WRITE_BIT))) return 0;
        if (vmas[i].owner != current->pid && !process_may_access(vmas[i].owner, current->pid, at)) return 0;
        at = vmas[i].end;
    }
    return at >= end;
}

void vm_release(int owner) {
    if (!ready) return;
    u64 *tree = tree_of(owner);
    unsigned out = 0;
    for (unsigned i = 0; i < vma_count; ++i) {
        if (vmas[i].owner != owner) { vmas[out++] = vmas[i]; continue; }
        drop_vma(tree, &vmas[i]);
    }
    vma_count = out;
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
void vm_resume(void) {}
int vm_contains(uptr address) { (void)address; return 0; }
int vm_present(uptr address) { (void)address; return 1; }
u64 vm_total(void) { return 0; }
u64 vm_free(void) { return 0; }

#endif
