/*
 * DOS-XC /os/runtime/ata.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "storage.h"

static inline u16 inw(u16 port) { u16 v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(port)); return v; }
static inline void outl(u16 port, u32 v) { __asm__ volatile("outl %0,%1" ::"a"(v), "Nd"(port)); }
static inline void insw(u16 port, void *p, uptr n) { __asm__ volatile("rep insw" : "+D"(p), "+c"(n) : "d"(port) : "memory"); }
static inline void outsw(u16 port, const void *p, uptr n) { __asm__ volatile("rep outsw" : "+S"(p), "+c"(n) : "d"(port) : "memory"); }

enum { DATA = 0, ERROR = 1, FEATURES = 1, COUNT = 2, LBA0 = 3, LBA1 = 4, LBA2 = 5, DEVICE = 6, STATUS = 7, COMMAND = 7 };
#define BSY 0x80
#define DF 0x20
#define DRQ 0x08
#define ERR 0x01

struct channel {
    u16 io, control, bm;              /* bm: bus-master registers, 0 = none */
    u32 *prdt;                        /* 2 entries */
};
struct ata {
    struct blockdev dev;
    struct channel *ch;
    int slave, lba48, dma;
};

static struct channel channels[8];
static unsigned channel_count;
static struct ata drives[16];
static unsigned drive_count;

static void delay400(struct channel *c) { for (int i = 0; i < 4; ++i) rt_inb(c->control); }

static int wait_idle(struct channel *c, u64 ms) {
    u64 end = cpu_nanoseconds() + ms * 1000000ull;
    for (;;) {
        u8 s = rt_inb(c->control);
        if (s == 0xff) return -1;                      /* nothing there */
        if (!(s & BSY)) return s;
        if (cpu_nanoseconds() > end) return -1;
    }
}

static int wait_drq(struct channel *c, u64 ms) {
    u64 end = cpu_nanoseconds() + ms * 1000000ull;
    for (;;) {
        u8 s = rt_inb(c->control);
        if (!(s & BSY)) {
            if (s & (ERR | DF)) return -1;
            if (s & DRQ) return 0;
        }
        if (cpu_nanoseconds() > end) return -1;
    }
}

static int select(struct channel *c, int slave, u8 bits) {
    if (wait_idle(c, 2000) < 0) return -1;
    rt_outb((u16)(c->io + DEVICE), (u8)(0xa0 | bits | (slave ? 0x10 : 0)));
    delay400(c);
    return wait_idle(c, 2000) < 0 ? -1 : 0;
}

/* Registers for LBA and COUNT (LBA48 when EXT). */
static void task_file(struct ata *a, u64 lba, u32 count, int ext) {
    struct channel *c = a->ch;
    if (ext) {
        rt_outb((u16)(c->io + COUNT), (u8)(count >> 8));
        rt_outb((u16)(c->io + LBA0), (u8)(lba >> 24));
        rt_outb((u16)(c->io + LBA1), (u8)(lba >> 32));
        rt_outb((u16)(c->io + LBA2), (u8)(lba >> 40));
    }
    rt_outb((u16)(c->io + COUNT), (u8)count);
    rt_outb((u16)(c->io + LBA0), (u8)lba);
    rt_outb((u16)(c->io + LBA1), (u8)(lba >> 8));
    rt_outb((u16)(c->io + LBA2), (u8)(lba >> 16));
}

/* ---- bus-master DMA --------------------------------------------------- */

static void dma_prepare(struct channel *c, void *buffer, u32 bytes, int to_memory) {
    u32 a = dma_address(buffer);
    int n = 0;
    while (bytes) {
        u32 room = 0x10000 - (a & 0xffff), part = bytes < room ? bytes : room;
        c->prdt[n * 2] = a;
        c->prdt[n * 2 + 1] = part & 0xffff;   /* 0 means 64 KiB */
        a += part;
        bytes -= part;
        ++n;
    }
    c->prdt[n * 2 - 1] |= 0x80000000u;
    rt_outb(c->bm, 0);
    outl((u16)(c->bm + 4), dma_address(c->prdt));
    rt_outb(c->bm, to_memory ? 8 : 0);
    rt_outb((u16)(c->bm + 2), 6);
}

/* Run the engine until the device is done.  0, or -1 after an error. */
static int dma_run(struct channel *c, u64 ms) {
    rt_outb(c->bm, (u8)(rt_inb(c->bm) | 1));
    u64 end = cpu_nanoseconds() + ms * 1000000ull;
    int failed = 0;
    for (;;) {
        u8 bm = rt_inb((u16)(c->bm + 2)), s = rt_inb(c->control);
        if (bm & 2) { failed = 1; break; }
        if (!(s & BSY) && (s & (ERR | DF))) { failed = 1; break; }
        if ((bm & 4) && !(s & BSY)) break;
        if (!(bm & 1) && !(s & BSY) && !(s & DRQ)) break;
        if (cpu_nanoseconds() > end) { failed = 1; break; }
    }
    rt_outb(c->bm, (u8)(rt_inb(c->bm) & ~1));
    u8 s = rt_inb((u16)(c->io + STATUS));
    rt_outb((u16)(c->bm + 2), 6);
    return failed || (s & (ERR | DF)) ? -1 : 0;
}

static void soft_reset(struct channel *c) {
    rt_outb(c->control, 6);
    cpu_sleep(5000);
    rt_outb(c->control, 2);
    cpu_sleep(2000000);
    wait_idle(c, 3000);
}

/* ---- hard disks ------------------------------------------------------- */

static int ata_pio(struct ata *a, u64 lba, u32 count, u8 *buffer, int write) {
    struct channel *c = a->ch;
    int ext = a->lba48 && (lba + count > 0x0fffffff || count > 256);
    if (select(c, a->slave, (u8)(0x40 | (ext ? 0 : (lba >> 24) & 15)))) return -1;
    task_file(a, lba, count, ext);
    rt_outb((u16)(c->io + COMMAND), write ? (ext ? 0x34 : 0x30) : (ext ? 0x24 : 0x20));
    for (u32 i = 0; i < count; ++i) {
        delay400(c);
        if (wait_drq(c, 5000)) return -1;
        if (write) outsw(c->io, buffer + i * 512, 256);
        else insw(c->io, buffer + i * 512, 256);
    }
    delay400(c);
    int s = wait_idle(c, 5000);
    return s < 0 || (s & (ERR | DF)) ? -1 : 0;
}

static int ata_dma(struct ata *a, u64 lba, u32 count, u8 *buffer, int write) {
    struct channel *c = a->ch;
    int ext = a->lba48 && (lba + count > 0x0fffffff || count > 256);
    if (select(c, a->slave, (u8)(0x40 | (ext ? 0 : (lba >> 24) & 15)))) return -1;
    dma_prepare(c, buffer, count * 512, !write);
    task_file(a, lba, count, ext);
    rt_outb((u16)(c->io + COMMAND), write ? (ext ? 0x35 : 0xca) : (ext ? 0x25 : 0xc8));
    return dma_run(c, 10000);
}

static int ata_transfer(struct blockdev *dev, u64 block, u32 blocks, void *buffer, int write) {
    struct ata *a = (struct ata *)dev;
    if (a->dma) {
        if (!ata_dma(a, block, blocks, buffer, write)) return 0;
        soft_reset(a->ch);
        if (!ata_dma(a, block, blocks, buffer, write)) return 0;
        a->dma = 0;
        soft_reset(a->ch);
    }
    return ata_pio(a, block, blocks, buffer, write);
}

static int ata_flush(struct blockdev *dev) {
    struct ata *a = (struct ata *)dev;
    if (select(a->ch, a->slave, 0x40)) return -1;
    rt_outb((u16)(a->ch->io + COMMAND), a->lba48 ? 0xea : 0xe7);
    delay400(a->ch);
    return wait_idle(a->ch, 30000) < 0 ? -1 : 0;
}

/* ---- ATAPI (CD/DVD) --------------------------------------------------- */

/* A packet command; data (at most 64 KiB) by DMA when DMA is set, else PIO. */
static int packet(struct ata *a, const u8 *cdb, void *buffer, u32 bytes, int dma) {
    struct channel *c = a->ch;
    if (select(c, a->slave, 0)) return -1;
    if (dma) dma_prepare(c, buffer, bytes, 1);
    rt_outb((u16)(c->io + FEATURES), dma ? 1 : 0);
    u32 limit = bytes > 0xfffe ? 0xfffe : (bytes ? bytes : 2);
    rt_outb((u16)(c->io + LBA1), (u8)limit);
    rt_outb((u16)(c->io + LBA2), (u8)(limit >> 8));
    rt_outb((u16)(c->io + COMMAND), 0xa0);
    delay400(c);
    if (wait_drq(c, 2000)) return -1;
    outsw(c->io, cdb, 6);
    if (dma) return dma_run(c, 20000);
    u8 *out = buffer;
    u32 done = 0;
    for (;;) {
        delay400(c);
        int s = wait_idle(c, 20000);
        if (s < 0 || (s & (ERR | DF))) return -1;
        if (!(s & DRQ)) break;
        u32 n = rt_inb((u16)(c->io + LBA1)) | (u32)rt_inb((u16)(c->io + LBA2)) << 8;
        for (u32 i = 0; i < n; i += 2) {
            u16 w = inw(c->io);
            if (done + 1 < bytes) { out[done] = (u8)w; out[done + 1] = (u8)(w >> 8); }
            done += 2;
        }
    }
    return done >= bytes ? 0 : -1;
}

static int atapi_capacity(struct blockdev *dev) {
    struct ata *a = (struct ata *)dev;
    u8 cdb[12] = {0x25}, reply[8];
    for (int tries = 0; tries < 3; ++tries)
        if (!packet(a, cdb, reply, 8, 0)) {
            u32 last = (u32)reply[0] << 24 | (u32)reply[1] << 16 | (u32)reply[2] << 8 | reply[3];
            u32 size = (u32)reply[4] << 24 | (u32)reply[5] << 16 | (u32)reply[6] << 8 | reply[7];
            if (size != 2048) return -1;
            dev->blocks = (u64)last + 1;
            return 0;
        }
    dev->blocks = 0;
    return -1;
}

static int atapi_transfer(struct blockdev *dev, u64 block, u32 blocks, void *buffer, int write) {
    struct ata *a = (struct ata *)dev;
    if (write) return -1;
    u8 *out = buffer;
    while (blocks) {
        u32 n = blocks > 32 ? 32 : blocks;
        u8 cdb[12] = {0x28, 0, (u8)(block >> 24), (u8)(block >> 16), (u8)(block >> 8), (u8)block,
                      0, (u8)(n >> 8), (u8)n};
        int ok = 0;
        for (int tries = 0; tries < 3 && !ok; ++tries) {
            if (a->dma && !packet(a, cdb, out, n * 2048, 1)) { ok = 1; break; }
            if (a->dma) { soft_reset(a->ch); if (tries) a->dma = 0; }
            if (!a->dma && !packet(a, cdb, out, n * 2048, 0)) ok = 1;
        }
        if (!ok) return -1;
        out += n * 2048;
        block += n;
        blocks -= n;
    }
    return 0;
}

/* ---- finding the devices ---------------------------------------------- */

static void identify(struct channel *c, int slave) {
    if (drive_count == sizeof(drives) / sizeof(drives[0])) return;
    rt_outb((u16)(c->io + DEVICE), (u8)(0xa0 | (slave ? 0x10 : 0)));
    delay400(c);
    if (rt_inb(c->control) == 0xff) return;
    if (wait_idle(c, 1000) < 0) return;
    rt_outb((u16)(c->io + COUNT), 0);
    rt_outb((u16)(c->io + LBA0), 0);
    rt_outb((u16)(c->io + LBA1), 0);
    rt_outb((u16)(c->io + LBA2), 0);
    rt_outb((u16)(c->io + COMMAND), 0xec);
    delay400(c);
    if (!rt_inb(c->control)) return;
    int s = wait_idle(c, 3000);
    if (s < 0) return;
    u8 mid = rt_inb((u16)(c->io + LBA1)), high = rt_inb((u16)(c->io + LBA2));
    int atapi = mid == 0x14 && high == 0xeb;
    if (atapi) {
        rt_outb((u16)(c->io + COMMAND), 0xa1);
        delay400(c);
        if (wait_idle(c, 3000) < 0) return;
    } else if (mid || high) {
        return;
    }
    if (wait_drq(c, 3000)) return;
    static u16 id[256];
    insw(c->io, id, 256);
    struct ata *a = &drives[drive_count];
    memset(a, 0, sizeof(*a));
    a->ch = c;
    a->slave = slave;
    a->dma = c->bm && (id[49] & 0x100);
    a->dev.driver = a;
    if (atapi) {
        if (((id[0] >> 8) & 0x1f) != 5) return;             /* only CD/DVD drives */
        rt_strcpy(a->dev.kind, "CD");
        a->dev.atapi = 1;
        a->dev.readonly = 1;
        a->dev.removable = 1;
        a->dev.block_size = 2048;
        a->dev.transfer = atapi_transfer;
        a->dev.capacity = atapi_capacity;
        if (atapi_capacity(&a->dev)) a->dev.blocks = 0;
        if (!a->dev.blocks) a->dev.blocks = 1;
    } else {
        rt_strcpy(a->dev.kind, "IDE");
        a->lba48 = (id[83] & 0x400) != 0;
        u64 sectors = (u32)id[60] | (u32)id[61] << 16;
        if (a->lba48) {
            u64 big = (u64)id[100] | (u64)id[101] << 16 | (u64)id[102] << 32 | (u64)id[103] << 48;
            if (big) sectors = big;
        }
        if (!sectors || !(id[49] & 0x200)) return;          /* LBA only */
        a->dev.block_size = 512;
        a->dev.blocks = sectors;
        a->dev.removable = (id[0] & 0x80) != 0;
        a->dev.transfer = ata_transfer;
        a->dev.flush = ata_flush;
    }
    a->dev.max_blocks = a->dev.atapi ? 64 : 256;
    if (!storage_add(&a->dev)) ++drive_count;
}

static void add_channel(u16 io, u16 control, u16 bm) {
    if (channel_count == sizeof(channels) / sizeof(channels[0]) || !io || !control) return;
    struct channel *c = &channels[channel_count];
    c->io = io;
    c->control = control;
    c->bm = bm;
    if (bm) {
        c->prdt = dma_alloc(64, 64);
        if (!c->prdt) c->bm = 0;
    }
    if (rt_inb((u16)(io + STATUS)) == 0xff && rt_inb(control) == 0xff) return;   /* floating bus */
    ++channel_count;
    rt_outb(control, 2);
    identify(c, 0);
    identify(c, 1);
}

void ata_probe(const struct pci_function *f) {
    int io;
    u16 bm = 0;
    u64 bar4 = pci_bar(f, 4, &io);
    if (bar4 && io) bm = (u16)bar4;
    pci_enable(f, 0, 1);
    storage_claim();
    for (int ch = 0; ch < 2; ++ch) {
        int native = (f->prog_if >> (ch * 2)) & 1;
        u16 cmd = ch ? 0x170 : 0x1f0, control = ch ? 0x376 : 0x3f6;
        if (native) {
            u64 a = pci_bar(f, ch * 2, &io), b = pci_bar(f, ch * 2 + 1, &io);
            if (!a || !b) continue;
            cmd = (u16)a;
            control = (u16)(b + 2);
        }
        add_channel(cmd, control, bm ? (u16)(bm + ch * 8) : 0);
    }
}
