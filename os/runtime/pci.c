/*
 * DOS-XC /os/runtime/pci.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "usb.h"

static u32 address_of(const struct pci_function *f, u8 offset) {
    return 0x80000000u | (u32)f->bus << 16 | (u32)f->device << 11 | (u32)f->function << 8 | (offset & 0xfc);
}
static inline void outl(u16 port, u32 v) { __asm__ volatile("outl %0,%1" ::"a"(v), "Nd"(port)); }
static inline u32 inl(u16 port) { u32 v; __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(port)); return v; }

u32 pci_read32(const struct pci_function *f, u8 offset) {
    outl(0xcf8, address_of(f, offset));
    return inl(0xcfc);
}
void pci_write32(const struct pci_function *f, u8 offset, u32 value) {
    outl(0xcf8, address_of(f, offset));
    outl(0xcfc, value);
}
u16 pci_read16(const struct pci_function *f, u8 offset) {
    return (u16)(pci_read32(f, offset) >> ((offset & 2) * 8));
}
void pci_write16(const struct pci_function *f, u8 offset, u16 value) {
    u32 v = pci_read32(f, offset), shift = (offset & 2) * 8u;
    pci_write32(f, offset, (v & ~(0xffffu << shift)) | (u32)value << shift);
}
u8 pci_read8(const struct pci_function *f, u8 offset) {
    return (u8)(pci_read32(f, offset) >> ((offset & 3) * 8));
}
void pci_write8(const struct pci_function *f, u8 offset, u8 value) {
    u32 v = pci_read32(f, offset), shift = (offset & 3) * 8u;
    pci_write32(f, offset, (v & ~(0xffu << shift)) | (u32)value << shift);
}

u64 pci_bar64(const struct pci_function *f, int index, int *io) {
    u8 offset = (u8)(0x10 + index * 4);
    u32 low = pci_read32(f, offset);
    if (low & 1) {
        if (io) *io = 1;
        return low & ~3u;
    }
    if (io) *io = 0;
    u64 base = low & ~15u;
    if (((low >> 1) & 3) == 2) base |= (u64)pci_read32(f, (u8)(offset + 4)) << 32;
    return base;
}

u64 pci_bar(const struct pci_function *f, int index, int *io) {
    u64 base = pci_bar64(f, index, io);
    return base >> 32 ? 0 : base;
}

/* For the kernel only: also above 4 GiB where the firmware mapped it (UEFI). */
u64 pci_bar_kernel(const struct pci_function *f, int index, int *io) {
#if __SIZEOF_POINTER__ == 8
    if (!ctx->bios_available) return pci_bar64(f, index, io);
#endif
    return pci_bar(f, index, io);
}

void pci_enable(const struct pci_function *f, int memory, int io) {
    u16 command = pci_read16(f, 4);
    command |= 4;
    if (memory) command |= 2;
    if (io) command |= 1;
    command |= 0x400;                     /* no legacy INTx: everything is polled */
    pci_write16(f, 4, command);
}

void pci_scan_class(u8 class_code, u8 subclass, pci_found_fn found) {
    for (unsigned bus = 0; bus < 256; ++bus)
        for (u8 device = 0; device < 32; ++device) {
            struct pci_function f = {(u8)bus, device, 0, 0, 0, 0};
            u32 id = pci_read32(&f, 0);
            if ((id & 0xffff) == 0xffff) continue;
            int functions = (pci_read8(&f, 0x0e) & 0x80) ? 8 : 1;
            for (u8 function = 0; function < functions; ++function) {
                f.function = function;
                id = pci_read32(&f, 0);
                if ((id & 0xffff) == 0xffff) continue;
                u32 class_word = pci_read32(&f, 8);
                if ((class_word >> 24) != class_code || ((class_word >> 16) & 0xff) != subclass) continue;
                f.vendor = (u16)id;
                f.id = (u16)(id >> 16);
                f.prog_if = (u8)(class_word >> 8);
                found(&f);
            }
        }
}

#define POOL_SIZE (512u * 1024)
#define UNIT 64u
#define UNITS (POOL_SIZE / UNIT)
static u8 *pool;
static u32 used_map[UNITS / 32];

static int unit_used(u32 i) { return (used_map[i / 32] >> (i % 32)) & 1; }
static void mark(u32 first, u32 count, int used) {
    for (u32 i = first; i < first + count; ++i) {
        if (used) used_map[i / 32] |= 1u << (i % 32);
        else used_map[i / 32] &= ~(1u << (i % 32));
    }
}

void *dma_alloc(u32 size, u32 align) {
    if (!pool) {
        pool = memory_alloc(POOL_SIZE, 4096, OWNER_KERNEL, 1);
        if (!pool || (u64)(uptr)pool + POOL_SIZE > 0x100000000ull) { pool = 0; return 0; }
    }
    if (!size) size = 1;
    if (align < UNIT) align = UNIT;
    u32 count = (size + UNIT - 1) / UNIT, step = align / UNIT;
    for (u32 first = 0; first + count <= UNITS; first += step) {
        u32 i = 0;
        while (i < count && !unit_used(first + i)) ++i;
        if (i < count) continue;
        /* A block never crosses a 4 KiB page unless it is page-sized itself
           (UHCI/OHCI/EHCI descriptors must not straddle pages). */
        if (size <= 4096 && (first * UNIT) / 4096 != ((first + count) * UNIT - 1) / 4096) continue;
        mark(first, count, 1);
        u8 *p = pool + first * UNIT;
        memset(p, 0, count * UNIT);
        return p;
    }
    return 0;
}

void dma_free(void *block, u32 size) {
    if (!block || !pool) return;
    u32 first = (u32)((u8 *)block - pool) / UNIT, count = (size ? size + UNIT - 1 : UNIT) / UNIT;
    if (first + count <= UNITS) mark(first, count, 0);
}
