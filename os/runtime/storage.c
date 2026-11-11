/*
 * DOS-XC /os/runtime/storage.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "storage.h"

static struct blockdev *devices[STORAGE_MAX];
static unsigned device_count;
static u8 *bounce;                      /* STORAGE_BUFFER bytes, 64 KiB aligned */

int storage_add(struct blockdev *device) {
    if (device_count == STORAGE_MAX || !device->blocks || !device->transfer) return -1;
    if (device->block_size != 512 && device->block_size != 2048 && device->block_size != 4096) return -1;
    u32 most = STORAGE_BUFFER / device->block_size;
    if (!device->max_blocks || device->max_blocks > most) device->max_blocks = most;
    devices[device_count++] = device;
    return 0;
}

unsigned storage_count(void) { return device_count; }
struct blockdev *storage_device(unsigned index) { return index < device_count ? devices[index] : 0; }

u64 storage_sectors(unsigned index) {
    struct blockdev *d = storage_device(index);
    return d ? d->blocks * (d->block_size / 512) : 0;
}

int storage_wait(volatile u32 *reg, u32 mask, u32 value, u64 ms) {
    u64 end = cpu_nanoseconds() + ms * 1000000ull;
    for (;;) {
        if ((*reg & mask) == value) return 0;
        if (cpu_nanoseconds() > end) return -1;
        __asm__ volatile("pause");
    }
}

/* Whole device blocks through the bounce buffer. */
static int blocks_io(struct blockdev *d, u64 block, u32 blocks, u8 *buffer, int write) {
    while (blocks) {
        u32 n = blocks < d->max_blocks ? blocks : d->max_blocks;
        uptr bytes = (uptr)n * d->block_size;
        if (write) memcpy(bounce, buffer, bytes);
        if (d->transfer(d, block, n, bounce, write)) return -1;
        if (!write) memcpy(buffer, bounce, bytes);
        block += n;
        blocks -= n;
        buffer += bytes;
    }
    return 0;
}

int storage_io(unsigned index, u64 lba, u32 count, void *buffer, int write) {
    struct blockdev *d = storage_device(index);
    if (!d || !count) return -1;
    if (!bounce) {
        bounce = memory_alloc(STORAGE_BUFFER, 65536, OWNER_KERNEL, 1);
        if (!bounce || (u64)(uptr)bounce + STORAGE_BUFFER > 0x100000000ull) { bounce = 0; return -1; }
    }
    if (write && d->readonly) return -1;
    u32 per = d->block_size / 512;
    if (lba + count > d->blocks * per) return -1;
    u8 *out = buffer;
    if (per == 1) return blocks_io(d, lba, count, out, write);
    /* A partial first or last block: read it whole, then copy (or merge and
       write it back). */
    static u8 partial[4096];
    while (count) {
        u64 block = lba / per;
        u32 skip = (u32)(lba % per);
        if (skip || count < per) {
            u32 n = per - skip < count ? per - skip : count;
            if (blocks_io(d, block, 1, partial, 0)) return -1;
            if (write) {
                memcpy(partial + skip * 512, out, (uptr)n * 512);
                if (blocks_io(d, block, 1, partial, 1)) return -1;
            } else {
                memcpy(out, partial + skip * 512, (uptr)n * 512);
            }
            lba += n;
            count -= n;
            out += (uptr)n * 512;
            continue;
        }
        u32 whole = count / per;
        if (blocks_io(d, block, whole, out, write)) return -1;
        lba += (u64)whole * per;
        count -= whole * per;
        out += (uptr)whole * d->block_size;
    }
    return 0;
}

void storage_flush_all(void) {
    for (unsigned i = 0; i < device_count; ++i)
        if (devices[i]->flush && !devices[i]->readonly) devices[i]->flush(devices[i]);
}

void storage_probe(void) {
    static int probed;
    if (probed) return;
    probed = 1;
    pci_scan_class(1, 0x01, ata_probe);       /* IDE */
    pci_scan_class(1, 0x06, ahci_probe);      /* SATA (AHCI) */
    pci_scan_class(1, 0x08, nvme_probe);      /* NVMe */
}
