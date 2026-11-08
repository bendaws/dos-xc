/*
 * DOS-XC E1000.DRV - Intel PRO/1000 Ethernet (82540/82544/82545/82574)
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * The adapters QEMU, VirtualBox and VMware emulate, and the Intel CT
 * Desktop card.  Polled: descriptors live in memory, so receiving costs no
 * register reads until a frame has arrived.
 */

#include <dosxc/driver.h>

#define CTRL 0x0000
#define STATUS 0x0008
#define EERD 0x0014
#define ICR 0x00c0
#define IMC 0x00d8
#define RCTL 0x0100
#define TCTL 0x0400
#define TIPG 0x0410
#define RDBAL 0x2800
#define RDBAH 0x2804
#define RDLEN 0x2808
#define RDH 0x2810
#define RDT 0x2818
#define TDBAL 0x3800
#define TDBAH 0x3804
#define TDLEN 0x3808
#define TDH 0x3810
#define TDT 0x3818
#define MTA 0x5200
#define RAL 0x5400
#define RAH 0x5404

#define CTRL_SLU (1u << 6)
#define CTRL_ASDE (1u << 5)
#define CTRL_RST (1u << 26)
#define CTRL_PHY_RST (1u << 31)
#define STATUS_LU 2u
#define RCTL_EN 2u
#define RCTL_BAM (1u << 15)
#define RCTL_SECRC (1u << 26)
#define TCTL_EN 2u
#define TCTL_PSP 8u
#define CMD_EOP 1
#define CMD_IFCS 2
#define CMD_RS 8
#define STATUS_DD 1

#define RX_COUNT 128
#define TX_COUNT 64
#define BUFFER_SIZE 2048

struct rx_descriptor { dosxc_u64 address; unsigned short length, checksum; unsigned char status, errors; unsigned short special; };
struct tx_descriptor { dosxc_u64 address; unsigned short length; unsigned char cso, cmd, status, css; unsigned short special; };

struct adapter {
    volatile void *regs;
    struct rx_descriptor *rx;
    struct tx_descriptor *tx;
    unsigned char *rx_buffers, *tx_buffers;
    unsigned rx_next, tx_next, tx_clean;
    int handle, link;
    dosxc_u64 next_link_check;
    struct dk_pci_device device;
};

static const struct dk_api *dk;

static const struct dk_pci_id ids[] = {
    {0x8086, 0x100e}, /* 82540EM: QEMU e1000, VirtualBox PRO/1000 MT Desktop */
    {0x8086, 0x100f}, /* 82545EM: VMware e1000, VirtualBox MT Server */
    {0x8086, 0x100c}, /* 82544GC */
    {0x8086, 0x1004}, /* 82543GC: VirtualBox T Server */
    {0x8086, 0x1026}, /* 82545GM */
    {0x8086, 0x10d3}, /* 82574L: QEMU e1000e, VMware e1000e, Intel Gigabit CT Desktop */
    {0, 0},
};

static int transmit(void *instance, const void *frame, unsigned length) {
    struct adapter *a = instance;
    if (length > BUFFER_SIZE) return -1;
    while (a->tx_clean != a->tx_next && (a->tx[a->tx_clean].status & STATUS_DD))
        a->tx_clean = (a->tx_clean + 1) % TX_COUNT;
    unsigned next = (a->tx_next + 1) % TX_COUNT;
    if (next == a->tx_clean) return -1;      /* ring full */
    struct tx_descriptor *d = &a->tx[a->tx_next];
    unsigned char *buffer = a->tx_buffers + a->tx_next * BUFFER_SIZE;
    __builtin_memcpy(buffer, frame, length);
    d->address = dk->dma_address(buffer);
    d->length = (unsigned short)length;
    d->cmd = CMD_EOP | CMD_IFCS | CMD_RS;
    d->status = 0;
    a->tx_next = next;
    dk_barrier();
    dk_write32(a->regs, TDT, next);
    return 0;
}

static const struct dk_net_ops ops = {transmit};

static void check_link(struct adapter *a) {
    dosxc_u32 status = dk_read32(a->regs, STATUS);
    int up = (status & STATUS_LU) != 0;
    if (up == a->link) return;
    a->link = up;
    unsigned speed = (status >> 6) & 3;
    dk->net_link(a->handle, up, speed == 0 ? 10 : speed == 1 ? 100 : 1000);
    dk->log("E1000: link %s", up ? "up" : "down");
}

static void poll(void *instance) {
    struct adapter *a = instance;
    for (unsigned budget = 0; budget < RX_COUNT; ++budget) {
        struct rx_descriptor *d = &a->rx[a->rx_next];
        if (!(d->status & STATUS_DD)) break;
        if (!d->errors && d->length >= 14)
            dk->net_receive(a->handle, a->rx_buffers + a->rx_next * BUFFER_SIZE, d->length);
        d->status = 0;
        dk_barrier();
        dk_write32(a->regs, RDT, a->rx_next);
        a->rx_next = (a->rx_next + 1) % RX_COUNT;
    }
    dosxc_u64 now = dk->nanoseconds();
    if (now >= a->next_link_check) {
        a->next_link_check = now + 500000000ull;
        check_link(a);
    }
}

static int read_mac(struct adapter *a, unsigned char mac[6]) {
    dosxc_u32 low = dk_read32(a->regs, RAL), high = dk_read32(a->regs, RAH);
    if (!(high & (1u << 31))) {
        /* Nothing loaded by the hardware: read the EEPROM ourselves. */
        for (unsigned word = 0; word < 3; ++word) {
            dk_write32(a->regs, EERD, word << 8 | 1);
            dosxc_u32 value = 0;
            int ready = 0;
            for (int i = 0; i < 1000 && !ready; ++i) {
                value = dk_read32(a->regs, EERD);
                ready = (value & 0x10) != 0;
                if (!ready) dk->sleep(1000);
            }
            if (!ready) return -1;
            mac[word * 2] = (unsigned char)(value >> 16);
            mac[word * 2 + 1] = (unsigned char)(value >> 24);
        }
        return 0;
    }
    for (int i = 0; i < 4; ++i) mac[i] = (unsigned char)(low >> (i * 8));
    mac[4] = (unsigned char)high;
    mac[5] = (unsigned char)(high >> 8);
    return 0;
}

static int load(const struct dk_api *api) { dk = api; return 0; }

static int probe(const struct dk_pci_device *device, void **instance) {
    int io = 0;
    dosxc_u64 bar = dk->pci_bar(device, 0, &io);
    if (!bar || io) { dk->log("E1000: register BAR is not usable"); return -1; }
    volatile void *regs = dk->mmio_map(bar, 0x20000);
    if (!regs) return -1;
    dk->pci_enable(device, 1, 0);
    struct adapter *a = dk->alloc(sizeof(*a));
    if (!a) return -1;
    a->regs = regs;
    a->device = *device;

    dk_write32(regs, IMC, 0xffffffffu);
    dk_write32(regs, RCTL, 0);
    dk_write32(regs, TCTL, 0);
    dk->sleep(1000000);
    dk_write32(regs, CTRL, dk_read32(regs, CTRL) | CTRL_RST);
    dk->sleep(10000000);
    dk_write32(regs, IMC, 0xffffffffu);
    (void)dk_read32(regs, ICR);

    unsigned char mac[6];
    if (read_mac(a, mac)) { dk->log("E1000: no MAC address"); dk->free(a); return -1; }

    dosxc_u32 control = dk_read32(regs, CTRL);
    control &= ~(CTRL_PHY_RST | (1u << 3) | (1u << 7) | (1u << 30));   /* LRST, ILOS, VME */
    dk_write32(regs, CTRL, control | CTRL_SLU | CTRL_ASDE);

    a->rx = dk->dma_alloc(sizeof(struct rx_descriptor) * RX_COUNT, 4096);
    a->tx = dk->dma_alloc(sizeof(struct tx_descriptor) * TX_COUNT, 4096);
    a->rx_buffers = dk->dma_alloc(RX_COUNT * BUFFER_SIZE, 4096);
    a->tx_buffers = dk->dma_alloc(TX_COUNT * BUFFER_SIZE, 4096);
    if (!a->rx || !a->tx || !a->rx_buffers || !a->tx_buffers) {
        dk->log("E1000: out of DMA memory");
        return -1;
    }
    for (unsigned i = 0; i < RX_COUNT; ++i) a->rx[i].address = dk->dma_address(a->rx_buffers + i * BUFFER_SIZE);

    for (unsigned i = 0; i < 128; ++i) dk_write32(regs, MTA + i * 4, 0);
    dk_write32(regs, RAL, (dosxc_u32)mac[0] | (dosxc_u32)mac[1] << 8 | (dosxc_u32)mac[2] << 16 | (dosxc_u32)mac[3] << 24);
    dk_write32(regs, RAH, (dosxc_u32)mac[4] | (dosxc_u32)mac[5] << 8 | 1u << 31);

    dosxc_u64 address = dk->dma_address(a->rx);
    dk_write32(regs, RDBAL, (dosxc_u32)address);
    dk_write32(regs, RDBAH, (dosxc_u32)(address >> 32));
    dk_write32(regs, RDLEN, sizeof(struct rx_descriptor) * RX_COUNT);
    dk_write32(regs, RDH, 0);
    dk_write32(regs, RDT, RX_COUNT - 1);
    dk_write32(regs, RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    address = dk->dma_address(a->tx);
    dk_write32(regs, TDBAL, (dosxc_u32)address);
    dk_write32(regs, TDBAH, (dosxc_u32)(address >> 32));
    dk_write32(regs, TDLEN, sizeof(struct tx_descriptor) * TX_COUNT);
    dk_write32(regs, TDH, 0);
    dk_write32(regs, TDT, 0);
    dk_write32(regs, TIPG, 10 | 8 << 10 | 6 << 20);
    dk_write32(regs, TCTL, TCTL_EN | TCTL_PSP | 0x10 << 4 | 0x40 << 12);

    char name[16];
    dk->format(name, sizeof(name), "E1000/%04x", device->id);
    a->handle = dk->net_register(&ops, a, mac, name);
    if (a->handle < 0) return -1;
    dk->log("E1000: %04x:%04x at %02x:%02x.%u, MAC %02x:%02x:%02x:%02x:%02x:%02x", device->vendor, device->id,
            device->bus, device->device, device->function, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    check_link(a);
    *instance = a;
    return 0;
}

static void remove_adapter(void *instance) {
    struct adapter *a = instance;
    dk_write32(a->regs, RCTL, 0);
    dk_write32(a->regs, TCTL, 0);
    dk_write32(a->regs, CTRL, dk_read32(a->regs, CTRL) | CTRL_RST);
    dk->sleep(5000000);
    dk->net_unregister(a->handle);
    dk->pci_write16(&a->device, 4, (unsigned short)(dk->pci_read16(&a->device, 4) & ~4u));
    dk->dma_free(a->rx);
    dk->dma_free(a->tx);
    dk->dma_free(a->rx_buffers);
    dk->dma_free(a->tx_buffers);
    dk->free(a);
}

DK_DRIVER = {DK_ABI_VERSION, sizeof(struct dk_driver), "Intel PRO/1000", "1.0",
             "Intel 8254x/82574L Gigabit Ethernet", ids, load, probe, poll, remove_adapter, 0};
