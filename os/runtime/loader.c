/*
 * DOS-XC /os/runtime/loader.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "runtime.h"
#include "elf.h"
#include "net.h"

#define R_NONE 0
#define R_RELATIVE 8
#if __SIZEOF_POINTER__ == 8
#define R_IRELATIVE 37
#else
#define R_IRELATIVE 42
#endif

struct image { uptr base, entry, phdr, phnum, stack_size; u32 abi; uptr dynamic, bias; int interp; uptr tls_vaddr, tls_filesz, tls_memsz, tls_align; };

static struct process processes[MAX_DEPTH];
struct process *current;
static struct process *root;

struct process *process_root(void) { return root; }
static int next_pid = 1;
static u8 header[4096];
static Phdr phdrs[MAX_PHDRS];
static u8 note[1024];

static void complain(const char *path, const char *message) {
    char posix[PATH_MAX_RT];
    path_posix(path, posix, sizeof(posix));
    rt_print(posix);
    rt_print(": ");
    rt_print(message);
    rt_print("\r\n");
}

static int read_at(const char *path, u64 offset, void *buffer, uptr size, u64 file_size) {
    if (offset > file_size || size > file_size - offset) return -1;
    return size ? fs_read_at(path, (u32)offset, buffer, (u32)size) : 0;
}

/* The ABI version from the program's DOS-XC note, or 0 without one. */
static u32 dosxc_note_abi(const char *path, const Phdr *ph, u64 file_size) {
    uptr size = ph->filesz < sizeof(note) ? (uptr)ph->filesz : sizeof(note);
    if (read_at(path, ph->offset, note, size, file_size)) return 0;
    for (uptr at = 0; at + 12 <= size;) {
        u32 namesz = *(u32 *)(note + at), descsz = *(u32 *)(note + at + 4), type = *(u32 *)(note + at + 8);
        uptr name = at + 12, desc = name + ((namesz + 3) & ~3u), next = desc + ((descsz + 3) & ~3u);
        if (next > size || next <= at) return 0;
        if (namesz == 7 && type == DOSXC_NOTE_ABI && !memcmp(note + name, DOSXC_NOTE_NAME, 7) &&
            descsz >= 4 && *(u32 *)(note + desc) >= 1)
            return *(u32 *)(note + desc);
        at = next;
    }
    return 0;
}

static int relocate(const char *path, uptr bias, const Dyn *dynamic) {
    uptr rela = 0, rela_size = 0, rel = 0, rel_size = 0, relr = 0, relr_size = 0;
    for (const Dyn *d = dynamic; d->tag; ++d) {
        switch (d->tag) {
        case 7: rela = bias + (uptr)d->value; break;
        case 8: rela_size = (uptr)d->value; break;
        case 17: rel = bias + (uptr)d->value; break;
        case 18: rel_size = (uptr)d->value; break;
        case 36: relr = bias + (uptr)d->value; break;
        case 35: relr_size = (uptr)d->value; break;
        }
    }
    for (int pass = 0; pass < 2; ++pass) {
        for (uptr at = 0; at + sizeof(Rela) <= rela_size; at += sizeof(Rela)) {
            const Rela *r = (const Rela *)(rela + at);
            uptr *where = (uptr *)(bias + (uptr)r->offset);
            u32 type = R_TYPE(r->info);
            if (type == R_NONE) continue;
            if (type == R_RELATIVE) { if (!pass) *where = bias + (uptr)r->addend; }
            else if (type == R_IRELATIVE) { if (pass) *where = ((uptr(*)(void))(bias + (uptr)r->addend))(); }
            else goto unsupported;
        }
        for (uptr at = 0; at + sizeof(Rel) <= rel_size; at += sizeof(Rel)) {
            const Rel *r = (const Rel *)(rel + at);
            uptr *where = (uptr *)(bias + r->offset);
            u32 type = R_TYPE(r->info);
            if (type == R_NONE) continue;
            if (type == R_RELATIVE) { if (!pass) *where += bias; }
            else if (type == R_IRELATIVE) { if (pass) *where = ((uptr(*)(void))(bias + *where))(); }
            else goto unsupported;
        }
    }
    uptr *where = 0;
    for (uptr at = 0; at + sizeof(uptr) <= relr_size; at += sizeof(uptr)) {
        uptr entry = *(const uptr *)(relr + at);
        if (!(entry & 1)) {
            where = (uptr *)(bias + entry);
            *where++ += bias;
        } else if (where) {
            for (unsigned i = 0; (entry >>= 1); ++i)
                if (entry & 1) where[i] += bias;
            where += 8 * sizeof(uptr) - 1;
        }
    }
    return 0;
unsupported:
    complain(path, "contains relocations that need a dynamic linker (link it with -static-pie).");
    return -1;
}

static int load_image(struct process *p, const char *path, struct image *out) {
    struct fat32_info info;
    if (fs_stat(path, &info)) return -DOSXC_ENOENT;
    u64 size = info.size;
    uptr first = size < sizeof(header) ? (uptr)size : sizeof(header);
    if (read_at(path, 0, header, first, size)) return -DOSXC_EIO;
    const Ehdr *e = (const Ehdr *)header;
    if (first < sizeof(Ehdr) || memcmp(e->ident, "\177ELF", 4) || e->ident[5] != 1 || e->ident[6] != 1)
        return -DOSXC_ENOEXEC;
    if (e->ident[4] != ELF_CLASS || e->machine != ELF_MACHINE) {
        complain(path, e->ident[4] == 2 ? "is a 64-bit (x86-64) program; boot the x64 edition of DOS-XC to run it."
                                        : "is a 32-bit (i386) program; boot the x86 edition of DOS-XC to run it.");
        return -DOSXC_ENOEXEC;
    }
    if (e->type != ET_DYN && e->type != ET_EXEC) { complain(path, "is not an executable ELF file."); return -DOSXC_ENOEXEC; }
    if (e->phentsize != sizeof(Phdr) || !e->phnum || e->phnum > MAX_PHDRS ||
        read_at(path, e->phoff, phdrs, e->phnum * sizeof(Phdr), size)) {
        complain(path, "has invalid ELF program headers.");
        return -DOSXC_ENOEXEC;
    }
    out->interp = 0;
    uptr low = (uptr)-1, high = 0, align = PAGE, phdr = 0, stack = 0;
    u32 abi = 0;
    const Phdr *dynamic = 0;
    for (unsigned i = 0; i < e->phnum; ++i) {
        const Phdr *ph = &phdrs[i];
        if (ph->type == PT_INTERP) {
            char interp[64];
            uptr n = ph->filesz < sizeof(interp) - 1 ? (uptr)ph->filesz : sizeof(interp) - 1;
            if (read_at(path, ph->offset, interp, n, size)) return -DOSXC_EIO;
            interp[n] = 0;
            while (n && !interp[n - 1]) --n;
            if (n < 10 || !rt_equal_nocase(interp + n - 10, "ELFAPI.DLL")) {
                complain(path, "is dynamically linked for another operating system.");
                return -DOSXC_ENOEXEC;
            }
            out->interp = 1;
        }
        if (ph->type == PT_TLS) {
#if __SIZEOF_POINTER__ == 8
            out->tls_vaddr = (uptr)ph->vaddr;
            out->tls_filesz = (uptr)ph->filesz;
            out->tls_memsz = (uptr)ph->memsz;
            out->tls_align = (uptr)ph->align;
#else
            complain(path, "uses thread-local storage, which the 32-bit edition does not provide.");
            return -DOSXC_ENOEXEC;
#endif
        }
        if (ph->type == PT_NOTE && !abi) abi = dosxc_note_abi(path, ph, size);
        if (ph->type == PT_DYNAMIC) dynamic = ph;
        if (ph->type == PT_PHDR) phdr = (uptr)ph->vaddr;
        if (ph->type == PT_GNU_STACK) stack = (uptr)ph->memsz;
        if (ph->type != PT_LOAD || !ph->memsz) continue;
        if (ph->filesz > ph->memsz || ph->vaddr + ph->memsz < ph->vaddr) { complain(path, "has an invalid segment."); return -DOSXC_ENOEXEC; }
        if ((uptr)ph->vaddr < low) low = (uptr)ph->vaddr & ~(uptr)(PAGE - 1);
        if ((uptr)(ph->vaddr + ph->memsz) > high) high = (uptr)(ph->vaddr + ph->memsz);
        if (ph->align > align && ph->align <= 0x200000) align = (uptr)ph->align;
    }
    if (!abi) {
        complain(path, "is not a DOS-XC program (it has no .note.dosxc ABI note). Link it with crt0.o from the DOS-XC C library.");
        return -DOSXC_ENOEXEC;
    }
    if (abi > DOSXC_ABI_VERSION) {
        complain(path, "needs a newer version of DOS-XC (its program ABI is not supported).");
        return -DOSXC_ENOEXEC;
    }
    if (e->type == ET_EXEC) {
        complain(path, "is linked at a fixed address; DOS-XC programs must be position independent (-static-pie).");
        return -DOSXC_ENOEXEC;
    }
    if (high <= low) return -DOSXC_ENOEXEC;
    high = (high + PAGE - 1) & ~(uptr)(PAGE - 1);
    u8 *base = memory_alloc(high - low, align, p->pid, 1);
    if (!base) { complain(path, "does not fit in free memory."); return -DOSXC_ENOMEM; }
    uptr bias = (uptr)base - low;
    for (unsigned i = 0; i < e->phnum; ++i) {
        const Phdr *ph = &phdrs[i];
        if (ph->type == PT_LOAD && ph->filesz && read_at(path, ph->offset, (void *)(bias + (uptr)ph->vaddr), (uptr)ph->filesz, size)) {
            complain(path, "could not be read from disk.");
            return -DOSXC_EIO;
        }
    }
    out->dynamic = dynamic ? bias + (uptr)dynamic->vaddr : 0;
    out->bias = bias;
    out->base = (uptr)base;
    out->entry = bias + (uptr)e->entry;
    out->phdr = phdr ? bias + phdr : bias + low + (uptr)e->phoff;
    out->phnum = e->phnum;
    out->stack_size = stack;
    out->abi = abi;
    p->image_base = (uptr)base;
    p->image_size = high - low;
    return 0;
}

/* System V initial stack: argc, argv[], 0, envp[], 0, auxv[], strings. */
static uptr build_stack(uptr base, uptr size, const char *execfn, char *const argv[], char *const envp[],
                        const struct image *image, const struct link_result *link) {
    uptr argc = 0, envc = 0, strings = rt_strlen(execfn) + 1 + 16;
    uptr lists = link ? (uptr)(link->init_count + link->fini_count + 2) * sizeof(uptr) : 0;
    strings += lists + sizeof(uptr);
    for (; argv[argc]; ++argc) strings += rt_strlen(argv[argc]) + 1;
    for (; envp && envp[envc]; ++envc) strings += rt_strlen(envp[envc]) + 1;
    if (strings > size / 4 || argc > 65536 || envc > 65536) return 0;
    uptr top = base + size;
    char *text = (char *)((top - strings) & ~(uptr)15);
    enum { AUX = 16 };
    uptr words = 1 + argc + 1 + envc + 1 + 2 * AUX;
    uptr sp = ((uptr)text - words * sizeof(uptr)) & ~(uptr)15;
    uptr *w = (uptr *)sp;
    *w++ = argc;
    char *cursor = text;
    u64 seed = cpu_nanoseconds() ^ (u64)sp;
    for (int i = 0; i < 16; ++i) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        cursor[i] = (char)(seed >> 56);
    }
    uptr random = (uptr)cursor;
    cursor += 16;
    for (uptr i = 0; i < argc; ++i) {
        uptr n = rt_strlen(argv[i]) + 1;
        memcpy(cursor, argv[i], n);
        *w++ = (uptr)cursor;
        cursor += n;
    }
    *w++ = 0;
    for (uptr i = 0; i < envc; ++i) {
        uptr n = rt_strlen(envp[i]) + 1;
        memcpy(cursor, envp[i], n);
        *w++ = (uptr)cursor;
        cursor += n;
    }
    *w++ = 0;
    uptr name = (uptr)cursor;
    memcpy(cursor, execfn, rt_strlen(execfn) + 1);
    cursor += rt_strlen(execfn) + 1;
    uptr inits = 0, finis = 0;
    if (link) {
        uptr *list = (uptr *)(((uptr)cursor + sizeof(uptr) - 1) & ~(uptr)(sizeof(uptr) - 1));
        inits = (uptr)list;
        for (int i = 0; i < link->init_count; ++i) *list++ = link->inits[i];
        *list++ = 0;
        finis = (uptr)list;
        for (int i = 0; i < link->fini_count; ++i) *list++ = link->finis[i];
        *list++ = 0;
    }
    extern struct dosxc_api runtime_api;
    int table = image->abi == DOSXC_ABI_TABLE;
    uptr aux[AUX * 2] = {
        3, image->phdr, 4, sizeof(Phdr), 5, image->phnum, 6, PAGE, 7, 0, 9, image->entry,
        25, random, 31, name, table ? DOSXC_AT_API : 1, table ? (uptr)&runtime_api : 0, DOSXC_AT_BASE, image->base,
        23, 0, inits ? DOSXC_AT_INITS : 1, inits, finis ? DOSXC_AT_FINIS : 1, finis,
        DOSXC_AT_STACK_BASE, base, DOSXC_AT_STACK_SIZE, size, 0, 0,
    };
    memcpy(w, aux, sizeof(aux));
    return sp;
}

static int find_program(struct process *parent, const char *path, char *canonical) {
    int r = path_resolve(parent, path, canonical);
    if (r) return r;
    struct fat32_info info;
    if (!fs_stat(canonical, &info))
        return (info.attributes & DOSXC_ATTR_DIRECTORY) ? -DOSXC_EACCES : 0;
    uptr n = rt_strlen(canonical), at = n;
    while (at && canonical[at - 1] != '\\' && canonical[at - 1] != '.') --at;
    if ((at && canonical[at - 1] == '.') || n + 5 >= PATH_MAX_RT) return -DOSXC_ENOENT;
    rt_strcpy(canonical + n, ".EXE");
    if (!fs_stat(canonical, &info) && !(info.attributes & DOSXC_ATTR_DIRECTORY)) return 0;
    return -DOSXC_ENOENT;
}

/* Load a program without running it.  On failure nothing remains of it. */
static int process_prepare(struct process *parent, const char *path, char *const argv[], char *const envp[],
                           const char *output, int append, struct process **out, uptr *entry, uptr *stack_pointer) {
    char canonical[PATH_MAX_RT], posix[PATH_MAX_RT];
    int r = find_program(parent, path, canonical);
    if (r) return r;
    u8 magic[4];
    if ((r = files_is_executable(canonical, magic))) return r;
    if (memcmp(magic, "\177ELF", 4)) {
        if ((magic[0] == 'M' && magic[1] == 'Z') || !memcmp(magic, "BPCM", 4) || parent)
            complain(canonical, "is not a 32/64-bit program and cannot be started from one.");
        return -DOSXC_ENOEXEC;
    }
    struct process *p = 0;
    for (unsigned i = 0; i < MAX_DEPTH && !p; ++i)
        if (!processes[i].used) p = &processes[i];
    if (!p) return -DOSXC_EAGAIN;
    memset(p, 0, sizeof(*p));
    p->used = 1;
    p->pid = next_pid++;
    if (next_pid > 0x7fff0000) next_pid = 1;
    p->parent = parent;
    vc_attach(p, parent ? parent->vc : 0);
    rt_strcpy(p->path, canonical);
#if __SIZEOF_POINTER__ == 8
    if (!(p->space = aspace_new())) {
        vc_detach(p);
        p->used = 0;
        return -DOSXC_ENOMEM;
    }
#endif
    if ((r = files_setup(p, parent, output, append))) {
        vc_detach(p);
        aspace_free(p->space);
        p->used = 0;
        return r;
    }
    struct image image;
    uptr stack_size = 0, stack = 0, sp = 0;
    static struct link_result links[MAX_DEPTH];
    struct link_result *link = 0;
    memset(&image, 0, sizeof(image));
    r = load_image(p, canonical, &image);
    if (!r && image.tls_memsz)
        r = tls_add(p, 0, image.bias + image.tls_vaddr, image.tls_filesz, image.tls_memsz, image.tls_align, image.tls_vaddr, 1);
    if (!r && image.dynamic) {
        struct elf_object program;
        elf_parse_dynamic(&program, image.bias, (const Dyn *)image.dynamic);
        if (program.needed_count || image.interp) {
            struct process *saved = current;
            current = p;
            libraries_activate(p);
            link = &links[p - processes];
            r = dynamic_link(p, canonical, &program, link);
            current = saved;
            libraries_activate(saved);
            if (r < 0 && r != -DOSXC_ENOMEM && r != -DOSXC_ENOENT) r = -DOSXC_ENOEXEC;
        } else if (relocate(canonical, image.bias, (const Dyn *)image.dynamic)) {
            r = -DOSXC_ENOEXEC;
        }
    }
    if (!r) {
        stack_size = image.stack_size ? image.stack_size : memory_total() >= (512u << 20) ? 16u << 20 : 4u << 20;
        if (stack_size < 65536) stack_size = 65536;
        stack_size = (stack_size + PAGE - 1) & ~(uptr)(PAGE - 1);
        stack = (uptr)memory_alloc(stack_size, PAGE, p->pid, 0);
        if (!stack) {
            complain(canonical, "cannot be given a stack: out of memory.");
            r = -DOSXC_ENOMEM;
        }
    }
    if (!r) {
        struct process *saved = current;
        current = p;
        path_posix(canonical, posix, sizeof(posix));
        current = saved;
        char *default_argv[2] = {posix, 0};
        sp = build_stack(stack, stack_size, posix, argv && argv[0] ? argv : default_argv,
                         envp ? envp : environment_master(), &image, link);
        if (!sp) r = -DOSXC_E2BIG;
    }
    if (r) {
        process_finish(p);
        return r;
    }
    *out = p;
    *entry = image.entry;
    *stack_pointer = sp;
    return 0;
}

int process_spawn(struct process *parent, const char *path, char *const argv[], char *const envp[],
                  const char *output, int append) {
    struct process *p;
    uptr entry, sp;
    int r = process_prepare(parent, path, argv, envp, output, append, &p, &entry, &sp);
    if (r) return r;
    struct process *saved = current;
    current = p;
    sched_set_leaf(p);
    if (!parent) root = p;
    cpu_install_idt();
    libraries_activate(p);
    r = runtime_enter(entry, sp, &p->saved, cpu_has_sse());
    cpu_restore_idt();
    current = saved;
    sched_set_leaf(saved);
    libraries_activate(saved);
    process_finish(p);
    return r;
}

/* Start a program beside its parent; returns its pid. */
int process_background(struct process *parent, const char *path, char *const argv[], struct vconsole *console) {
    struct process *p;
    uptr entry, sp;
    int r = process_prepare(parent, path, argv, 0, 0, 0, &p, &entry, &sp);
    if (r) return r;
    if (console) { vc_detach(p); vc_attach(p, console); }
    r = sched_start(p, entry, sp);
    if (r < 0) process_finish(p);
    return r;
}

int process_list(struct dosxc_task_info *out, unsigned capacity) {
    unsigned n = 0;
    for (unsigned i = 0; i < MAX_DEPTH; ++i) {
        struct process *p = &processes[i];
        if (!p->used) continue;
        if (n < capacity) {
            struct dosxc_task_info *t = &out[n];
            memset(t, 0, sizeof(*t));
            t->pid = p->pid;
            t->parent = p->parent ? p->parent->pid : 0;
            t->background = (dosxc_u32)p->background;
            t->memory = memory_owned(p->pid);
            t->cpu_ns = p->cpu_ns;
            rt_strcpy(t->path, p->path);
        }
        ++n;
    }
    return (int)n;
}

int process_alive(int pid) {
    for (unsigned i = 0; i < MAX_DEPTH; ++i)
        if (processes[i].used && processes[i].pid == pid) return 1;
    return 0;
}

void process_kill_console(struct vconsole *console) {
    for (unsigned i = 0; i < MAX_DEPTH; ++i)
        if (processes[i].used && processes[i].vc == console) processes[i].killed = 1;
}

/* End PID and the children it is waiting for at their next safe point. */
int process_kill(int pid) {
    struct process *target = 0;
    for (unsigned i = 0; i < MAX_DEPTH; ++i)
        if (processes[i].used && processes[i].pid == pid) target = &processes[i];
    if (!target) return -DOSXC_ESRCH;
    if (target == root || target->pid == display_owner()) return -DOSXC_EPERM;
    target->killed = 1;
    for (int changed = 1; changed;) {
        changed = 0;
        for (unsigned i = 0; i < MAX_DEPTH; ++i) {
            struct process *p = &processes[i];
            if (p->used && !p->killed && !p->background && p->parent && p->parent->killed) { p->killed = 1; changed = 1; }
        }
    }
    return 0;
}

struct process *process_find(int pid) {
    for (unsigned i = 0; i < MAX_DEPTH; ++i)
        if (processes[i].used && processes[i].pid == pid) return &processes[i];
    return 0;
}

#define MAX_SHARES 64
static struct { int owner, pid; uptr start, end; } shares[MAX_SHARES];

int process_may_access(int owner, int pid, uptr address) {
    if (owner == pid) return 1;
    struct process *o = process_find(owner);
    if (o)
        for (int i = 0; i < 4; ++i)
            if (o->grantees[i] == pid) return 1;
    for (int i = 0; i < MAX_SHARES; ++i)
        if (shares[i].owner == owner && shares[i].pid == pid && address >= shares[i].start && address < shares[i].end) return 1;
    return 0;
}

void process_memory_added(uptr base, uptr size, int owner) {
    struct process *p = process_find(owner);
    if (!p) return;
    aspace_user(p->space, base, size, 2);
    for (int i = 0; i < 4; ++i) {
        struct process *g = p->grantees[i] ? process_find(p->grantees[i]) : 0;
        if (g) aspace_user(g->space, base, size, 2);
    }
}

void process_memory_removed(uptr base, uptr size) { aspace_user_all(base, size, 0); }

struct region_list { uptr base[256], size[256]; unsigned count; };
static void collect_region(uptr base, uptr size, void *context) {
    struct region_list *l = context;
    if (l->count < 256) { l->base[l->count] = base; l->size[l->count] = size; ++l->count; }
}

int process_grant(struct process *p, int pid) {
    struct process *g = process_find(pid);
    if (!g || g == p) return -DOSXC_EINVAL;
    int slot = -1;
    for (int i = 0; i < 4; ++i) {
        if (p->grantees[i] == pid) return 0;
        if (!p->grantees[i] || !process_find(p->grantees[i])) slot = i;
    }
    if (slot < 0) return -DOSXC_EBUSY;
    p->grantees[slot] = pid;
    static struct region_list list;
    list.count = 0;
    memory_each(p->pid, collect_region, &list);
    for (unsigned i = 0; i < list.count; ++i) aspace_user(g->space, list.base[i], list.size[i], 2);
    return 0;
}

int process_share(int owner, uptr start, uptr end, int pid) {
    struct process *o = process_find(owner), *p = process_find(pid);
    if (!o || !p) return -DOSXC_EINVAL;
    int slot = -1;
    for (int i = 0; i < MAX_SHARES; ++i) {
        if (shares[i].owner == owner && shares[i].pid == pid && shares[i].start == start) return 0;
        if (slot < 0 && (!shares[i].owner || !process_find(shares[i].owner) || !process_find(shares[i].pid))) slot = i;
    }
    if (slot < 0) return -DOSXC_EBUSY;
    shares[slot].owner = owner;
    shares[slot].pid = pid;
    shares[slot].start = start;
    shares[slot].end = end;
    if ((u64)end <= 0x100000000ull) aspace_user(p->space, start, end - start, 2);
    return 0;
}

/* Release what a finished process owned. */
void process_finish(struct process *p) {
    sched_stop_threads(p);
    display_process_finished(p->pid);
    speaker_process_finished(p->pid);
    audio_process_finished(p->pid);
    sched_process_died(p);
    for (unsigned i = 0; i < MAX_DEPTH; ++i)
        if (processes[i].used && processes[i].parent == p) processes[i].parent = 0;
    vc_detach(p);
    libraries_release(p);
    files_release(p);
    vm_release(p->pid);
    memory_free_owner(p->pid);
#if __SIZEOF_POINTER__ == 8
    if (current == p || !current) aspace_activate(0);
    aspace_free(p->space);
    p->space = 0;
#endif
    if (p == root) root = 0;
    p->used = 0;
}

void process_exit(int wait_status) {
    if (!current) for (;;) __asm__ volatile("cli; hlt");
    sched_thread_leave(wait_status);
    runtime_leave(&current->saved, wait_status & 0xffff);
}

#ifdef __x86_64__
extern char __runtime_bss_end[];

/* Run the program's handler at the point of the fault, if it has one and the
   fault is in its code: a bad pointer given to a system call stays fatal. */
static int deliver_fault(uptr *frame, uptr *regs, int signal, uptr vector, uptr cr2) {
    static const u8 order[15] = {13, 11, 14, 12, 9, 8, 10, 0, 1, 2, 3, 4, 5, 6, 7};
    struct process *p = current;
    uptr ip = frame[2], error = frame[1];

    if (!p || !p->fault_entry || !(p->fault_mask & (1u << signal))) return 0;
    if (ip >= 0x4400000 && ip < (uptr)__runtime_bss_end) return 0;
    if (sched_fault_enter()) return 0;
    uptr sp = (frame[5] - 128 - sizeof(struct dosxc_fault)) & ~(uptr)63;
    struct dosxc_fault *f = (struct dosxc_fault *)sp;
    f->signal = (dosxc_u64)signal;
    f->address = vector == 14 ? cr2 : ip;
    f->code = signal == 11 ? ((vector == 14 && (error & 1)) ? 2 : 1) : signal == 4 ? 2 : signal == 8 ? 1 : 2;
    for (int i = 0; i < 15; ++i) f->regs[order[i]] = regs[i];
    f->regs[15] = frame[5];
    f->regs[16] = ip;
    f->regs[17] = frame[4];
    for (int i = 18; i < 23; ++i) f->regs[i] = 0;
    f->regs[19] = error;
    f->regs[20] = vector;
    f->regs[22] = cr2;
    *(uptr *)(sp - 8) = 0;
    frame[2] = p->fault_entry;
    frame[5] = sp - 8;
    frame[4] &= ~(uptr)0x400;       /* direction flag */
    regs[5] = sp;
    return 1;
}
#endif

void runtime_exception(uptr *frame, uptr *regs) {
    static const char *const names[32] = {
        "Divide error", "Debug exception", "Non-maskable interrupt", "Breakpoint", "Overflow",
        "Bound range exceeded", "Invalid opcode", "FPU not available", "Double fault",
        "Coprocessor segment overrun", "Invalid TSS", "Segment not present", "Stack fault",
        "General protection fault", "Page fault", "Reserved exception", "x87 floating-point error",
        "Alignment check", "Machine check", "SIMD floating-point error", "Virtualization exception",
        "Control protection fault"};
    uptr vector = frame[0] & 31, error = frame[1], ip = frame[2];
    drivers_fault(vector, ip);
    int signal = vector == 0 || vector == 16 || vector == 19 ? 8
               : vector == 6 ? 4 : vector == 1 || vector == 3 ? 5 : vector == 17 ? 7 : 11;
#ifdef __x86_64__
    uptr fault_address = 0;
    __asm__ volatile("mov %%cr2,%0" : "=r"(fault_address));
    if (deliver_fault(frame, regs, signal, vector, fault_address)) return;
#else
    (void)regs;
#endif
    if (!current || display_owner() == current->pid) display_text();
    rt_print("\r\n");
    if (current) {
        char posix[PATH_MAX_RT];
        path_posix(current->path, posix, sizeof(posix));
        rt_print(posix);
        rt_print(": ");
    }
    rt_print(names[vector] ? names[vector] : "Processor exception");
    rt_print(" at ");
    rt_print_hex(ip);
    uptr offset;
    const char *library = library_at(ip, &offset);
    if (current && ip >= current->image_base && ip < current->image_base + current->image_size) {
        rt_print(" (image+");
        rt_print_hex(ip - current->image_base);
        rt_print(")");
    } else if (library) {
        char posix[PATH_MAX_RT];
        path_posix(library, posix, sizeof(posix));
        rt_print(" (");
        rt_print(posix);
        rt_print("+");
        rt_print_hex(offset);
        rt_print(")");
    }
    if (vector == 14) {
        uptr cr2;
        __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
        rt_print(", address ");
        rt_print_hex(cr2);
    }
    if (vector == 13 || vector == 14) {
        rt_print(", error ");
        rt_print_hex(error);
    }
    rt_print("\r\n");
#ifdef __x86_64__
    /* Words on the stack that point into the program or its libraries: most
       are return addresses, which is enough to see roughly how it got here. */
    if (current) {
        uptr sp = frame[5] & ~(uptr)7;
        int shown = 0;
        for (int i = 0; i < 2048 && shown < 24; ++i) {
            uptr at = sp + (uptr)i * 8;
            if ((i == 0 || !(at & (PAGE - 1))) && !vm_present(at)) break;
            uptr value = *(uptr *)at, where;
            const char *in = library_at(value, &where);
            if (!in && value >= current->image_base && value < current->image_base + current->image_size) {
                in = current->path;
                where = value - current->image_base;
            }
            if (!in) continue;
            char posix[PATH_MAX_RT];
            path_posix(in, posix, sizeof(posix));
            rt_print("  ");
            rt_print(posix);
            rt_print("+");
            rt_print_hex(where);
            rt_print("\r\n");
            ++shown;
        }
    }
#endif
    if (!current) {
        rt_print("The system is halted.\r\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    process_exit(DOSXC_SIGNALED(signal));
}
