/*
 * DOS-XC /runtime/loader.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * The DOS-XC executable loader and process model.
 *
 * spawn() works like MS-DOS EXEC: the parent is suspended until the child
 * exits, then resumes with the child's wait status.  Every program is a
 * static PIE, so the loader places the child in free arena memory, applies
 * its RELATIVE relocations and enters it on a private stack.  Exceptions
 * raised while a program runs terminate that program only.
 */

#include "runtime.h"

#if __SIZEOF_POINTER__ == 8
typedef struct { u8 ident[16]; u16 type, machine; u32 version; u64 entry, phoff, shoff; u32 flags;
                 u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } Ehdr;
typedef struct { u32 type, flags; u64 offset, vaddr, paddr, filesz, memsz, align; } Phdr;
typedef struct { i64 tag; u64 value; } Dyn;
typedef struct { u64 offset, info; i64 addend; } Rela;
#define ELF_CLASS 2
#define ELF_MACHINE 62
#define R_IRELATIVE 37
#define R_TYPE(info) ((u32)(info))
#define MACHINE_NAME "x86-64"
#else
typedef struct { u8 ident[16]; u16 type, machine; u32 version, entry, phoff, shoff, flags;
                 u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } Ehdr;
typedef struct { u32 type, offset, vaddr, paddr, filesz, memsz, flags, align; } Phdr;
typedef struct { int tag; u32 value; } Dyn;
typedef struct { u32 offset, info; int addend; } Rela;
#define ELF_CLASS 1
#define ELF_MACHINE 3
#define R_IRELATIVE 42
#define R_TYPE(info) ((info) & 0xff)
#define MACHINE_NAME "i386"
#endif
typedef struct { uptr offset, info; } Rel;

#define ET_EXEC 2
#define ET_DYN 3
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PT_NOTE 4
#define PT_PHDR 6
#define PT_TLS 7
#define PT_GNU_STACK 0x6474e551u
#define R_NONE 0
#define R_RELATIVE 8
#define MAX_PHDRS 64

struct image { uptr base, entry, phdr, phnum, stack_size; };

static struct process processes[MAX_DEPTH];
struct process *current;
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
    return size ? ctx->read_at(ctx->volume, path, (u32)offset, buffer, (u32)size) : 0;
}

static int has_dosxc_note(const char *path, const Phdr *ph, u64 file_size) {
    uptr size = ph->filesz < sizeof(note) ? (uptr)ph->filesz : sizeof(note);
    if (read_at(path, ph->offset, note, size, file_size)) return 0;
    for (uptr at = 0; at + 12 <= size;) {
        u32 namesz = *(u32 *)(note + at), descsz = *(u32 *)(note + at + 4), type = *(u32 *)(note + at + 8);
        uptr name = at + 12, desc = name + ((namesz + 3) & ~3u), next = desc + ((descsz + 3) & ~3u);
        if (next > size || next <= at) return 0;
        if (namesz == 7 && type == DOSXC_NOTE_ABI && !memcmp(note + name, DOSXC_NOTE_NAME, 7) &&
            descsz >= 4 && *(u32 *)(note + desc) >= 1)
            return 1;
        at = next;
    }
    return 0;
}

static int relocate(const char *path, uptr bias, const Dyn *dynamic) {
    uptr rela = 0, rela_size = 0, rel = 0, rel_size = 0, relr = 0, relr_size = 0;
    for (const Dyn *d = dynamic; d->tag; ++d) {
        switch (d->tag) {
        case 1: complain(path, "requires shared libraries, which DOS-XC does not support."); return -1;
        case 7: rela = bias + (uptr)d->value; break;
        case 8: rela_size = (uptr)d->value; break;
        case 17: rel = bias + (uptr)d->value; break;
        case 18: rel_size = (uptr)d->value; break;
        case 36: relr = bias + (uptr)d->value; break;
        case 35: relr_size = (uptr)d->value; break;
        }
    }
    /* Pass 0 applies RELATIVE relocations, pass 1 calls IRELATIVE resolvers. */
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
    if (ctx->stat(ctx->volume, path, &info)) return -DOSXC_ENOENT;
    u64 size = info.size;
    uptr first = size < sizeof(header) ? (uptr)size : sizeof(header);
    if (read_at(path, 0, header, first, size)) return -DOSXC_EIO;
    const Ehdr *e = (const Ehdr *)header;
    if (first < sizeof(Ehdr) || memcmp(e->ident, "\177ELF", 4) || e->ident[5] != 1 || e->ident[6] != 1)
        return -DOSXC_ENOEXEC;
    if (e->ident[4] != ELF_CLASS || e->machine != ELF_MACHINE) {
        complain(path, e->ident[4] == 2 ? "is a 64-bit (x86-64) program; boot the x64 edition of DOS-XC to run it."
                                        : "is a 32-bit (i386) program; boot the x32 edition of DOS-XC to run it.");
        return -DOSXC_ENOEXEC;
    }
    if (e->type != ET_DYN && e->type != ET_EXEC) { complain(path, "is not an executable ELF file."); return -DOSXC_ENOEXEC; }
    if (e->phentsize != sizeof(Phdr) || !e->phnum || e->phnum > MAX_PHDRS ||
        read_at(path, e->phoff, phdrs, e->phnum * sizeof(Phdr), size)) {
        complain(path, "has invalid ELF program headers.");
        return -DOSXC_ENOEXEC;
    }
    uptr low = (uptr)-1, high = 0, align = PAGE, phdr = 0, stack = 0;
    int noted = 0;
    const Phdr *dynamic = 0;
    for (unsigned i = 0; i < e->phnum; ++i) {
        const Phdr *ph = &phdrs[i];
        if (ph->type == PT_INTERP) { complain(path, "is dynamically linked; DOS-XC programs must be static PIE (-static-pie)."); return -DOSXC_ENOEXEC; }
        if (ph->type == PT_TLS) { complain(path, "uses thread-local storage, which DOS-XC does not provide."); return -DOSXC_ENOEXEC; }
        if (ph->type == PT_NOTE && !noted) noted = has_dosxc_note(path, ph, size);
        if (ph->type == PT_DYNAMIC) dynamic = ph;
        if (ph->type == PT_PHDR) phdr = (uptr)ph->vaddr;
        if (ph->type == PT_GNU_STACK) stack = (uptr)ph->memsz;
        if (ph->type != PT_LOAD || !ph->memsz) continue;
        if (ph->filesz > ph->memsz || ph->vaddr + ph->memsz < ph->vaddr) { complain(path, "has an invalid segment."); return -DOSXC_ENOEXEC; }
        if ((uptr)ph->vaddr < low) low = (uptr)ph->vaddr & ~(uptr)(PAGE - 1);
        if ((uptr)(ph->vaddr + ph->memsz) > high) high = (uptr)(ph->vaddr + ph->memsz);
        if (ph->align > align && ph->align <= 0x200000) align = (uptr)ph->align;
    }
    if (!noted) {
        complain(path, "is not a DOS-XC program (it has no .note.dosxc ABI note). Link it with crt0.o from the DOS-XC C library.");
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
    if (dynamic && relocate(path, bias, (const Dyn *)(bias + (uptr)dynamic->vaddr))) return -DOSXC_ENOEXEC;
    out->base = (uptr)base;
    out->entry = bias + (uptr)e->entry;
    out->phdr = phdr ? bias + phdr : bias + low + (uptr)e->phoff;
    out->phnum = e->phnum;
    out->stack_size = stack;
    p->image_base = (uptr)base;
    p->image_size = high - low;
    return 0;
}

/* System V initial stack: argc, argv[], 0, envp[], 0, auxv[], strings. */
static uptr build_stack(uptr base, uptr size, const char *execfn, char *const argv[], char *const envp[],
                        const struct image *image) {
    uptr argc = 0, envc = 0, strings = rt_strlen(execfn) + 1 + 16;
    for (; argv[argc]; ++argc) strings += rt_strlen(argv[argc]) + 1;
    for (; envp && envp[envc]; ++envc) strings += rt_strlen(envp[envc]) + 1;
    if (strings > size / 4 || argc > 65536 || envc > 65536) return 0;
    uptr top = base + size;
    char *text = (char *)((top - strings) & ~(uptr)15);
    enum { AUX = 12 };
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
    extern struct dosxc_api runtime_api;
    uptr aux[AUX * 2] = {
        3, image->phdr, 4, sizeof(Phdr), 5, image->phnum, 6, PAGE, 7, 0, 9, image->entry,
        25, random, 31, name, DOSXC_AT_API, (uptr)&runtime_api, 0x42440002u, image->base,
        23, 0, 0, 0,
    };
    memcpy(w, aux, sizeof(aux));
    return sp;
}

static int find_program(struct process *parent, const char *path, char *canonical) {
    int r = path_resolve(parent, path, canonical);
    if (r) return r;
    struct fat32_info info;
    if (!ctx->stat(ctx->volume, canonical, &info))
        return (info.attributes & DOSXC_ATTR_DIRECTORY) ? -DOSXC_EACCES : 0;
    uptr n = rt_strlen(canonical), at = n;
    while (at && canonical[at - 1] != '\\' && canonical[at - 1] != '.') --at;
    if ((at && canonical[at - 1] == '.') || n + 5 >= PATH_MAX_RT) return -DOSXC_ENOENT;
    rt_strcpy(canonical + n, ".EXE");
    if (!ctx->stat(ctx->volume, canonical, &info) && !(info.attributes & DOSXC_ATTR_DIRECTORY)) return 0;
    return -DOSXC_ENOENT;
}

int process_spawn(struct process *parent, const char *path, char *const argv[], char *const envp[],
                  const char *output, int append) {
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
    rt_strcpy(p->path, canonical);
    if ((r = files_setup(p, parent, output, append))) {
        p->used = 0;
        return r;
    }
    struct image image;
    uptr stack_size = 0, stack = 0, sp = 0;
    r = load_image(p, canonical, &image);
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
        path_posix(canonical, posix, sizeof(posix));
        char *default_argv[2] = {posix, 0};
        sp = build_stack(stack, stack_size, posix, argv && argv[0] ? argv : default_argv, envp, &image);
        if (!sp) r = -DOSXC_E2BIG;
    }
    if (!r) {
        struct process *saved = current;
        current = p;
        cpu_install_idt();
        r = runtime_enter(image.entry, sp, &p->saved, cpu_has_sse());
        cpu_restore_idt();
        current = saved;
    }
    files_release(p);
    memory_free_owner(p->pid);
    p->used = 0;
    return r;
}

void process_exit(int wait_status) {
    if (!current) for (;;) __asm__ volatile("cli; hlt");
    runtime_leave(&current->saved, wait_status & 0xffff);
}

void runtime_exception(uptr *frame) {
    static const char *const names[32] = {
        "Divide error", "Debug exception", "Non-maskable interrupt", "Breakpoint", "Overflow",
        "Bound range exceeded", "Invalid opcode", "FPU not available", "Double fault",
        "Coprocessor segment overrun", "Invalid TSS", "Segment not present", "Stack fault",
        "General protection fault", "Page fault", "Reserved exception", "x87 floating-point error",
        "Alignment check", "Machine check", "SIMD floating-point error", "Virtualization exception",
        "Control protection fault"};
    uptr vector = frame[0] & 31, error = frame[1], ip = frame[2];
    int signal = vector == 0 || vector == 16 || vector == 19 ? 8
               : vector == 6 ? 4 : vector == 1 || vector == 3 ? 5 : vector == 17 ? 7 : 11;
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
    if (current && ip >= current->image_base && ip < current->image_base + current->image_size) {
        rt_print(" (image+");
        rt_print_hex(ip - current->image_base);
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
    if (!current) {
        rt_print("The system is halted.\r\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    runtime_leave(&current->saved, DOSXC_SIGNALED(signal));
}
