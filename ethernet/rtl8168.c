/*
 * DOS-XC /ethernet/rtl8168.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include <dosxc/driver.h>

#define MAC0 0x00
#define MAR0 0x08
#define TX_DESC_LOW 0x20
#define TX_DESC_HIGH 0x24
#define CHIP_CMD 0x37
#define TX_POLL 0x38
#define INTR_MASK 0x3c
#define INTR_STATUS 0x3e
#define TX_CONFIG 0x40
#define RX_CONFIG 0x44
#define CFG9346 0x50
#define PHYAR 0x60
#define PHY_STATUS 0x6c
#define GPHY_OCP 0xb8
#define RX_MAX_SIZE 0xda
#define CPLUS_CMD 0xe0
#define RX_DESC_LOW 0xe4
#define RX_DESC_HIGH 0xe8
#define MAX_TX_PACKET 0xec
#define MISC 0xf0

#define CMD_RESET 0x10
#define CMD_RX 0x08
#define CMD_TX 0x04
#define POLL_NPQ 0x40
#define ACCEPT_BROADCAST 0x08
#define ACCEPT_MULTICAST 0x04
#define ACCEPT_MINE 0x02
#define RXDV_GATED (1u << 19)

#define DESC_OWN (1u << 31)
#define DESC_END (1u << 30)
#define DESC_FIRST (1u << 29)
#define DESC_LAST (1u << 28)
#define RX_ERROR (1u << 21)

#define RX_COUNT 128
#define TX_COUNT 64
#define BUFFER_SIZE 2048

struct descriptor { dosxc_u32 opts1, opts2; dosxc_u64 address; };

/* Families that change how the chip is started. */
enum { OLD_8169, FAST_8101, GIGA_8168, GIGA_8168F, GIGA_8168G };

struct adapter {
    volatile void *regs;
    unsigned short port;               /* I/O BAR when the memory BAR cannot be mapped */
    struct descriptor *rx, *tx;
    unsigned char *rx_buffers, *tx_buffers;
    unsigned rx_next, tx_next, tx_clean;
    int handle, link, family;
    dosxc_u32 xid;
    dosxc_u64 next_link_check;
    struct dk_pci_device device;
};

static const struct dk_api *dk;

static const struct dk_pci_id ids[] = {
    {0x10ec, 0x8168}, /* RTL8111/8168 B..H, 8411 */
    {0x10ec, 0x8161}, /* RTL8111/8168 (some boards) */
    {0x10ec, 0x8136}, /* RTL8101E/8102E/8103E/8105E/8106E (100 Mbit/s) */
    {0x10ec, 0x8167}, /* RTL8110SC/8169SC */
    {0x10ec, 0x8169}, /* RTL8169 (PCI) */
    {0x10ec, 0x2502}, {0x10ec, 0x2600},
    {0x1186, 0x4300}, {0x1186, 0x4302}, /* D-Link DGE-528T */
    {0x1259, 0xc107}, /* Corega */
    {0x16ec, 0x0116}, /* US Robotics */
    {0x1737, 0x1032}, /* Linksys */
    {0, 0},
};

static unsigned char r8(struct adapter *a, unsigned reg) { return a->regs ? dk_read8(a->regs, reg) : dk_inb((unsigned short)(a->port + reg)); }
static unsigned short r16(struct adapter *a, unsigned reg) { return a->regs ? dk_read16(a->regs, reg) : dk_inw((unsigned short)(a->port + reg)); }
static dosxc_u32 r32(struct adapter *a, unsigned reg) { return a->regs ? dk_read32(a->regs, reg) : dk_inl((unsigned short)(a->port + reg)); }
static void w8(struct adapter *a, unsigned reg, unsigned char v) { if (a->regs) dk_write8(a->regs, reg, v); else dk_outb((unsigned short)(a->port + reg), v); }
static void w16(struct adapter *a, unsigned reg, unsigned short v) { if (a->regs) dk_write16(a->regs, reg, v); else dk_outw((unsigned short)(a->port + reg), v); }
static void w32(struct adapter *a, unsigned reg, dosxc_u32 v) { if (a->regs) dk_write32(a->regs, reg, v); else dk_outl((unsigned short)(a->port + reg), v); }

/* TX_CONFIG's version bits, as r8169's rtl8169_get_mac_version reads them. */
static int family_of(struct adapter *a) {
    dosxc_u32 x = a->xid & 0x7c8;
    if (x == 0x4c0 || x == 0x500 || x == 0x508 || x == 0x5c8 || x == 0x540 || x == 0x548) return GIGA_8168G;
    if (x == 0x480 || x == 0x488) return GIGA_8168F;
    if (x == 0x248 || x == 0x340 || x == 0x348 || x == 0x408 || x == 0x440 || x == 0x448) return FAST_8101;
    if (x >= 0x280 || x == 0x300 || x == 0x380) return GIGA_8168;
    return OLD_8169;
}

static int phy_read(struct adapter *a, unsigned reg, unsigned short *value) {
    if (a->family == GIGA_8168G) {
        w32(a, GPHY_OCP, (0xa400u + reg * 2) << 15);
        for (int i = 0; i < 100; ++i) {
            dk->sleep(10000);
            dosxc_u32 v = r32(a, GPHY_OCP);
            if (v & (1u << 31)) { *value = (unsigned short)v; return 0; }
        }
        return -1;
    }
    w32(a, PHYAR, (reg & 31u) << 16);
    for (int i = 0; i < 100; ++i) {
        dk->sleep(25000);
        dosxc_u32 v = r32(a, PHYAR);
        if (v & (1u << 31)) { *value = (unsigned short)v; dk->sleep(20000); return 0; }
    }
    return -1;
}

static int phy_write(struct adapter *a, unsigned reg, unsigned short value) {
    if (a->family == GIGA_8168G) {
        w32(a, GPHY_OCP, 1u << 31 | (0xa400u + reg * 2) << 15 | value);
        for (int i = 0; i < 100; ++i) {
            dk->sleep(10000);
            if (!(r32(a, GPHY_OCP) & (1u << 31))) return 0;
        }
        return -1;
    }
    w32(a, PHYAR, 1u << 31 | (reg & 31u) << 16 | value);
    for (int i = 0; i < 100; ++i) {
        dk->sleep(25000);
        if (!(r32(a, PHYAR) & (1u << 31))) { dk->sleep(20000); return 0; }
    }
    return -1;
}

/* Windows and suspend leave the PHY powered down; wake it and autonegotiate. */
static void wake_phy(struct adapter *a) {
    unsigned short bmcr;
    if (phy_read(a, 0, &bmcr) || bmcr == 0xffff) { dk->log("RTL8168: the PHY does not answer"); return; }
    if (bmcr & 0x0800) dk->log("RTL8168: the PHY was powered down");
    phy_write(a, 0, (unsigned short)((bmcr & ~0x0800u) | 0x1200u));
}

static int transmit(void *instance, const void *frame, unsigned length) {
    struct adapter *a = instance;
    if (length > BUFFER_SIZE) return -1;
    while (a->tx_clean != a->tx_next && !(a->tx[a->tx_clean].opts1 & DESC_OWN))
        a->tx_clean = (a->tx_clean + 1) % TX_COUNT;
    unsigned next = (a->tx_next + 1) % TX_COUNT;
    if (next == a->tx_clean) return -1;
    struct descriptor *d = &a->tx[a->tx_next];
    unsigned char *buffer = a->tx_buffers + a->tx_next * BUFFER_SIZE;
    __builtin_memcpy(buffer, frame, length);
    if (length < 60) { __builtin_memset(buffer + length, 0, 60 - length); length = 60; }
    d->address = dk->dma_address(buffer);
    d->opts2 = 0;
    dk_barrier();
    d->opts1 = DESC_OWN | DESC_FIRST | DESC_LAST | (a->tx_next == TX_COUNT - 1 ? DESC_END : 0) | length;
    a->tx_next = next;
    dk_barrier();
    w8(a, TX_POLL, POLL_NPQ);
    return 0;
}

static const struct dk_net_ops ops = {transmit};

static void check_link(struct adapter *a) {
    unsigned char status = r8(a, PHY_STATUS);
    int up = (status & 0x02) != 0;
    if (up == a->link) return;
    a->link = up;
    dk->net_link(a->handle, up, status & 0x10 ? 1000 : status & 0x08 ? 100 : 10);
    dk->log("RTL8168: link %s", up ? "up" : "down");
}

static void give_rx(struct adapter *a, unsigned i) {
    a->rx[i].opts2 = 0;
    dk_barrier();
    a->rx[i].opts1 = DESC_OWN | (i == RX_COUNT - 1 ? DESC_END : 0) | BUFFER_SIZE;
}

static void poll(void *instance) {
    struct adapter *a = instance;
    for (unsigned budget = 0; budget < RX_COUNT; ++budget) {
        struct descriptor *d = &a->rx[a->rx_next];
        dosxc_u32 status = d->opts1;
        if (status & DESC_OWN) break;
        unsigned length = status & 0x3fffu;
        if (!(status & RX_ERROR) && (status & DESC_FIRST) && (status & DESC_LAST) && length >= 18 && length <= BUFFER_SIZE)
            dk->net_receive(a->handle, a->rx_buffers + a->rx_next * BUFFER_SIZE, length - 4);
        give_rx(a, a->rx_next);
        a->rx_next = (a->rx_next + 1) % RX_COUNT;
    }
    unsigned short pending = r16(a, INTR_STATUS);
    if (pending) w16(a, INTR_STATUS, pending);
    if (pending & 0x0010) dk->log("RTL8168: receive ring overflowed");
    dosxc_u64 now = dk->nanoseconds();
    if (now >= a->next_link_check) {
        a->next_link_check = now + 500000000ull;
        check_link(a);
    }
}

static void free_adapter(struct adapter *a) {
    if (a->rx) dk->dma_free(a->rx);
    if (a->tx) dk->dma_free(a->tx);
    if (a->rx_buffers) dk->dma_free(a->rx_buffers);
    if (a->tx_buffers) dk->dma_free(a->tx_buffers);
    dk->free(a);
}

static int load(const struct dk_api *api) { dk = api; return 0; }

static int probe(const struct dk_pci_device *device, void **instance) {
    struct adapter *a = dk->alloc(sizeof(*a));
    if (!a) return -1;
    a->device = *device;
    dosxc_u64 io_port = 0;
    for (int i = 0; i < 6 && !a->regs; ++i) {
        int io = 0;
        dosxc_u64 bar = dk->pci_bar(device, i, &io);
        if (!bar) continue;
        if (io) { if (!io_port) io_port = bar; continue; }
        a->regs = dk->mmio_map(bar, 256);
        if (!(dk->pci_read32(device, 0x10 + i * 4) & 1) && ((dk->pci_read32(device, 0x10 + i * 4) >> 1) & 3) == 2) ++i;
    }
    if (!a->regs && !io_port) { dk->log("RTL8168: no usable register BAR"); dk->free(a); return -1; }
    if (!a->regs) a->port = (unsigned short)io_port;
    dk->pci_enable(device, a->regs != 0, a->regs == 0);

    a->xid = r32(a, TX_CONFIG) >> 20 & 0xfcf;
    a->family = family_of(a);
    if (device->id == 0x8169 || device->id == 0x8167) a->family = OLD_8169;

    unsigned char mac[6];
    for (int i = 0; i < 6; ++i) mac[i] = r8(a, MAC0 + i);
    if ((mac[0] & 1) || !(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5])) {
        dk->log("RTL8168: no valid MAC address");
        dk->free(a);
        return -1;
    }

    w16(a, INTR_MASK, 0);
    w16(a, INTR_STATUS, 0xffff);
    w32(a, RX_CONFIG, r32(a, RX_CONFIG) & ~0x3fu);
    dk->sleep(2000000);
    w8(a, CHIP_CMD, CMD_RESET);
    dosxc_u64 deadline = dk->nanoseconds() + 100000000ull;
    while ((r8(a, CHIP_CMD) & CMD_RESET) && dk->nanoseconds() < deadline) dk->sleep(100000);
    if (r8(a, CHIP_CMD) & CMD_RESET) { dk->log("RTL8168: the chip did not leave reset"); dk->free(a); return -1; }

    a->rx = dk->dma_alloc(sizeof(struct descriptor) * RX_COUNT, 256);
    a->tx = dk->dma_alloc(sizeof(struct descriptor) * TX_COUNT, 256);
    a->rx_buffers = dk->dma_alloc(RX_COUNT * BUFFER_SIZE, 4096);
    a->tx_buffers = dk->dma_alloc(TX_COUNT * BUFFER_SIZE, 4096);
    if (!a->rx || !a->tx || !a->rx_buffers || !a->tx_buffers) { dk->log("RTL8168: out of DMA memory"); free_adapter(a); return -1; }
    for (unsigned i = 0; i < RX_COUNT; ++i) {
        a->rx[i].address = dk->dma_address(a->rx_buffers + i * BUFFER_SIZE);
        give_rx(a, i);
    }
    a->tx[TX_COUNT - 1].opts1 = DESC_END;

    wake_phy(a);
    w8(a, CFG9346, 0xc0);
    if (a->family == GIGA_8168G || a->family == GIGA_8168F) w32(a, MISC, r32(a, MISC) & ~RXDV_GATED);
    if (a->family >= GIGA_8168) w8(a, MAX_TX_PACKET, 0x3f);
    w16(a, CPLUS_CMD, (unsigned short)((r16(a, CPLUS_CMD) & ~0x0060u) | (a->family == OLD_8169 ? 0x0008u : 0)));
    w16(a, RX_MAX_SIZE, BUFFER_SIZE);
    dosxc_u64 address = dk->dma_address(a->tx);
    w32(a, TX_DESC_HIGH, (dosxc_u32)(address >> 32));
    w32(a, TX_DESC_LOW, (dosxc_u32)address);
    address = dk->dma_address(a->rx);
    w32(a, RX_DESC_HIGH, (dosxc_u32)(address >> 32));
    w32(a, RX_DESC_LOW, (dosxc_u32)address);
    w8(a, CFG9346, 0);
    w8(a, CHIP_CMD, CMD_TX | CMD_RX);

    dosxc_u32 rx_config = 7u << 8;                                  /* unlimited DMA burst */
    if (a->family == OLD_8169) rx_config |= 7u << 13;              /* no receive FIFO threshold */
    else if (a->family == FAST_8101) rx_config |= 1u << 15;
    else rx_config |= 1u << 15 | 1u << 14 | (a->family == GIGA_8168G ? 1u << 11 : 0);
    w32(a, TX_CONFIG, 7u << 8 | 3u << 24 | (a->family >= GIGA_8168F ? 1u << 7 : 0));
    w32(a, MAR0, 0xffffffffu);
    w32(a, MAR0 + 4, 0xffffffffu);
    w32(a, RX_CONFIG, rx_config | ACCEPT_BROADCAST | ACCEPT_MULTICAST | ACCEPT_MINE);

    char name[16];
    dk->format(name, sizeof(name), "RTL/%04x", device->id);
    a->handle = dk->net_register(&ops, a, mac, name);
    if (a->handle < 0) { free_adapter(a); return -1; }
    dk->log("RTL8168: %04x:%04x at %02x:%02x.%u, chip %x, MAC %02x:%02x:%02x:%02x:%02x:%02x", device->vendor, device->id,
            device->bus, device->device, device->function, a->xid, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    check_link(a);
    *instance = a;
    return 0;
}

static void remove_adapter(void *instance) {
    struct adapter *a = instance;
    w32(a, RX_CONFIG, r32(a, RX_CONFIG) & ~0x3fu);
    w8(a, CHIP_CMD, 0);
    dk->sleep(2000000);
    w8(a, CHIP_CMD, CMD_RESET);
    dk->sleep(10000000);
    dk->net_unregister(a->handle);
    dk->pci_write16(&a->device, 4, (unsigned short)(dk->pci_read16(&a->device, 4) & ~4u));
    free_adapter(a);
}

DK_DRIVER = {DK_ABI_VERSION, sizeof(struct dk_driver), "Realtek RTL8168", "1.0",
             "Realtek RTL8111/8168/8101/8169 Ethernet", ids, load, probe, poll, remove_adapter, 0};
