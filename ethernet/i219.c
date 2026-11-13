/*
 * DOS-XC /ethernet/i219.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include <dosxc/driver.h>

#define CTRL 0x0000
#define STATUS 0x0008
#define CTRL_EXT 0x0018
#define MDIC 0x0020
#define FEXTNVM3 0x003c
#define ICR 0x00c0
#define IMC 0x00d8
#define FEXTNVM7 0x00e4
#define RCTL 0x0100
#define TCTL 0x0400
#define TIPG 0x0410
#define EXTCNF_CTRL 0x0f00
#define IOSFPC 0x0f28
#define KABGTXD 0x3004
#define RDBAL 0x2800
#define RDBAH 0x2804
#define RDLEN 0x2808
#define RDH 0x2810
#define RDT 0x2818
#define RXDCTL 0x2828
#define TDBAL 0x3800
#define TDBAH 0x3804
#define TDLEN 0x3808
#define TDH 0x3810
#define TDT 0x3818
#define TXDCTL 0x3828
#define TARC0 0x3840
#define RFCTL 0x5008
#define MTA 0x5200
#define RAL 0x5400
#define RAH 0x5404
#define H2ME 0x5b50
#define FWSM 0x5b54

#define CTRL_GIO_MASTER_DISABLE (1u << 2)
#define CTRL_SLU (1u << 6)
#define CTRL_FRCSPD (1u << 11)
#define CTRL_FRCDPX (1u << 12)
#define CTRL_LANPHYPC_OVERRIDE (1u << 16)
#define CTRL_LANPHYPC_VALUE (1u << 17)
#define CTRL_RST (1u << 26)
#define CTRL_PHY_RST (1u << 31)
#define STATUS_LU (1u << 1)
#define STATUS_LAN_INIT_DONE (1u << 9)
#define STATUS_PHYRA (1u << 10)
#define STATUS_GIO_MASTER (1u << 19)
#define CTRL_EXT_LPCD (1u << 2)
#define CTRL_EXT_FORCE_SMBUS (1u << 11)
#define EXTCNF_SWFLAG (1u << 5)
#define FWSM_RSPCIPHY (1u << 6)
#define FWSM_ULP_CFG_DONE (1u << 10)
#define FWSM_FW_VALID (1u << 15)
#define H2ME_ULP (1u << 11)
#define H2ME_ENFORCE (1u << 12)
#define MDIC_READY (1u << 28)
#define MDIC_ERROR (1u << 30)

#define RCTL_EN 2u
#define RCTL_MPE (1u << 4)
#define RCTL_BAM (1u << 15)
#define RCTL_SECRC (1u << 26)
#define TCTL_EN 2u
#define TCTL_PSP 8u
#define TCTL_RTLC (1u << 24)
#define CMD_EOP 1
#define CMD_IFCS 2
#define CMD_RS 8
#define STATUS_DD 1

/* PHY registers: page, register. */
#define CV_SMB_CTRL 769, 23
#define HV_PM_CTRL 770, 17
#define ULP_CONFIG1 779, 16

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
    {0x8086, 0x153a}, {0x8086, 0x153b},                                         /* I217 (Lynx Point) */
    {0x8086, 0x155a}, {0x8086, 0x1559}, {0x8086, 0x15a0}, {0x8086, 0x15a1},
    {0x8086, 0x15a2}, {0x8086, 0x15a3},                                         /* I218 */
    {0x8086, 0x156f}, {0x8086, 0x1570}, {0x8086, 0x15b7}, {0x8086, 0x15b8},
    {0x8086, 0x15b9}, {0x8086, 0x15d7}, {0x8086, 0x15d8}, {0x8086, 0x15e3},
    {0x8086, 0x15d6},                                                           /* I219, Sunrise/Lewisburg/Union Point */
    {0x8086, 0x15bd}, {0x8086, 0x15be}, {0x8086, 0x15bb}, {0x8086, 0x15bc},     /* Cannon Lake (ThinkStation P330: 15bb) */
    {0x8086, 0x15df}, {0x8086, 0x15e0}, {0x8086, 0x15e1}, {0x8086, 0x15e2},     /* Ice Lake */
    {0x8086, 0x0d4e}, {0x8086, 0x0d4f}, {0x8086, 0x0d4c}, {0x8086, 0x0d4d},
    {0x8086, 0x0d53}, {0x8086, 0x0d55},                                         /* Comet Lake */
    {0x8086, 0x15fb}, {0x8086, 0x15fc}, {0x8086, 0x15f9}, {0x8086, 0x15fa},
    {0x8086, 0x15f4}, {0x8086, 0x15f5},                                         /* Tiger Lake */
    {0x8086, 0x1a1e}, {0x8086, 0x1a1f}, {0x8086, 0x1a1c}, {0x8086, 0x1a1d},     /* Alder Lake */
    {0x8086, 0x550a}, {0x8086, 0x550b}, {0x8086, 0x550c}, {0x8086, 0x550d},     /* Meteor Lake */
    {0, 0},
};

static int is_i219(unsigned short id) { return id != 0x153a && id != 0x153b && (id < 0x1559 || id > 0x155a) && (id < 0x15a0 || id > 0x15a3); }

static int wait32(struct adapter *a, unsigned reg, dosxc_u32 mask, int set, unsigned milliseconds) {
    dosxc_u64 deadline = dk->nanoseconds() + (dosxc_u64)milliseconds * 1000000ull;
    for (;;) {
        if (!!(dk_read32(a->regs, reg) & mask) == set) return 0;
        if (dk->nanoseconds() > deadline) return -1;
        dk->sleep(20000);
    }
}

/* The software flag shares the PHY and NVM with the management engine. */
static void acquire(struct adapter *a) {
    wait32(a, EXTCNF_CTRL, EXTCNF_SWFLAG, 0, 1000);
    dk_write32(a->regs, EXTCNF_CTRL, dk_read32(a->regs, EXTCNF_CTRL) | EXTCNF_SWFLAG);
    if (wait32(a, EXTCNF_CTRL, EXTCNF_SWFLAG, 1, 1000)) dk->log("I219: the firmware kept the PHY flag; going on without it");
}

static void release(struct adapter *a) {
    dk_write32(a->regs, EXTCNF_CTRL, dk_read32(a->regs, EXTCNF_CTRL) & ~EXTCNF_SWFLAG);
}

static int mdic(struct adapter *a, unsigned phy, unsigned reg, unsigned op, unsigned short *value) {
    dk_write32(a->regs, MDIC, (op == 1 ? *value : 0) | (reg & 31u) << 16 | phy << 21 | op << 26);
    dosxc_u32 v = 0;
    for (int i = 0; i < 1920; ++i) {
        dk->sleep(50000);
        v = dk_read32(a->regs, MDIC);
        if (v & MDIC_READY) break;
    }
    dk->sleep(100000);
    if (!(v & MDIC_READY) || (v & MDIC_ERROR)) return -1;
    if (op == 2) *value = (unsigned short)v;
    return 0;
}

/* PHY registers of the HV family: pages 768 and up sit at address 1 and
   need the page select for registers above 15. */
static int phy_access(struct adapter *a, unsigned page, unsigned reg, unsigned short *value, int write) {
    unsigned phy = page >= 768 ? 1 : 2;
    if (page == 768) page = 0;
    if (reg > 15) {
        unsigned short select = (unsigned short)(page << 5);
        if (mdic(a, phy, 31, 1, &select)) return -1;
    }
    return mdic(a, phy, reg, write ? 1 : 2, value);
}
static int phy_read(struct adapter *a, unsigned page, unsigned reg, unsigned short *value) { return phy_access(a, page, reg, value, 0); }
static int phy_write(struct adapter *a, unsigned page, unsigned reg, unsigned short value) { return phy_access(a, page, reg, &value, 1); }

static int phy_accessible(struct adapter *a) {
    for (int i = 0; i < 2; ++i) {
        unsigned short id1 = 0, id2 = 0;
        if (!phy_read(a, 0, 2, &id1) && !phy_read(a, 0, 3, &id2) && id1 && id1 != 0xffff && id2 != 0xffff) return 1;
    }
    return 0;
}

static int phy_reset_blocked(struct adapter *a) { return !(dk_read32(a->regs, FWSM) & FWSM_RSPCIPHY); }

static void force_smbus(struct adapter *a, int on) {
    dosxc_u32 v = dk_read32(a->regs, CTRL_EXT);
    dk_write32(a->regs, CTRL_EXT, on ? v | CTRL_EXT_FORCE_SMBUS : v & ~CTRL_EXT_FORCE_SMBUS);
}

/* Power-cycle the PHY through LANPHYPC: it leaves ULP and SMBus mode. */
static void toggle_lanphypc(struct adapter *a) {
    dosxc_u32 v = dk_read32(a->regs, FEXTNVM3);
    dk_write32(a->regs, FEXTNVM3, (v & ~0x0c000000u) | 0x08000000u);
    v = dk_read32(a->regs, CTRL);
    v |= CTRL_LANPHYPC_OVERRIDE;
    v &= ~CTRL_LANPHYPC_VALUE;
    dk_write32(a->regs, CTRL, v);
    (void)dk_read32(a->regs, STATUS);
    dk->sleep(20000);
    dk_write32(a->regs, CTRL, v & ~CTRL_LANPHYPC_OVERRIDE);
    (void)dk_read32(a->regs, STATUS);
    for (int i = 0; i < 20 && !(dk_read32(a->regs, CTRL_EXT) & CTRL_EXT_LPCD); ++i) dk->sleep(5000000);
    dk->sleep(30000000);
}

/* Firmware or an earlier system may have left the PHY in ultra low power. */
static void leave_ulp(struct adapter *a) {
    if (dk_read32(a->regs, FWSM) & FWSM_FW_VALID) {
        dosxc_u32 v = dk_read32(a->regs, H2ME);
        dk_write32(a->regs, H2ME, (v & ~H2ME_ULP) | H2ME_ENFORCE);
        for (int i = 0; i < 30 && (dk_read32(a->regs, FWSM) & FWSM_ULP_CFG_DONE); ++i) dk->sleep(10000000);
        dk_write32(a->regs, H2ME, dk_read32(a->regs, H2ME) & ~H2ME_ENFORCE);
        return;
    }
    toggle_lanphypc(a);
    unsigned short reg;
    if (phy_read(a, CV_SMB_CTRL, &reg)) {
        force_smbus(a, 1);
        dk->sleep(50000000);
        if (phy_read(a, CV_SMB_CTRL, &reg)) return;
    }
    phy_write(a, CV_SMB_CTRL, (unsigned short)(reg & ~1u));
    force_smbus(a, 0);
    if (!phy_read(a, HV_PM_CTRL, &reg)) phy_write(a, HV_PM_CTRL, (unsigned short)(reg | 0x4000u));
    if (!phy_read(a, ULP_CONFIG1, &reg)) {
        reg &= (unsigned short)~0x1d74u;
        phy_write(a, ULP_CONFIG1, reg);
        phy_write(a, ULP_CONFIG1, (unsigned short)(reg | 1u));
    }
    dk_write32(a->regs, FEXTNVM7, dk_read32(a->regs, FEXTNVM7) & ~0x20u);
}

/* As Linux's e1000_init_phy_workarounds_pchlan: make the PHY answer on MDIO. */
static int wake_phy(struct adapter *a) {
    acquire(a);
    leave_ulp(a);
    int ok = phy_accessible(a);
    if (!ok) {
        force_smbus(a, 1);
        dk->sleep(50000000);
        ok = phy_accessible(a);
    }
    if (!ok && !phy_reset_blocked(a)) {
        toggle_lanphypc(a);
        ok = phy_accessible(a);
        if (!ok) {
            force_smbus(a, 0);
            ok = phy_accessible(a);
        }
    }
    if (ok && !(dk_read32(a->regs, FWSM) & FWSM_FW_VALID)) {
        unsigned short reg;
        if (!phy_read(a, CV_SMB_CTRL, &reg)) phy_write(a, CV_SMB_CTRL, (unsigned short)(reg & ~1u));
        force_smbus(a, 0);
    }
    release(a);
    return ok ? 0 : -1;
}

static void setup_rings(struct adapter *a) {
    dosxc_u64 address = dk->dma_address(a->rx);
    dk_write32(a->regs, RDBAL, (dosxc_u32)address);
    dk_write32(a->regs, RDBAH, (dosxc_u32)(address >> 32));
    dk_write32(a->regs, RDLEN, sizeof(struct rx_descriptor) * RX_COUNT);
    dk_write32(a->regs, RDH, 0);
    dk_write32(a->regs, RDT, 0);
    address = dk->dma_address(a->tx);
    dk_write32(a->regs, TDBAL, (dosxc_u32)address);
    dk_write32(a->regs, TDBAH, (dosxc_u32)(address >> 32));
    dk_write32(a->regs, TDLEN, sizeof(struct tx_descriptor) * TX_COUNT);
    dk_write32(a->regs, TDH, 0);
    dk_write32(a->regs, TDT, 0);
    a->tx_next = a->tx_clean = 0;
}

/* Skylake and later hang in reset while descriptor fetches are pending (left by PXE firmware). */
static void flush_rings(struct adapter *a) {
    if (!(dk->pci_read16(&a->device, 0xe4) & 0x100) || !dk_read32(a->regs, TDLEN)) return;
    setup_rings(a);
    dk_write32(a->regs, TCTL, dk_read32(a->regs, TCTL) | TCTL_EN);
    a->tx[0].address = dk->dma_address(a->tx_buffers);
    a->tx[0].length = 512;
    a->tx[0].cmd = CMD_IFCS;
    dk_barrier();
    dk_write32(a->regs, TDT, 1);
    dk->sleep(250000);
    a->tx[0] = (struct tx_descriptor){0};
    if (!(dk->pci_read16(&a->device, 0xe4) & 0x100)) return;
    dk_write32(a->regs, RCTL, dk_read32(a->regs, RCTL) & ~RCTL_EN);
    (void)dk_read32(a->regs, STATUS);
    dk->sleep(150000);
    dosxc_u32 rxdctl = dk_read32(a->regs, RXDCTL);
    dk_write32(a->regs, RXDCTL, (rxdctl & 0xffffc000u) | 0x1fu | 1u << 8 | 1u << 16 | 1u << 24);
    dk_write32(a->regs, RCTL, dk_read32(a->regs, RCTL) | RCTL_EN);
    (void)dk_read32(a->regs, STATUS);
    dk->sleep(150000);
    dk_write32(a->regs, RCTL, dk_read32(a->regs, RCTL) & ~RCTL_EN);
}

static int reset(struct adapter *a) {
    dk_write32(a->regs, CTRL, dk_read32(a->regs, CTRL) | CTRL_GIO_MASTER_DISABLE);
    wait32(a, STATUS, STATUS_GIO_MASTER, 0, 80);
    dk_write32(a->regs, IMC, 0xffffffffu);
    dk_write32(a->regs, RCTL, 0);
    dk_write32(a->regs, TCTL, TCTL_PSP);
    (void)dk_read32(a->regs, STATUS);
    dk->sleep(10000000);
    dosxc_u32 control = dk_read32(a->regs, CTRL);
    int phy_too = !phy_reset_blocked(a);
    if (phy_too) control |= CTRL_PHY_RST;
    acquire(a);
    dk_write32(a->regs, CTRL, control | CTRL_RST);
    dk->sleep(20000000);
    if (wait32(a, CTRL, CTRL_RST, 0, 100)) return -1;
    if (phy_too) {
        dk->sleep(10000000);
        if (wait32(a, STATUS, STATUS_LAN_INIT_DONE, 1, 150)) dk->log("I219: no LAN init done after reset");
        dosxc_u32 status = dk_read32(a->regs, STATUS);
        dk_write32(a->regs, STATUS, status & ~(STATUS_LAN_INIT_DONE | STATUS_PHYRA));
    }
    release(a);
    dk_write32(a->regs, IMC, 0xffffffffu);
    (void)dk_read32(a->regs, ICR);
    dk_write32(a->regs, KABGTXD, dk_read32(a->regs, KABGTXD) | 0x00050000u);
    return 0;
}

static void start_link(struct adapter *a) {
    dosxc_u32 control = dk_read32(a->regs, CTRL);
    dk_write32(a->regs, CTRL, (control & ~(CTRL_FRCSPD | CTRL_FRCDPX)) | CTRL_SLU);
    acquire(a);
    phy_write(a, 0, 4, 0x0de1);           /* 10/100, both duplexes, pause */
    phy_write(a, 0, 9, 0x0200);           /* 1000 full duplex */
    phy_write(a, 0, 0, 0x1340);           /* autonegotiate, restart */
    release(a);
}

static int transmit(void *instance, const void *frame, unsigned length) {
    struct adapter *a = instance;
    if (length > BUFFER_SIZE) return -1;
    while (a->tx_clean != a->tx_next && (a->tx[a->tx_clean].status & STATUS_DD))
        a->tx_clean = (a->tx_clean + 1) % TX_COUNT;
    unsigned next = (a->tx_next + 1) % TX_COUNT;
    if (next == a->tx_clean) return -1;
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
    dk->log("I219: link %s", up ? "up" : "down");
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

static void free_adapter(struct adapter *a) {
    if (a->rx) dk->dma_free(a->rx);
    if (a->tx) dk->dma_free(a->tx);
    if (a->rx_buffers) dk->dma_free(a->rx_buffers);
    if (a->tx_buffers) dk->dma_free(a->tx_buffers);
    dk->free(a);
}

static int load(const struct dk_api *api) { dk = api; return 0; }

static int probe(const struct dk_pci_device *device, void **instance) {
    int io = 0;
    dosxc_u64 bar = dk->pci_bar(device, 0, &io);
    if (!bar || io) { dk->log("I219: register BAR is not usable"); return -1; }
    volatile void *regs = dk->mmio_map(bar, 0x20000);
    if (!regs) { dk->log("I219: registers at %lx cannot be mapped", (unsigned long)bar); return -1; }
    dk->pci_enable(device, 1, 0);
    struct adapter *a = dk->alloc(sizeof(*a));
    if (!a) return -1;
    a->regs = regs;
    a->device = *device;
    a->rx = dk->dma_alloc(sizeof(struct rx_descriptor) * RX_COUNT, 4096);
    a->tx = dk->dma_alloc(sizeof(struct tx_descriptor) * TX_COUNT, 4096);
    a->rx_buffers = dk->dma_alloc(RX_COUNT * BUFFER_SIZE, 4096);
    a->tx_buffers = dk->dma_alloc(TX_COUNT * BUFFER_SIZE, 4096);
    if (!a->rx || !a->tx || !a->rx_buffers || !a->tx_buffers) {
        dk->log("I219: out of DMA memory");
        free_adapter(a);
        return -1;
    }

    if (wake_phy(a)) dk->log("I219: the PHY does not answer; trying a reset anyway");
    if (is_i219(device->id)) flush_rings(a);
    if (reset(a)) { dk->log("I219: the controller did not come out of reset"); free_adapter(a); return -1; }

    dosxc_u32 high = dk_read32(regs, RAH), low = dk_read32(regs, RAL);
    if (!(high & (1u << 31))) { dk->log("I219: no MAC address (NVM not loaded)"); free_adapter(a); return -1; }
    unsigned char mac[6];
    for (int i = 0; i < 4; ++i) mac[i] = (unsigned char)(low >> (i * 8));
    mac[4] = (unsigned char)high;
    mac[5] = (unsigned char)(high >> 8);

    dk_write32(regs, TXDCTL, (dk_read32(regs, TXDCTL) & ~0x003f003fu) | 0x0101001fu | 1u << 22);
    dk_write32(regs, TARC0, dk_read32(regs, TARC0) | 1u << 23 | 1u << 24 | 1u << 26 | 1u << 27);
    dk_write32(regs, CTRL_EXT, dk_read32(regs, CTRL_EXT) | 1u << 22);
    dk_write32(regs, STATUS, dk_read32(regs, STATUS) & ~(1u << 31));
    dk_write32(regs, RFCTL, dk_read32(regs, RFCTL) | 0x40u | 0x80u | 0x10000u);
    if (is_i219(device->id)) {
        dk_write32(regs, IOSFPC, dk_read32(regs, IOSFPC) | 0x00010000u);
        dk_write32(regs, TARC0, (dk_read32(regs, TARC0) & ~(3u << 28)) | 1u << 29);
    }

    for (unsigned i = 0; i < RX_COUNT; ++i) a->rx[i].address = dk->dma_address(a->rx_buffers + i * BUFFER_SIZE);
    for (unsigned i = 0; i < 32; ++i) dk_write32(regs, MTA + i * 4, 0);
    setup_rings(a);
    dk_write32(regs, RDT, RX_COUNT - 1);
    dk_write32(regs, RCTL, RCTL_EN | RCTL_MPE | RCTL_BAM | RCTL_SECRC);
    dk_write32(regs, TIPG, 8 | 8 << 10 | 6 << 20);
    dk_write32(regs, TCTL, TCTL_EN | TCTL_PSP | TCTL_RTLC | 0x0f << 4 | 0x3f << 12);
    start_link(a);

    char name[16];
    dk->format(name, sizeof(name), "I219/%04x", device->id);
    a->handle = dk->net_register(&ops, a, mac, name);
    if (a->handle < 0) { free_adapter(a); return -1; }
    dk->log("I219: %04x:%04x at %02x:%02x.%u, MAC %02x:%02x:%02x:%02x:%02x:%02x%s", device->vendor, device->id,
            device->bus, device->device, device->function, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
            dk_read32(regs, FWSM) & FWSM_FW_VALID ? ", management firmware present" : "");
    check_link(a);
    *instance = a;
    return 0;
}

static void remove_adapter(void *instance) {
    struct adapter *a = instance;
    dk_write32(a->regs, RCTL, 0);
    dk_write32(a->regs, TCTL, TCTL_PSP);
    (void)dk_read32(a->regs, STATUS);
    dk->sleep(2000000);
    dk->net_unregister(a->handle);
    dk->pci_write16(&a->device, 4, (unsigned short)(dk->pci_read16(&a->device, 4) & ~4u));
    free_adapter(a);
}

DK_DRIVER = {DK_ABI_VERSION, sizeof(struct dk_driver), "Intel I219", "1.0",
             "Intel I217/I218/I219 chipset Ethernet", ids, load, probe, poll, remove_adapter, 0};
