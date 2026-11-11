/*
 * DOS-XC /os/runtime/nvme.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "storage.h"

#define QUEUE 32                          /* entries per queue */

struct queue {
    u8 *sq;                               /* QUEUE * 64 bytes */
    volatile u32 *cq;                     /* QUEUE * 16 bytes */
    volatile u32 *sq_doorbell, *cq_doorbell;
    u16 tail, head, cid;
    u8 phase;
};
struct controller {
    volatile u8 *regs;
    struct queue admin, io;
    u64 *prp_list;
    u32 max_pages;                        /* per command */
};
struct namespace {
    struct blockdev dev;
    struct controller *c;
    u32 id;
};

static struct controller controllers[4];
static unsigned controller_count;
static struct namespace namespaces[8];
static unsigned namespace_count;

static u64 read64(volatile u8 *p) { return *(volatile u32 *)p | (u64) * (volatile u32 *)(p + 4) << 32; }
static void write64(volatile u8 *p, u64 v) { *(volatile u32 *)p = (u32)v; *(volatile u32 *)(p + 4) = (u32)(v >> 32); }

/* Submit CMD (16 dwords; the command id is filled in) and wait for it. */
static int submit(struct queue *q, u32 *cmd, u64 ms) {
    u16 cid = q->cid++;
    cmd[0] = (cmd[0] & 0xffff) | (u32)cid << 16;
    memcpy(q->sq + (uptr)q->tail * 64, cmd, 64);
    q->tail = (u16)((q->tail + 1) % QUEUE);
    __asm__ volatile("" ::: "memory");
    *q->sq_doorbell = q->tail;
    u64 end = cpu_nanoseconds() + ms * 1000000ull;
    for (;;) {
        volatile u32 *e = q->cq + (uptr)q->head * 4;
        if (((e[3] >> 16) & 1) == q->phase) {
            u32 status = (e[3] >> 17) & 0x7fff;
            q->head = (u16)((q->head + 1) % QUEUE);
            if (!q->head) q->phase ^= 1;
            *q->cq_doorbell = q->head;
            if ((e[3] & 0xffff) != cid) continue;           /* an old completion */
            return status ? -1 : 0;
        }
        if (cpu_nanoseconds() > end) return -1;
        __asm__ volatile("pause");
    }
}

static int admin(struct controller *c, u8 opcode, u32 nsid, void *data, u32 cdw10, u32 cdw11) {
    u32 cmd[16] = {opcode, nsid};
    cmd[6] = dma_address(data);
    cmd[10] = cdw10;
    cmd[11] = cdw11;
    return submit(&c->admin, cmd, 5000);
}

static int ns_transfer(struct blockdev *dev, u64 block, u32 blocks, void *buffer, int write) {
    struct namespace *n = (struct namespace *)dev;
    struct controller *c = n->c;
    u32 bytes = blocks * dev->block_size, pages = (bytes + 4095) / 4096;
    u32 address = dma_address(buffer);
    u32 cmd[16] = {write ? 0x01u : 0x02u, n->id};
    cmd[6] = address;
    if (pages == 2) cmd[8] = address + 4096;
    else if (pages > 2) {
        for (u32 i = 1; i < pages; ++i) c->prp_list[i - 1] = address + i * 4096ull;
        cmd[8] = dma_address(c->prp_list);
    }
    cmd[10] = (u32)block;
    cmd[11] = (u32)(block >> 32);
    cmd[12] = blocks - 1;
    return submit(&c->io, cmd, 20000);
}

static int ns_flush(struct blockdev *dev) {
    struct namespace *n = (struct namespace *)dev;
    u32 cmd[16] = {0x00, n->id};
    return submit(&n->c->io, cmd, 30000);
}

static int make_queue(struct queue *q, volatile u8 *regs, u32 stride, int id) {
    q->sq = dma_alloc(QUEUE * 64, 4096);
    q->cq = dma_alloc(QUEUE * 16, 4096);
    if (!q->sq || !q->cq) return -1;
    q->sq_doorbell = (volatile u32 *)(regs + 0x1000 + (2 * id) * stride);
    q->cq_doorbell = (volatile u32 *)(regs + 0x1000 + (2 * id + 1) * stride);
    q->phase = 1;
    return 0;
}

void nvme_probe(const struct pci_function *f) {
    if (f->prog_if != 2 || controller_count == sizeof(controllers) / sizeof(controllers[0])) return;
    int io;
    u64 base = pci_bar(f, 0, &io);
    if (!base || io) return;
    pci_enable(f, 1, 0);
    struct controller *c = &controllers[controller_count];
    memset(c, 0, sizeof(*c));
    c->regs = (volatile u8 *)(uptr)base;
    u64 cap = read64(c->regs);
    u32 stride = 4u << ((cap >> 32) & 15), timeout = (u32)((cap >> 24) & 0xff) * 500 + 500;
    if ((cap >> 48) & 15) return;
    volatile u32 *cc = (volatile u32 *)(c->regs + 0x14), *csts = (volatile u32 *)(c->regs + 0x1c);
    *cc &= ~1u;
    if (storage_wait(csts, 1, 0, timeout)) return;
    if (make_queue(&c->admin, c->regs, stride, 0) || make_queue(&c->io, c->regs, stride, 1)) return;
    *(volatile u32 *)(c->regs + 0x24) = (QUEUE - 1) << 16 | (QUEUE - 1);   /* AQA */
    write64(c->regs + 0x28, dma_address(c->admin.sq));
    write64(c->regs + 0x30, dma_address((void *)c->admin.cq));
    *cc = 4u << 20 | 6u << 16 | 1;
    if (storage_wait(csts, 3, 1, timeout)) return;
    u8 *page = dma_alloc(4096, 4096);
    c->prp_list = dma_alloc(4096, 4096);
    if (!page || !c->prp_list) return;
    if (admin(c, 0x06, 0, page, 1, 0)) return;              /* identify controller */
    u32 mdts = page[77];
    c->max_pages = mdts ? 1u << mdts : 1024;
    u32 nn = page[516] | (u32)page[517] << 8 | (u32)page[518] << 16 | (u32)page[519] << 24;
    if (admin(c, 0x05, 0, (void *)c->io.cq, (QUEUE - 1) << 16 | 1, 1)) return;          /* I/O CQ */
    if (admin(c, 0x01, 0, c->io.sq, (QUEUE - 1) << 16 | 1, 1u << 16 | 1)) return;       /* I/O SQ */
    ++controller_count;
    if (nn > 8) nn = 8;
    for (u32 id = 1; id <= nn && namespace_count < sizeof(namespaces) / sizeof(namespaces[0]); ++id) {
        if (admin(c, 0x06, id, page, 0, 0)) continue;       /* identify namespace */
        u64 size = *(u64 *)page;
        u32 format = page[26] & 15, lbads = page[128 + format * 4 + 2];
        if (!size || (lbads != 9 && lbads != 12)) continue;
        struct namespace *n = &namespaces[namespace_count];
        memset(n, 0, sizeof(*n));
        n->c = c;
        n->id = id;
        rt_strcpy(n->dev.kind, "NVMe");
        n->dev.driver = n;
        n->dev.block_size = 1u << lbads;
        n->dev.blocks = size;
        n->dev.max_blocks = c->max_pages * 4096 / n->dev.block_size;
        n->dev.transfer = ns_transfer;
        n->dev.flush = ns_flush;
        if (!storage_add(&n->dev)) ++namespace_count;
    }
    dma_free(page, 4096);
}
