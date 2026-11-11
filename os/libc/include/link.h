/*
 * DOS-XC /os/libc/include/link.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _LINK_H
#define _LINK_H

#include <elf.h>
#include <stddef.h>

#ifdef __x86_64__
#define ElfW(type) Elf64_##type
#else
#define ElfW(type) Elf32_##type
#endif

struct dl_phdr_info {
    ElfW(Addr) dlpi_addr;
    const char *dlpi_name;
    const ElfW(Phdr) *dlpi_phdr;
    ElfW(Half) dlpi_phnum;
};

#ifdef __cplusplus
extern "C" {
#endif
int dl_iterate_phdr(int (*callback)(struct dl_phdr_info *, size_t, void *), void *data);
#ifdef __cplusplus
}
#endif

#endif
