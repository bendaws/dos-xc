/*
 * DOS-XC /runtime/elf.h - ELF structures for the program loader
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#pragma once
#include "runtime.h"

#if __SIZEOF_POINTER__ == 8
typedef struct { u8 ident[16]; u16 type, machine; u32 version; u64 entry, phoff, shoff; u32 flags;
                 u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } Ehdr;
typedef struct { u32 type, flags; u64 offset, vaddr, paddr, filesz, memsz, align; } Phdr;
typedef struct { i64 tag; u64 value; } Dyn;
typedef struct { u64 offset, info; i64 addend; } Rela;
typedef struct { u32 name; u8 info, other; u16 shndx; u64 value, size; } Sym;
#define ELF_CLASS 2
#define ELF_MACHINE 62
#define R_TYPE(info) ((u32)(info))
#define MACHINE_NAME "x86-64"
#else
typedef struct { u8 ident[16]; u16 type, machine; u32 version, entry, phoff, shoff, flags;
                 u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } Ehdr;
typedef struct { u32 type, offset, vaddr, paddr, filesz, memsz, flags, align; } Phdr;
typedef struct { int tag; u32 value; } Dyn;
typedef struct { u32 offset, info; int addend; } Rela;
typedef struct { u32 name, value, size; u8 info, other; u16 shndx; } Sym;
#define ELF_CLASS 1
#define ELF_MACHINE 3
#define R_TYPE(info) ((info) & 0xff)
#define MACHINE_NAME "i386"
#endif
typedef struct { uptr offset, info; } Rel;
#define ELF_BIND(info) ((info) >> 4)
#define ELF_TYPE(info) ((info) & 15)

#define ET_EXEC 2
#define ET_DYN 3
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PT_NOTE 4
#define PT_PHDR 6
#define PT_TLS 7
#define PT_GNU_STACK 0x6474e551u
#define PF_W 2
#define MAX_PHDRS 64

#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_INIT 12
#define DT_FINI 13
#define DT_RPATH 15
#define DT_REL 17
#define DT_RELSZ 18
#define DT_PLTREL 20
#define DT_TEXTREL 22
#define DT_JMPREL 23
#define DT_INIT_ARRAY 25
#define DT_FINI_ARRAY 26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_RUNPATH 29
#define DT_FLAGS 30
#define DT_RELRSZ 35
#define DT_RELR 36
#define DT_GNU_HASH 0x6ffffef5
#define DF_TEXTREL 4

#define MAX_NEEDED 16
struct elf_object {
    uptr bias;
    const Dyn *dynamic;
    const Sym *symtab;
    const char *strtab;
    const u32 *hash, *gnu_hash;
    uptr rela, rela_size, rel, rel_size, relr, relr_size, jmprel, jmprel_size;
    int plt_rela, textrel;
    uptr init, fini, init_array, init_array_size, fini_array, fini_array_size;
    const char *runpath;
    uptr needed[MAX_NEEDED];          /* name pointers after parsing */
    int needed_count;
};

#define MAX_CONSTRUCTORS 64
struct link_result {
    uptr inits[MAX_CONSTRUCTORS], finis[MAX_CONSTRUCTORS];
    int init_count, fini_count;
};

int driver_image_load(const char *path, uptr *image, const void **symbol);
int elf_parse_dynamic(struct elf_object *o, uptr bias, const Dyn *dynamic);
int dynamic_link(struct process *p, const char *path, struct elf_object *program, struct link_result *out);
