/*
 * DOS-XC /os/runtime/fork.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "runtime.h"

#if __SIZEOF_POINTER__ == 8

static const struct fork_remap *remap_over(const struct process *p, uptr base, uptr size) {
    for (int i = 0; i < p->remap_count; ++i)
        if (p->remaps[i].va < base + size && base < p->remaps[i].va + p->remaps[i].size) return &p->remaps[i];
    return 0;
}

static int add_remap(struct process *p, uptr va, uptr pa, uptr size) {
    if (p->remap_count == p->remap_capacity) {
        int capacity = p->remap_capacity ? p->remap_capacity * 2 : 128;
        struct fork_remap *bigger = memory_alloc((uptr)capacity * sizeof(*bigger), PAGE, OWNER_KERNEL, 0), *old = p->remaps;
        if (!bigger) return -1;
        if (old) memcpy(bigger, old, (uptr)p->remap_count * sizeof(*bigger));
        p->remaps = bigger;
        p->remap_capacity = capacity;
        if (old) memory_free(old, OWNER_KERNEL);
    }
    p->remaps[p->remap_count++] = (struct fork_remap){va, pa, size};
    return 0;
}

/* The address P sees FRAME at. */
static uptr address_of(const struct process *p, uptr frame) {
    for (int i = 0; i < p->remap_count; ++i)
        if (frame >= p->remaps[i].pa && frame - p->remaps[i].pa < p->remaps[i].size) return p->remaps[i].va + (frame - p->remaps[i].pa);
    return frame;
}

struct region_list { uptr *base, *size; unsigned count, capacity; };

static void count_region(uptr base, uptr size, void *context) {
    (void)base; (void)size;
    ++*(unsigned *)context;
}

static void collect(uptr base, uptr size, void *context) {
    struct region_list *l = context;
    if (l->count == l->capacity) return;
    l->base[l->count] = base;
    l->size[l->count++] = size;
}

static int library_data(const struct process *p, uptr base) {
    for (int i = 0; i < p->library_count; ++i)
        if ((uptr)p->library_data[i] == base) return 1;
    return 0;
}

/* CHILD gets a copy of every region PARENT holds (but its libraries' data,
   see libraries_fork), shown at the addresses PARENT sees it at.  Below SP
   (the calling thread's stack) nothing is copied. */
int fork_memory(struct process *child, struct process *parent, uptr sp) {
    unsigned n = 0;
    memory_each(parent->pid, count_region, &n);
    if (!n) return 0;
    struct region_list list = {0, 0, 0, n};
    if (!(list.base = memory_alloc(n * 2 * sizeof(uptr), PAGE, OWNER_KERNEL, 0))) return -DOSXC_ENOMEM;
    list.size = list.base + n;
    memory_each(parent->pid, collect, &list);
    int r = 0;
    for (unsigned i = 0; i < list.count && !r; ++i) {
        uptr base = list.base[i], size = list.size[i];
        if (remap_over(parent, base, size) || library_data(parent, base)) continue;   /* holes: kept so nothing else gets those addresses */
        uptr va = address_of(parent, base), from = 0;
        u8 *copy = memory_alloc(size, PAGE, child->pid, 0);
        if (!copy) { r = -DOSXC_ENOMEM; break; }
        if (sp - 256 >= va && sp - va < size) from = (sp - 256 - va) & ~(uptr)(PAGE - 1);
        memcpy(copy + from, (const void *)(base + from), size - from);
        if (add_remap(child, va, (uptr)copy, size)) { r = -DOSXC_ENOMEM; break; }
        for (uptr off = 0; off < size && !r; off += PAGE)
            if (aspace_map(child->space, va + off, (uptr)copy + off)) r = -DOSXC_ENOMEM;
    }
    memory_free(list.base, OWNER_KERNEL);
    return r;
}

int fork_release(struct process *p, uptr block) {
    for (int i = 0; i < p->remap_count; ++i) {
        struct fork_remap r = p->remaps[i];
        if (r.va != block) continue;
        p->remaps[i] = p->remaps[--p->remap_count];
        for (uptr off = 0; off < r.size; off += PAGE) aspace_map(p->space, r.va + off, r.va + off);
        aspace_user(p->space, r.va, r.size, 0);
        memory_free((void *)r.pa, p->pid);
        if (memory_owns(r.va, r.size, p->pid)) memory_free_range(r.va, r.size, p->pid);
        return 1;
    }
    return 0;
}

void fork_forget(struct process *p) {
    struct fork_remap *remaps = p->remaps;
    p->remaps = 0;
    p->remap_count = p->remap_capacity = 0;
    if (remaps) memory_free(remaps, OWNER_KERNEL);
}

int fork_keeper(uptr base, uptr size) {
    for (int i = 0; i < MAX_DEPTH; ++i) {
        struct process *p = process_at(i);
        if (p && remap_over(p, base, size)) return p->pid;
    }
    return 0;
}

#else

int fork_memory(struct process *child, struct process *parent, uptr sp) { (void)child; (void)parent; (void)sp; return -DOSXC_ENOSYS; }
int fork_release(struct process *p, uptr block) { (void)p; (void)block; return 0; }
void fork_forget(struct process *p) { (void)p; }
int fork_keeper(uptr base, uptr size) { (void)base; (void)size; return 0; }
int runtime_enter_frame(const uptr *frame, struct saved_context *save, const void *fx) { (void)frame; (void)save; (void)fx; return -1; }

#endif
