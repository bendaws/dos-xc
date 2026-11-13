/*
 * DOS-XC /os/runtime/ahci.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "storage.h"

#define CAP 0x00
#define GHC 0x04
#define PI 0x0c
#define CAP2 0x24
#define BOHC 0x28
enum { CLB = 0x00, CLBU = 0x04, FB = 0x08, FBU = 0x0c, IS = 0x10, IE = 0x14, CMD = 0x18, TFD = 0x20,
       SIG = 0x24, SSTS = 0x28, SERR = 0x30, CI = 0x38 };
#define CMD_ST 1u
#define CMD_SUD 2u
#define CMD_POD 4u
#define CMD_FRE 0x10u
#define CMD_FR 0x4000u
#define CMD_CR 0x8000u
#define IS_TFES 0x40000000u

struct port {
    struct blockdev dev;
    volatile u8 *regs;
    u32 *list;                         /* command list: 32 headers, slot 0 used */
    u8 *fis;                           /* received FIS area */
    u8 *table;                         /* command table: FIS, ATAPI packet, PRDT */
    int failed;
};

static struct port ports[16];
static unsigned port_count;

static volatile u32 *reg(struct port *p, u32 offset) { return (volatile u32 *)(p->regs + offset); }

static int stop(struct port *p) {
    *reg(p, CMD) &= ~CMD_ST;
    if (storage_wait(reg(p, CMD), CMD_CR, 0, 500)) return -1;
    *reg(p, CMD) &= ~CMD_FRE;
    return storage_wait(reg(p, CMD), CMD_FR, 0, 500);
}

static int start(struct port *p) {
    if (storage_wait(reg(p, TFD), 0x88, 0, 1000)) return -1;   /* BSY, DRQ */
    *reg(p, CMD) |= CMD_FRE;
    *reg(p, CMD) |= CMD_ST;
    return 0;
}

static int setup(struct port *p) {
    if (stop(p)) return -1;
    *reg(p, CLB) = dma_address(p->list);
    *reg(p, CLBU) = 0;
    *reg(p, FB) = dma_address(p->fis);
    *reg(p, FBU) = 0;
    *reg(p, IE) = 0;
    *reg(p, SERR) = 0xffffffffu;
    *reg(p, IS) = 0xffffffffu;
    *reg(p, CMD) |= CMD_SUD | CMD_POD;
    return start(p);
}

/* COMMAND with DATA (bytes; 0 = none).  CDB: an ATAPI packet, or 0. */
static int run(struct port *p, u8 command, u64 lba, u32 count, void *data, u32 bytes, int write, const u8 *cdb) {
    u8 *t = p->table;
    memset(t, 0, 0x80 + 16 * 2);
    t[0] = 0x27;                                           /* H2D register FIS */
    t[1] = 0x80;                                           /* command */
    t[2] = command;
    t[3] = cdb ? 1 : 0;
    t[4] = (u8)lba; t[5] = (u8)(lba >> 8); t[6] = (u8)(lba >> 16);
    t[7] = 0x40;
    t[8] = (u8)(lba >> 24); t[9] = (u8)(lba >> 32); t[10] = (u8)(lba >> 40);
    t[12] = (u8)count; t[13] = (u8)(count >> 8);
    if (cdb) memcpy(t + 0x40, cdb, 12);
    u32 entries = 0;
    if (bytes) {
        u32 *prd = (u32 *)(t + 0x80);
        prd[0] = dma_address(data);
        prd[1] = 0;
        prd[2] = 0;
        prd[3] = bytes - 1;
        entries = 1;
    }
    p->list[0] = 5 | (cdb ? 0x20 : 0) | (write ? 0x40 : 0) | entries << 16;
    p->list[1] = 0;
    p->list[2] = dma_address(t);
    p->list[3] = 0;
    *reg(p, IS) = 0xffffffffu;
    *reg(p, CI) = 1;
    u64 end = cpu_nanoseconds() + 20000000000ull;
    for (;;) {
        if (*reg(p, IS) & IS_TFES) break;
        if (!(*reg(p, CI) & 1)) break;
        if (cpu_nanoseconds() > end) break;
        __asm__ volatile("pause");
    }
    int bad = (*reg(p, CI) & 1) || (*reg(p, IS) & IS_TFES) || (*reg(p, TFD) & 1);
    if (bad) setup(p);
    return bad ? -1 : 0;
}

static int disk_transfer(struct blockdev *dev, u64 block, u32 blocks, void *buffer, int write) {
    struct port *p = (struct port *)dev;
    return run(p, write ? 0x35 : 0x25, block, blocks, buffer, blocks * 512, write, 0);
}

static int disk_flush(struct blockdev *dev) {
    return run((struct port *)dev, 0xea, 0, 0, 0, 0, 0, 0);
}

static int cd_capacity(struct blockdev *dev) {
    struct port *p = (struct port *)dev;
    u8 cdb[12] = {0x25};
    u8 *reply = p->table + 0x100;
    for (int tries = 0; tries < 3; ++tries)
        if (!run(p, 0xa0, 0, 0, reply, 8, 0, cdb)) {
            u32 last = (u32)reply[0] << 24 | (u32)reply[1] << 16 | (u32)reply[2] << 8 | reply[3];
            u32 size = (u32)reply[4] << 24 | (u32)reply[5] << 16 | (u32)reply[6] << 8 | reply[7];
            if (size != 2048) break;
            dev->blocks = (u64)last + 1;
            return 0;
        }
    dev->blocks = 1;
    return -1;
}

static int cd_transfer(struct blockdev *dev, u64 block, u32 blocks, void *buffer, int write) {
    if (write) return -1;
    struct port *p = (struct port *)dev;
    u8 cdb[12] = {0x28, 0, (u8)(block >> 24), (u8)(block >> 16), (u8)(block >> 8), (u8)block,
                  0, (u8)(blocks >> 8), (u8)blocks};
    for (int tries = 0; tries < 3; ++tries)
        if (!run(p, 0xa0, 0, 0, buffer, blocks * 2048, 0, cdb)) return 0;
    return -1;
}

static void add_port(volatile u8 *abar, unsigned index) {
    if (port_count == sizeof(ports) / sizeof(ports[0])) return;
    struct port *p = &ports[port_count];
    memset(p, 0, sizeof(*p));
    p->regs = abar + 0x100 + index * 0x80;
    if ((*reg(p, SSTS) & 0xf) != 3) return;                /* no device, or no link */
    u32 sig = *reg(p, SIG);
    if (sig != 0x00000101u && sig != 0xeb140101u) return;
    p->list = dma_alloc(1024, 1024);
    p->fis = dma_alloc(256, 256);
    p->table = dma_alloc(0x300, 128);       /* + a 512-byte reply buffer at 0x100 */
    if (!p->list || !p->fis || !p->table || setup(p)) return;
    p->dev.driver = p;
    if (sig == 0xeb140101u) {
        rt_strcpy(p->dev.kind, "CD");
        p->dev.atapi = p->dev.readonly = p->dev.removable = 1;
        p->dev.block_size = 2048;
        p->dev.max_blocks = 64;
        p->dev.transfer = cd_transfer;
        p->dev.capacity = cd_capacity;
        cd_capacity(&p->dev);
    } else {
        u16 *id = (u16 *)(p->table + 0x100);
        if (run(p, 0xec, 0, 0, id, 512, 0, 0)) return;
        u64 sectors = (u64)id[100] | (u64)id[101] << 16 | (u64)id[102] << 32 | (u64)id[103] << 48;
        if (!(id[83] & 0x400) || !sectors) sectors = (u32)id[60] | (u32)id[61] << 16;
        if (!sectors) return;
        rt_strcpy(p->dev.kind, "SATA");
        p->dev.block_size = 512;
        p->dev.blocks = sectors;
        p->dev.max_blocks = 256;
        p->dev.transfer = disk_transfer;
        p->dev.flush = disk_flush;
    }
    if (!storage_add(&p->dev)) ++port_count;
}

void ahci_probe(const struct pci_function *f) {
    if (f->prog_if != 1) return;
    int io;
    u64 base = pci_bar_kernel(f, 5, &io);
    if (!base || io) return;
    pci_enable(f, 1, 0);
    storage_claim();
    volatile u8 *abar = (volatile u8 *)(uptr)base;
    volatile u32 *ghc = (volatile u32 *)(abar + GHC);
    if (*(volatile u32 *)(abar + CAP2) & 1) {
        volatile u32 *bohc = (volatile u32 *)(abar + BOHC);
        *bohc |= 2;
        storage_wait(bohc, 1, 0, 2000);
    }
    *ghc |= 0x80000000u;
    *ghc &= ~2u;
    u32 implemented = *(volatile u32 *)(abar + PI);
    for (unsigned i = 0; i < 32; ++i)
        if (implemented & (1u << i)) add_port(abar, i);
}
