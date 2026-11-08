/*
 * DOS-XC /setup/target.c - formatting and writing the installation disk
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * The FAT driver writes through a sector cache held in Setup's memory.
 * Dirty sectors are written in runs of up to 64 and read back to verify,
 * which keeps the number of (slow) disk calls low.
 */
#include "../drivers/fat32.h"
#include <dosxc/syscall.h>
#include "target.h"

void *malloc(__SIZE_TYPE__);
void free(void *);
void *memset(void *, int, __SIZE_TYPE__);
void *memcpy(void *, const void *, __SIZE_TYPE__);
int memcmp(const void *, const void *, __SIZE_TYPE__);

#define PARTITION_START 2048u
#define SLOTS 65536u                   /* 32 MiB of cached sectors */
#define EMPTY 0xffffffffu

static unsigned disk;
static uint32_t total_sectors;
static struct fat32_volume volume;
static unsigned char boot[512], mbr[512];
static uint32_t *slot_lba, *table;     /* table: hash -> slot */
static unsigned char *slot_data, *slot_dirty;
static uint32_t used;
static unsigned char run[64 * 512], check[64 * 512];

static void put16(unsigned char *p, unsigned n) { p[0] = (unsigned char)n; p[1] = (unsigned char)(n >> 8); }
static void put32(unsigned char *p, uint32_t n) { for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(n >> (i * 8)); }
static uint32_t get32(const unsigned char *p) { return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static uint32_t hash(uint32_t lba) { return (lba * 2654435761u) & (SLOTS * 2 - 1); }

static uint32_t find(uint32_t lba) {
    for (uint32_t h = hash(lba);; h = (h + 1) & (SLOTS * 2 - 1)) {
        if (table[h] == EMPTY) return EMPTY;
        if (slot_lba[table[h]] == lba) return table[h];
    }
}

/* Write dirty sectors in ascending runs and verify each run. */
static int sort_compare(uint32_t a, uint32_t b) { return slot_lba[a] < slot_lba[b]; }
static void sort(uint32_t *items, uint32_t n) {           /* heap sort by lba */
    for (uint32_t start = n / 2; start-- > 0;) {
        uint32_t root = start;
        for (uint32_t child; (child = root * 2 + 1) < n; root = child) {
            if (child + 1 < n && sort_compare(items[child], items[child + 1])) ++child;
            if (!sort_compare(items[root], items[child])) break;
            uint32_t t = items[root]; items[root] = items[child]; items[child] = t;
        }
    }
    for (uint32_t end = n; end-- > 1;) {
        uint32_t t = items[0]; items[0] = items[end]; items[end] = t;
        uint32_t root = 0;
        for (uint32_t child; (child = root * 2 + 1) < end; root = child) {
            if (child + 1 < end && sort_compare(items[child], items[child + 1])) ++child;
            if (!sort_compare(items[root], items[child])) break;
            t = items[root]; items[root] = items[child]; items[child] = t;
        }
    }
}

static int flush(void) {
    uint32_t *order = malloc(sizeof(uint32_t) * (used ? used : 1)), n = 0;
    if (!order) return -1;
    for (uint32_t i = 0; i < used; ++i) if (slot_dirty[i]) order[n++] = i;
    sort(order, n);
    for (uint32_t i = 0; i < n;) {
        uint32_t first = slot_lba[order[i]], count = 0;
        while (i + count < n && count < 64 && slot_lba[order[i + count]] == first + count) {
            memcpy(run + count * 512, slot_data + (__SIZE_TYPE__)order[i + count] * 512, 512);
            ++count;
        }
        if (dosxc_sys_disk(DOSXC_DISK_WRITE, disk, first, run, count) ||
            dosxc_sys_disk(DOSXC_DISK_READ, disk, first, check, count) || memcmp(run, check, count * 512)) {
            free(order);
            return -1;
        }
        for (uint32_t k = 0; k < count; ++k) slot_dirty[order[i + k]] = 0;
        i += count;
    }
    free(order);
    return 0;
}

static int evict_all(void) {
    if (flush()) return -1;
    for (uint32_t h = 0; h < SLOTS * 2; ++h) table[h] = EMPTY;
    used = 0;
    return 0;
}

static int store(uint32_t lba, const void *data, int dirty) {
    uint32_t slot = find(lba);
    if (slot == EMPTY) {
        if (used == SLOTS && evict_all()) return -1;
        slot = used++;
        slot_lba[slot] = lba;
        uint32_t h = hash(lba);
        while (table[h] != EMPTY) h = (h + 1) & (SLOTS * 2 - 1);
        table[h] = slot;
        slot_dirty[slot] = 0;
    }
    memcpy(slot_data + (__SIZE_TYPE__)slot * 512, data, 512);
    if (dirty) slot_dirty[slot] = 1;
    return 0;
}

static int __attribute__((sysv_abi)) cached_read(void *context, uint32_t lba, void *buffer) {
    (void)context;
    uint32_t slot = find(lba);
    if (slot != EMPTY) { memcpy(buffer, slot_data + (__SIZE_TYPE__)slot * 512, 512); return 0; }
    if (dosxc_sys_disk(DOSXC_DISK_READ, disk, lba, buffer, 1)) return -1;
    return store(lba, buffer, 0);
}

static int __attribute__((sysv_abi)) cached_write(void *context, uint32_t lba, const void *buffer) {
    (void)context;
    return store(lba, buffer, 1);
}

int target_format(unsigned index, unsigned long sectors, const unsigned char *boot_template, const unsigned char *mbr_template, int bits) {
    if (sectors < 131072 || sectors > 0xffffffffu) return -1;
    disk = index;
    total_sectors = (uint32_t)sectors;
    if (!table) {
        table = malloc(sizeof(uint32_t) * SLOTS * 2);
        slot_lba = malloc(sizeof(uint32_t) * SLOTS);
        slot_dirty = malloc(SLOTS);
        slot_data = malloc((__SIZE_TYPE__)SLOTS * 512);
        if (!table || !slot_lba || !slot_dirty || !slot_data) return -1;
    }
    for (uint32_t h = 0; h < SLOTS * 2; ++h) table[h] = EMPTY;
    used = 0;
    memcpy(boot, boot_template, 512);
    memcpy(mbr, mbr_template, 512);
    boot[0x1ab] = (unsigned char)bits;
    uint32_t total = total_sectors - PARTITION_START;
    unsigned spc = total < 1048576 ? 1 : total < 16777216 ? 8 : total < 67108864 ? 32 : 64;
    uint32_t fat = 1, clusters = 0;
    for (unsigned i = 0; i < 32; ++i) {
        clusters = (total - 32 - 2 * fat) / spc;
        uint32_t needed = (clusters + 2 + 127) / 128;
        if (needed <= fat) break;
        fat = needed;
    }
    clusters = (total - 32 - 2 * fat) / spc;
    if (clusters < 65525 || clusters >= 0x0ffffff0u) return -1;
    memset(boot + 11, 0, 79);
    put16(boot + 11, 512); boot[13] = (unsigned char)spc; put16(boot + 14, 32); boot[16] = 2; boot[21] = 0xf8;
    put16(boot + 24, 63); put16(boot + 26, 255); put32(boot + 28, PARTITION_START); put32(boot + 32, total);
    put32(boot + 36, fat); put32(boot + 44, 2); put16(boot + 48, 1); put16(boot + 50, 6); boot[64] = 0x80; boot[66] = 0x29;
    uint32_t serial = get32(mbr + 440) ^ total_sectors ^ 0x13579bdfu;
    put32(boot + 67, serial);
    const char *label = "DOS-XC     FAT32   ";
    for (unsigned i = 0; i < 19; ++i) boot[71 + i] = (unsigned char)label[i];
    boot[0x1aa] = 0;
    put32(boot + 0x1b0, PARTITION_START);
    put32(mbr + 440, serial);
    /* Clear old GPT metadata and the reserved and FAT areas.  The MBR is
       written last, once the file system is complete. */
    static unsigned char zero[512];
    uint32_t data = PARTITION_START + 32 + 2 * fat;
    for (uint32_t lba = 1; lba < data + spc; ++lba)
        if (cached_write(0, lba, zero)) return -1;
    if (cached_write(0, PARTITION_START, boot) || cached_write(0, PARTITION_START + 6, boot)) return -1;
    unsigned char sector[512];
    memset(sector, 0, 512);
    put32(sector, 0x41615252); put32(sector + 484, 0x61417272);
    put32(sector + 488, clusters - 1); put32(sector + 492, 3); put32(sector + 508, 0xaa550000u);
    if (cached_write(0, PARTITION_START + 1, sector) || cached_write(0, PARTITION_START + 7, sector)) return -1;
    memset(sector, 0, 512);
    put32(sector, 0x0ffffff8); put32(sector + 4, 0xffffffffu); put32(sector + 8, 0x0fffffff);
    if (cached_write(0, PARTITION_START + 32, sector) || cached_write(0, PARTITION_START + 32 + fat, sector)) return -1;
    memset(&volume, 0, sizeof(volume));
    volume.read_sector = cached_read;
    volume.write_sector = cached_write;
    return fat32_mount(&volume, PARTITION_START);
}

int target_mkdir(const char *path) {
    struct fat32_info info;
    if (!fat32_stat(&volume, path, &info)) return (info.attributes & 16) ? 0 : -1;
    return fat32_mkdir(&volume, path);
}

int target_write(const char *path, const void *data, unsigned long size) {
    static unsigned char empty[1];
    return fat32_write(&volume, path, size ? data : empty, (uint32_t)size);
}

int target_finish(void) {
    const char installed[] = "SETUP=0\r\n";
    uint32_t lba, size;
    if (fat32_write(&volume, "\\SYSTEM\\SETUP.CFG", installed, sizeof(installed) - 1)) return -1;
    if (fat32_file_lba(&volume, "\\SYSTEM\\LOADER.BIN", &lba, &size)) return -1;
    put32(boot + 0x1a4, lba); put16(boot + 0x1a8, (size + 511) / 512); put32(boot + 0x1ac, total_sectors);
    if (fat32_file_lba(&volume, "\\SYSTEM\\SETUP.CFG", &lba, &size)) return -1;
    put32(boot + 0x1b4, lba);
    if (fat32_file_lba(&volume, "\\SYSTEM\\DOSDATA.BIN", &lba, &size)) return -1;
    put32(boot + 0x1b8, lba);
    if (cached_write(0, PARTITION_START, boot) || cached_write(0, PARTITION_START + 6, boot)) return -1;
    /* Invalidate a backup GPT header left at the end of the disk. */
    unsigned char sector[512];
    if (cached_read(0, total_sectors - 1, sector)) return -1;
    if (get32(sector) == 0x20494645 && get32(sector + 4) == 0x54524150) {
        memset(sector, 0, 512);
        if (cached_write(0, total_sectors - 1, sector)) return -1;
    }
    if (flush()) return -1;
    memset(mbr + 446, 0, 64);
    mbr[446] = 0x80; mbr[447] = mbr[448] = mbr[449] = 0xff; mbr[450] = 0xef; mbr[451] = mbr[452] = mbr[453] = 0xff;
    put32(mbr + 454, PARTITION_START); put32(mbr + 458, total_sectors - PARTITION_START);
    if (cached_write(0, 0, mbr) || flush()) return -1;
    dosxc_sys_disk(DOSXC_DISK_RESCAN, 0, 0, 0, 0);
    return 0;
}
