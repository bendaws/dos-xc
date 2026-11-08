/*
 * DOS-XC /runtime/dynamic.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * Shared libraries (.so).  A library is loaded once, at an address chosen
 * like any program image, and its code is shared by every process that
 * uses it.  Its writable pages (data, bss, GOT) are private: each process
 * gets its own copy, made from a pristine template, relocated for that
 * process and mapped at the library's address while the process runs
 * (paging.c).  Programs nest strictly, so the mappings only change when a
 * process starts or ends.
 *
 * Libraries are found by name (DT_NEEDED) in the requesting object's
 * DT_RUNPATH/DT_RPATH folders, the program's own folder and \SYSTEM\LIB.
 * All symbols are bound when the program starts; lookups search the
 * program, then its libraries breadth-first.
 */

#include "runtime.h"
#include "elf.h"

#define MAX_LIBRARIES 32

struct library {
    int used, refs;
    char path[PATH_MAX_RT];
    struct elf_object object;
    uptr image, image_size;
    uptr data_start, data_end;        /* writable pages */
    u8 *template;
    struct library *needed[MAX_NEEDED];
    int needed_count;
};

static struct library libraries[MAX_LIBRARIES];
static u8 header[4096];
static Phdr phdrs[MAX_PHDRS];

static void complain(const char *path, const char *message, const char *detail) {
    char posix[PATH_MAX_RT];
    path_posix(path, posix, sizeof(posix));
    rt_print(posix);
    rt_print(": ");
    rt_print(message);
    if (detail) rt_print(detail);
    rt_print("\r\n");
}

static int read_at(const char *path, u64 offset, void *buffer, uptr size, u64 file_size) {
    if (offset > file_size || size > file_size - offset) return -1;
    return size ? fs_read_at(path, (u32)offset, buffer, (u32)size) : 0;
}

/* ---- dynamic sections -------------------------------------------------- */

int elf_parse_dynamic(struct elf_object *o, uptr bias, const Dyn *dynamic) {
    memset(o, 0, sizeof(*o));
    o->bias = bias;
    o->dynamic = dynamic;
    uptr runpath = 0, rpath = 0;
    for (const Dyn *d = dynamic; d && d->tag; ++d) {
        uptr v = (uptr)d->value;
        switch (d->tag) {
        case DT_NEEDED: if (o->needed_count < MAX_NEEDED) o->needed[o->needed_count++] = v; break;
        case DT_HASH: o->hash = (const u32 *)(bias + v); break;
        case DT_GNU_HASH: o->gnu_hash = (const u32 *)(bias + v); break;
        case DT_STRTAB: o->strtab = (const char *)(bias + v); break;
        case DT_SYMTAB: o->symtab = (const Sym *)(bias + v); break;
        case DT_RELA: o->rela = bias + v; break;
        case DT_RELASZ: o->rela_size = v; break;
        case DT_REL: o->rel = bias + v; break;
        case DT_RELSZ: o->rel_size = v; break;
        case DT_RELR: o->relr = bias + v; break;
        case DT_RELRSZ: o->relr_size = v; break;
        case DT_JMPREL: o->jmprel = bias + v; break;
        case DT_PLTRELSZ: o->jmprel_size = v; break;
        case DT_PLTREL: o->plt_rela = v == DT_RELA; break;
        case DT_INIT: o->init = bias + v; break;
        case DT_FINI: o->fini = bias + v; break;
        case DT_INIT_ARRAY: o->init_array = bias + v; break;
        case DT_INIT_ARRAYSZ: o->init_array_size = v; break;
        case DT_FINI_ARRAY: o->fini_array = bias + v; break;
        case DT_FINI_ARRAYSZ: o->fini_array_size = v; break;
        case DT_RUNPATH: runpath = v; break;
        case DT_RPATH: rpath = v; break;
        case DT_TEXTREL: o->textrel = 1; break;
        case DT_FLAGS: if (v & DF_TEXTREL) o->textrel = 1; break;
        }
    }
    if (o->strtab) {
        for (int i = 0; i < o->needed_count; ++i) o->needed[i] = (uptr)(o->strtab + o->needed[i]);
        if (runpath || rpath) o->runpath = o->strtab + (runpath ? runpath : rpath);
    }
    return 0;
}

/* ---- symbols -------------------------------------------------------------- */

static u32 gnu_hash(const char *s) {
    u32 h = 5381;
    for (; *s; ++s) h = h * 33 + (u8)*s;
    return h;
}

static u32 sysv_hash(const char *s) {
    u32 h = 0;
    for (; *s; ++s) {
        h = (h << 4) + (u8)*s;
        u32 g = h & 0xf0000000u;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

static int defines(const struct elf_object *o, const Sym *s, const char *name) {
    u8 bind = ELF_BIND(s->info);
    return s->shndx != 0 && (bind == 1 || bind == 2) && ELF_TYPE(s->info) != 3 && !rt_strcmp(o->strtab + s->name, name);
}

static const Sym *find_symbol(const struct elf_object *o, const char *name, u32 gh, u32 sh) {
    if (!o->symtab || !o->strtab) return 0;
    if (o->gnu_hash) {
        const u32 *h = o->gnu_hash;
        u32 buckets = h[0], offset = h[1], bloom_words = h[2];
        const uptr *bloom = (const uptr *)(h + 4);
        const u32 *bucket = (const u32 *)(bloom + bloom_words), *chain = bucket + buckets;
        if (!buckets) return 0;
        u32 i = bucket[gh % buckets];
        if (i < offset) return 0;
        for (;; ++i) {
            u32 c = chain[i - offset];
            if ((c | 1) == (gh | 1) && defines(o, &o->symtab[i], name)) return &o->symtab[i];
            if (c & 1) return 0;
        }
    }
    if (o->hash) {
        u32 buckets = o->hash[0];
        const u32 *bucket = o->hash + 2, *chain = bucket + buckets;
        for (u32 i = buckets ? bucket[sh % buckets] : 0; i; i = chain[i])
            if (defines(o, &o->symtab[i], name)) return &o->symtab[i];
    }
    return 0;
}

/* The scope of a process: its program, then its libraries breadth-first. */
struct scope { const struct elf_object *objects[MAX_LIBRARIES + 1]; int count; };

static uptr lookup(const struct scope *scope, const char *name, int skip_first, const Sym **found) {
    u32 gh = gnu_hash(name), sh = sysv_hash(name);
    for (int i = skip_first ? 1 : 0; i < scope->count; ++i) {
        const Sym *s = find_symbol(scope->objects[i], name, gh, sh);
        if (s) {
            if (found) *found = s;
            return scope->objects[i]->bias + (uptr)s->value;
        }
    }
    return 0;
}

/* ---- relocation ------------------------------------------------------------ */

#if __SIZEOF_POINTER__ == 8
enum { R_ABS = 1, R_PC = 2, R_COPY = 5, R_GLOB = 6, R_JUMP = 7, R_REL = 8, R_IREL = 37 };
#define R_SYM(info) ((u32)((info) >> 32))
#else
enum { R_ABS = 1, R_PC = 2, R_COPY = 5, R_GLOB = 6, R_JUMP = 7, R_REL = 8, R_IREL = 42 };
#define R_SYM(info) ((u32)((info) >> 8))
#endif

/* The compiler emits calls to these even in freestanding code, so drivers
   get them by name instead of through dk_api. */
static int resolving_driver;

static uptr runtime_symbol(const char *name) {
    if (!rt_strcmp(name, "memcpy")) return (uptr)memcpy;
    if (!rt_strcmp(name, "memset")) return (uptr)memset;
    if (!rt_strcmp(name, "memmove")) return (uptr)memmove;
    if (!rt_strcmp(name, "memcmp")) return (uptr)memcmp;
    return 0;
}

static int apply(const struct scope *scope, const struct elf_object *o, const char *path, uptr offset, uptr info,
                 i64 addend, int has_addend, int pass) {
    uptr *where = (uptr *)(o->bias + offset);
    u32 type = R_TYPE(info), index = R_SYM(info);
    if (type == 0) return 0;
    if (type == R_IREL) {
        if (pass) *where = ((uptr(*)(void))(o->bias + (uptr)(has_addend ? addend : (i64)*where)))();
        return 0;
    }
    if (pass) return 0;
    if (type == R_REL) {
        *where = o->bias + (uptr)(has_addend ? addend : (i64)*where);
        return 0;
    }
    if (type != R_ABS && type != R_PC && type != R_COPY && type != R_GLOB && type != R_JUMP) {
        complain(path, "uses a relocation type DOS-XC does not support.", 0);
        return -1;
    }
    const Sym *s = &o->symtab[index];
    const char *name = o->strtab + s->name;
    uptr value = 0;
    const Sym *definition = 0;
    if (index) {
        /* Copy relocations live in the program: take the library's value. */
        value = lookup(scope, name, type == R_COPY, &definition);
        if (!definition && resolving_driver) value = runtime_symbol(name);
        if (!definition && !value) {
            if (ELF_BIND(s->info) != 2) {
                complain(path, "needs a symbol no loaded library provides: ", name);
                return -1;
            }
        }
    }
    uptr a = has_addend ? (uptr)addend : (type == R_ABS || type == R_PC ? *where : 0);
    switch (type) {
    case R_ABS: *where = value + a; break;
    case R_PC: *where = value + a - (uptr)where; break;
    case R_GLOB: case R_JUMP: *where = value + (has_addend ? a : 0); break;
    case R_COPY: if (definition) memcpy(where, (void *)value, (uptr)s->size); break;
    }
    return 0;
}

static int relocate_object(const struct scope *scope, const struct elf_object *o, const char *path) {
    for (int pass = 0; pass < 2; ++pass) {
        for (uptr at = 0; at + sizeof(Rela) <= o->rela_size; at += sizeof(Rela)) {
            const Rela *r = (const Rela *)(o->rela + at);
            if (apply(scope, o, path, (uptr)r->offset, (uptr)r->info, r->addend, 1, pass)) return -1;
        }
        for (uptr at = 0; at + sizeof(Rel) <= o->rel_size; at += sizeof(Rel)) {
            const Rel *r = (const Rel *)(o->rel + at);
            if (apply(scope, o, path, r->offset, r->info, 0, 0, pass)) return -1;
        }
        uptr size = o->plt_rela ? sizeof(Rela) : sizeof(Rel);
        for (uptr at = 0; at + size <= o->jmprel_size; at += size) {
            if (o->plt_rela) {
                const Rela *r = (const Rela *)(o->jmprel + at);
                if (apply(scope, o, path, (uptr)r->offset, (uptr)r->info, r->addend, 1, pass)) return -1;
            } else {
                const Rel *r = (const Rel *)(o->jmprel + at);
                if (apply(scope, o, path, r->offset, r->info, 0, 0, pass)) return -1;
            }
        }
        if (!pass) {
            uptr *where = 0;
            for (uptr at = 0; at + sizeof(uptr) <= o->relr_size; at += sizeof(uptr)) {
                uptr entry = *(const uptr *)(o->relr + at);
                if (!(entry & 1)) {
                    where = (uptr *)(o->bias + entry);
                    *where++ += o->bias;
                } else if (where) {
                    for (unsigned i = 0; (entry >>= 1); ++i)
                        if (entry & 1) where[i] += o->bias;
                    where += 8 * sizeof(uptr) - 1;
                }
            }
        }
    }
    return 0;
}

/* ---- loading libraries ------------------------------------------------------- */

static void unmap_library(struct library *l) {
    for (uptr va = l->data_start; va < l->data_end; va += PAGE) paging_map(va, va);
}

static void unload(struct library *l) {
    unmap_library(l);
    memory_free((void *)l->image, OWNER_KERNEL);
    if (l->template) memory_free(l->template, OWNER_KERNEL);
    memset(l, 0, sizeof(*l));
}

static struct library *load_library(const char *path, int *error);

/* Find NAME for OWNER (whose DT_RUNPATH is RUNPATH); PROGRAM is the
   program's canonical path.  Returns 0 and the canonical path, or -ENOENT. */
static int search(const char *name, const char *runpath, const char *program, char *out) {
    char candidate[PATH_MAX_RT * 2];
    struct fat32_info info;
    for (const char *p = name; *p; ++p)
        if (*p == '/' || *p == '\\') return path_resolve(current, name, out) || fs_stat(out, &info) ? -DOSXC_ENOENT : 0;
    while (runpath && *runpath) {
        uptr n = 0;
        while (runpath[n] && runpath[n] != ':' && runpath[n] != ';') ++n;
        if (n && n < PATH_MAX_RT - 2) {
            memcpy(candidate, runpath, n);
            candidate[n] = '/';
            rt_strcpy(candidate + n + 1, name);
            if (!path_resolve(current, candidate, out) && !fs_stat(out, &info)) return 0;
        }
        runpath += n;
        if (*runpath) ++runpath;
    }
    rt_strcpy(candidate, program);
    uptr n = rt_strlen(candidate);
    while (n && candidate[n - 1] != '\\') --n;
    rt_strcpy(candidate + n, name);
    if (!path_resolve(current, candidate, out) && !fs_stat(out, &info)) return 0;
    candidate[0] = (char)rt_upper(ctx->drive);
    candidate[1] = ':';
    rt_strcpy(candidate + 2, "\\SYSTEM\\LIB\\");
    rt_strcpy(candidate + 14, name);
    if (!path_resolve(current, candidate, out) && !fs_stat(out, &info)) return 0;
    return -DOSXC_ENOENT;
}

static struct library *load_library(const char *path, int *error) {
    for (int i = 0; i < MAX_LIBRARIES; ++i)
        if (libraries[i].used && rt_equal_nocase(libraries[i].path, path)) return &libraries[i];
    struct library *l = 0;
    for (int i = 0; i < MAX_LIBRARIES && !l; ++i)
        if (!libraries[i].used) l = &libraries[i];
    if (!l) { complain(path, "cannot be loaded: too many shared libraries.", 0); *error = -DOSXC_ENOMEM; return 0; }
    struct fat32_info info;
    *error = -DOSXC_ENOEXEC;
    if (fs_stat(path, &info)) { *error = -DOSXC_ENOENT; return 0; }
    u64 size = info.size;
    uptr first = size < sizeof(header) ? (uptr)size : sizeof(header);
    if (read_at(path, 0, header, first, size)) { *error = -DOSXC_EIO; return 0; }
    const Ehdr *e = (const Ehdr *)header;
    if (first < sizeof(Ehdr) || memcmp(e->ident, "\177ELF", 4) || e->ident[4] != ELF_CLASS ||
        e->machine != ELF_MACHINE || e->type != ET_DYN || e->phentsize != sizeof(Phdr) || !e->phnum ||
        e->phnum > MAX_PHDRS || read_at(path, e->phoff, phdrs, e->phnum * sizeof(Phdr), size)) {
        complain(path, "is not a shared library for this edition of DOS-XC.", 0);
        return 0;
    }
    uptr low = (uptr)-1, high = 0, align = PAGE, data_low = (uptr)-1, data_high = 0;
    const Phdr *dynamic = 0;
    for (unsigned i = 0; i < e->phnum; ++i) {
        const Phdr *ph = &phdrs[i];
        if (ph->type == PT_TLS) { complain(path, "uses thread-local storage, which DOS-XC does not provide.", 0); return 0; }
        if (ph->type == PT_DYNAMIC) dynamic = ph;
        if (ph->type != PT_LOAD || !ph->memsz) continue;
        if ((uptr)ph->vaddr < low) low = (uptr)ph->vaddr & ~(uptr)(PAGE - 1);
        if ((uptr)(ph->vaddr + ph->memsz) > high) high = (uptr)(ph->vaddr + ph->memsz);
        if (ph->align > align && ph->align <= 0x200000) align = (uptr)ph->align;
        if (ph->flags & PF_W) {
            if ((uptr)ph->vaddr < data_low) data_low = (uptr)ph->vaddr & ~(uptr)(PAGE - 1);
            if ((uptr)(ph->vaddr + ph->memsz) > data_high) data_high = (uptr)(ph->vaddr + ph->memsz);
        }
    }
    if (!dynamic || high <= low) { complain(path, "is not a shared library (no dynamic section).", 0); return 0; }
    high = (high + PAGE - 1) & ~(uptr)(PAGE - 1);
    data_high = (data_high + PAGE - 1) & ~(uptr)(PAGE - 1);
    u8 *base = memory_alloc(high - low, align, OWNER_KERNEL, 1);
    if (!base) { complain(path, "does not fit in free memory.", 0); *error = -DOSXC_ENOMEM; return 0; }
    uptr bias = (uptr)base - low;
    for (unsigned i = 0; i < e->phnum; ++i) {
        const Phdr *ph = &phdrs[i];
        if (ph->type == PT_LOAD && ph->filesz &&
            read_at(path, ph->offset, (void *)(bias + (uptr)ph->vaddr), (uptr)ph->filesz, size)) {
            memory_free(base, OWNER_KERNEL);
            complain(path, "could not be read from disk.", 0);
            *error = -DOSXC_EIO;
            return 0;
        }
    }
    memset(l, 0, sizeof(*l));
    l->used = 1;
    rt_strcpy(l->path, path);
    l->image = (uptr)base;
    l->image_size = high - low;
    elf_parse_dynamic(&l->object, bias, (const Dyn *)(bias + (uptr)dynamic->vaddr));
    if (l->object.textrel) {
        complain(path, "has text relocations; build it with -fPIC.", 0);
        unload(l);
        return 0;
    }
    if (data_low < data_high) {
        l->data_start = bias + data_low;
        l->data_end = bias + data_high;
        l->template = memory_alloc(l->data_end - l->data_start, PAGE, OWNER_KERNEL, 0);
        if (!l->template) { unload(l); *error = -DOSXC_ENOMEM; return 0; }
        memcpy(l->template, (void *)l->data_start, l->data_end - l->data_start);
    }
    /* Copy the names out: loading a dependency reuses the header buffers. */
    for (int i = 0; i < l->object.needed_count; ++i) {
        char wanted[PATH_MAX_RT], found[PATH_MAX_RT];
        rt_strcpy(wanted, (const char *)l->object.needed[i]);
        if (search(wanted, l->object.runpath, path, found)) {
            complain(path, "needs a shared library that cannot be found: ", wanted);
            unload(l);
            *error = -DOSXC_ENOENT;
            return 0;
        }
        struct library *dependency = load_library(found, error);
        if (!dependency) { unload(l); return 0; }
        l->needed[l->needed_count++] = dependency;
    }
    return l;
}

/* ---- per-process instances -------------------------------------------------- */

static void map_instance(struct library *l, u8 *data) {
    for (uptr va = l->data_start; va < l->data_end; va += PAGE) paging_map(va, (uptr)data + (va - l->data_start));
}

static int contains(const struct process *p, const struct library *l) {
    for (int i = 0; i < p->library_count; ++i)
        if (p->libraries[i] == l) return i;
    return -1;
}

/* Depth-first, dependencies before dependents. */
static int add_ordered(struct process *p, struct library *l, int depth) {
    if (contains(p, l) >= 0) return 0;
    if (depth > MAX_LIBRARIES) return -1;
    for (int i = 0; i < l->needed_count; ++i)
        if (add_ordered(p, l->needed[i], depth + 1)) return -1;
    if (contains(p, l) >= 0) return 0;
    if (p->library_count == MAX_PROCESS_LIBRARIES) return -1;
    p->libraries[p->library_count++] = l;
    ++l->refs;
    return 0;
}

static void collect_scope(struct scope *scope, struct library *l) {
    for (int i = 1; i < scope->count; ++i)
        if (scope->objects[i] == &l->object) return;
    if (scope->count <= MAX_LIBRARIES) scope->objects[scope->count++] = &l->object;
}

void libraries_activate(struct process *p) {
    for (int i = 0; p && i < p->library_count; ++i)
        if (p->library_data[i]) map_instance(p->libraries[i], p->library_data[i]);
}

/* A process ends: give its libraries back to the ancestors using them (or
   identity map them), and unload the ones nobody uses any more. */
void libraries_release(struct process *p) {
    for (int i = p->library_count; i-- > 0;) {
        struct library *l = p->libraries[i];
        /* The nearest suspended ancestor using the library gets its copy back. */
        const struct process *owner = p->parent;
        int at = -1;
        while (owner && (at = contains(owner, l)) < 0) owner = owner->parent;
        if (owner && owner->library_data[at]) map_instance(l, owner->library_data[at]);
        else unmap_library(l);
        if (!--l->refs) unload(l);
    }
    p->library_count = 0;
}

/* Link a dynamically linked program: load its libraries, give the process
   its copies of their data, relocate everything and list the constructors
   and destructors to run.  Returns 0 or a negative errno value. */
int dynamic_link(struct process *p, const char *path, struct elf_object *program, struct link_result *out) {
    memset(out, 0, sizeof(*out));
    int error = 0;
    struct library *direct[MAX_NEEDED];
    int direct_count = 0;
    for (int i = 0; i < program->needed_count; ++i) {
        char wanted[PATH_MAX_RT], found[PATH_MAX_RT];
        rt_strcpy(wanted, (const char *)program->needed[i]);
        if (search(wanted, program->runpath, path, found)) {
            complain(path, "needs a shared library that cannot be found: ", wanted);
            return -DOSXC_ENOENT;
        }
        struct library *l = load_library(found, &error);
        if (!l) return error ? error : -DOSXC_ENOEXEC;
        direct[direct_count++] = l;
        if (add_ordered(p, l, 0)) return -DOSXC_ENOMEM;
    }
    for (int i = 0; i < p->library_count; ++i) {
        struct library *l = p->libraries[i];
        if (l->data_end > l->data_start) {
            u8 *data = memory_alloc(l->data_end - l->data_start, PAGE, p->pid, 0);
            if (!data) return -DOSXC_ENOMEM;
            memcpy(data, l->template, l->data_end - l->data_start);
            p->library_data[i] = data;
            map_instance(l, data);
        }
    }
    /* Scope: the program, then breadth-first through DT_NEEDED. */
    struct scope scope;
    scope.objects[0] = program;
    scope.count = 1;
    struct library *queue[MAX_LIBRARIES];
    int head = 0, tail = 0;
    for (int i = 0; i < direct_count; ++i) queue[tail++] = direct[i];
    while (head < tail) {
        struct library *l = queue[head++];
        collect_scope(&scope, l);
        for (int n = 0; n < l->needed_count; ++n) {
            int seen = 0;
            for (int k = 0; k < tail; ++k) if (queue[k] == l->needed[n]) seen = 1;
            if (!seen && tail < MAX_LIBRARIES) queue[tail++] = l->needed[n];
        }
    }
    for (int i = 0; i < p->library_count; ++i)
        if (relocate_object(&scope, &p->libraries[i]->object, p->libraries[i]->path)) return -DOSXC_ENOEXEC;
    if (relocate_object(&scope, program, path)) return -DOSXC_ENOEXEC;
    for (int i = 0; i < p->library_count; ++i) {
        const struct elf_object *o = &p->libraries[i]->object;
        if (o->init && out->init_count < MAX_CONSTRUCTORS) out->inits[out->init_count++] = o->init;
        for (uptr at = 0; at + sizeof(uptr) <= o->init_array_size && out->init_count < MAX_CONSTRUCTORS; at += sizeof(uptr)) {
            uptr f = *(uptr *)(o->init_array + at);
            if (f && f != (uptr)-1) out->inits[out->init_count++] = f;
        }
    }
    for (int i = p->library_count; i-- > 0;) {
        const struct elf_object *o = &p->libraries[i]->object;
        for (uptr at = o->fini_array_size; at >= sizeof(uptr) && out->fini_count < MAX_CONSTRUCTORS; at -= sizeof(uptr)) {
            uptr f = *(uptr *)(o->fini_array + at - sizeof(uptr));
            if (f && f != (uptr)-1) out->finis[out->fini_count++] = f;
        }
        if (o->fini && out->fini_count < MAX_CONSTRUCTORS) out->finis[out->fini_count++] = o->fini;
    }
    return 0;
}

/* Load the device driver at PATH: one instance, relocated against itself
   (drivers import nothing).  Returns 0, its image and the exported
   dosxc_driver descriptor, or a negative errno value. */
int driver_image_load(const char *path, uptr *image, const void **symbol) {
    struct fat32_info info;
    if (fs_stat(path, &info)) return -DOSXC_ENOENT;
    u64 size = info.size;
    uptr first = size < sizeof(header) ? (uptr)size : sizeof(header);
    if (read_at(path, 0, header, first, size)) return -DOSXC_EIO;
    const Ehdr *e = (const Ehdr *)header;
    if (first < sizeof(Ehdr) || memcmp(e->ident, "\177ELF", 4) || e->ident[4] != ELF_CLASS ||
        e->machine != ELF_MACHINE || e->type != ET_DYN || e->phentsize != sizeof(Phdr) || !e->phnum ||
        e->phnum > MAX_PHDRS || read_at(path, e->phoff, phdrs, e->phnum * sizeof(Phdr), size)) {
        complain(path, "is not a driver for this edition of DOS-XC.", 0);
        return -DOSXC_ENOEXEC;
    }
    uptr low = (uptr)-1, high = 0, align = PAGE;
    const Phdr *dynamic = 0;
    for (unsigned i = 0; i < e->phnum; ++i) {
        const Phdr *ph = &phdrs[i];
        if (ph->type == PT_TLS) { complain(path, "uses thread-local storage, which drivers cannot.", 0); return -DOSXC_ENOEXEC; }
        if (ph->type == PT_DYNAMIC) dynamic = ph;
        if (ph->type != PT_LOAD || !ph->memsz) continue;
        if ((uptr)ph->vaddr < low) low = (uptr)ph->vaddr & ~(uptr)(PAGE - 1);
        if ((uptr)(ph->vaddr + ph->memsz) > high) high = (uptr)(ph->vaddr + ph->memsz);
        if (ph->align > align && ph->align <= 0x200000) align = (uptr)ph->align;
    }
    if (!dynamic || high <= low) { complain(path, "is not a driver (no dynamic section).", 0); return -DOSXC_ENOEXEC; }
    high = (high + PAGE - 1) & ~(uptr)(PAGE - 1);
    u8 *base = memory_alloc(high - low, align, OWNER_KERNEL, 1);
    if (!base) { complain(path, "does not fit in free memory.", 0); return -DOSXC_ENOMEM; }
    uptr bias = (uptr)base - low;
    for (unsigned i = 0; i < e->phnum; ++i) {
        const Phdr *ph = &phdrs[i];
        if (ph->type == PT_LOAD && ph->filesz &&
            read_at(path, ph->offset, (void *)(bias + (uptr)ph->vaddr), (uptr)ph->filesz, size)) {
            memory_free(base, OWNER_KERNEL);
            complain(path, "could not be read from disk.", 0);
            return -DOSXC_EIO;
        }
    }
    struct elf_object object;
    elf_parse_dynamic(&object, bias, (const Dyn *)(bias + (uptr)dynamic->vaddr));
    if (object.needed_count) {
        memory_free(base, OWNER_KERNEL);
        complain(path, "links against a library; drivers use only the dk_api table.", 0);
        return -DOSXC_ENOEXEC;
    }
    struct scope scope;
    scope.objects[0] = &object;
    scope.count = 1;
    resolving_driver = 1;
    int failed = object.textrel || relocate_object(&scope, &object, path);
    resolving_driver = 0;
    if (failed) {
        memory_free(base, OWNER_KERNEL);
        return -DOSXC_ENOEXEC;
    }
    const Sym *found = find_symbol(&object, "dosxc_driver", gnu_hash("dosxc_driver"), sysv_hash("dosxc_driver"));
    if (!found) {
        memory_free(base, OWNER_KERNEL);
        complain(path, "does not export dosxc_driver.", 0);
        return -DOSXC_ENOEXEC;
    }
    *image = (uptr)base;
    *symbol = (const void *)(bias + (uptr)found->value);
    return 0;
}

/* The library containing ADDRESS, for crash messages. */
const char *library_at(uptr address, uptr *offset) {
    for (int i = 0; i < MAX_LIBRARIES; ++i)
        if (libraries[i].used && address >= libraries[i].image && address < libraries[i].image + libraries[i].image_size) {
            *offset = address - libraries[i].image;
            return libraries[i].path;
        }
    return 0;
}
