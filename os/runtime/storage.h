/*
 * DOS-XC /os/runtime/storage.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once
#include "usb.h"

struct blockdev {
    char kind[8];                     /* "IDE", "SATA", "NVMe" */
    u64 blocks;                       /* device blocks */
    u32 block_size;                   /* 512, 2048 (CD) or 4096 */
    u32 max_blocks;                   /* per transfer() call */
    int removable, readonly, atapi;
    void *driver;
    /* BUFFER is DMA memory (below 4 GiB, identity mapped) of BLOCKS blocks,
       BLOCKS <= max_blocks.  0 or -1. */
    int (*transfer)(struct blockdev *, u64 block, u32 blocks, void *buffer, int write);
    int (*flush)(struct blockdev *);  /* the device's write cache; may be 0 */
    int (*capacity)(struct blockdev *);   /* removable media: look again; may be 0 */
};

#define STORAGE_MAX 16
#define STORAGE_BUFFER (128u * 1024)    /* bytes per transfer at most */

int storage_add(struct blockdev *device);       /* drivers: a device found */
void storage_probe(void);                       /* scan PCI (volumes.c) */
unsigned storage_count(void);
struct blockdev *storage_device(unsigned index);
u64 storage_sectors(unsigned index);            /* 512-byte sectors */
/* 512-byte sectors, any kernel buffer. */
int storage_io(unsigned index, u64 lba, u32 count, void *buffer, int write);
void storage_flush_all(void);
int storage_wait(volatile u32 *reg, u32 mask, u32 value, u64 ms);   /* (*reg & mask) == value */

void ata_probe(const struct pci_function *f);
void ahci_probe(const struct pci_function *f);
void nvme_probe(const struct pci_function *f);
