/*
 * DOS-XC /ethernet/tg3.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include <dosxc/driver.h>

#define PCI_MISC_CTL 0x68
#define PCI_DMA_RW 0x6c
#define PCI_PCISTATE 0x70
#define PCI_MEMWIN_BASE 0x7c
#define PCI_MEMWIN_DATA 0x84
#define PCI_PRODID_ASICREV 0xbc
#define MISC_INIT 0x8bu                  /* CLEAR_INTA | MASK | WORDSWAP | INDIRECT */
#define MISC_RESET 0x9au                 /* MASK | WORDSWAP | PCISTATE_RW | INDIRECT */
#define PCISTATE_ROM 0x60u               /* ROM_ENABLE | ROM_RETRY_ENABLE */
#define PCISTATE_APE 0x70000u

#define MAC_MODE 0x0400
#define MAC_STS 0x0404
#define MAC_ADDR1_LO 0x0410
#define MAC_ADDR1_HI 0x0414
#define TX_RANDOM_BACKOFF 0x0438
#define RX_MTU 0x043c
#define MI_COMM 0x044c
#define MI_STS 0x0450
#define MI_MODE 0x0454
#define TX_MODE 0x045c
#define TX_LENGTHS 0x0464
#define RX_MODE 0x0468
#define MAR0 0x0470
#define RX_RULES_CFG 0x0500
#define MAX_RX_FRAME_LOWAT 0x0504
#define SDI_MODE 0x0c00
#define SDI_STATS_CTL 0x0c08
#define SDI_STATS_ENABLE 0x0c0c
#define SDC_MODE 0x1000
#define SRS_MODE 0x1400
#define SBDI_MODE 0x1800
#define SBDC_MODE 0x1c00
#define RXLP_MODE 0x2000
#define RXLP_CFG 0x2010
#define RXLP_STATS_CTL 0x2014
#define RXLP_STATS_ENABLE 0x2018
#define RDBDI_MODE 0x2400
#define RX_STD_RCB_HI 0x2450
#define RX_STD_RCB_LO 0x2454
#define RX_STD_RCB_FLAGS 0x2458
#define RX_STD_RCB_NIC 0x245c
#define RDC_MODE 0x2800
#define RBDI_MODE 0x2c00
#define RBDI_STD_THRESH 0x2c18
#define RBDC_MODE 0x3000
#define HCC_MODE 0x3c00
#define HCC_RX_TICKS 0x3c08
#define HCC_TX_TICKS 0x3c0c
#define HCC_RX_BDS 0x3c10
#define HCC_TX_BDS 0x3c14
#define HCC_RX_BDS_INT 0x3c20
#define HCC_TX_BDS_INT 0x3c24
#define HCC_STATUS_HI 0x3c38
#define HCC_STATUS_LO 0x3c3c
#define MARB_MODE 0x4000
#define BMAN_MODE 0x4400
#define BMAN_READDMA_LOWAT 0x4410
#define BMAN_MACRX_LOWAT 0x4414
#define BMAN_HIWAT 0x4418
#define BMAN_DESC_LOWAT 0x4434
#define BMAN_DESC_HIWAT 0x4438
#define RDMA_MODE 0x4800
#define RDMA_RSRVCTRL 0x4900
#define WDMA_MODE 0x4c00
#define FTQ_RESET 0x5c00
#define MODE_CTL 0x6800
#define MISC_CFG 0x6804
#define FASTBOOT_PC 0x6894
#define NVRAM_SWARB 0x7020
#define MEMWIN 0x8000

#define MBX_IRQ0 0x0204
#define MBX_RX_STD_PROD 0x026c
#define MBX_RX_CONS0 0x0284
#define MBX_TX_HOST_PROD0 0x0304
#define MBX_TX_NIC_PROD0 0x0384

#define HCC_ENABLE 2u
#define HCC_COAL_NOW 8u
#define HCC_32BYTE 0x100u
#define MI_BUSY 0x20000000u
#define MI_READ 0x08000000u
#define MI_WRITE 0x04000000u
#define MI_FAIL 0x10000000u
#define MI_MODE_500KHZ 0x8000u
#define MAC_HALF 2u
#define MAC_PORT_MASK 0xcu
#define MAC_PORT_MII 4u
#define MAC_PORT_GMII 8u
#define MAC_ENABLE 0x00e0d800u           /* TXDMA RXDMA FRMHDR, statistics */
#define FW_MAGIC 0x4b657654u
#define MODE_SWAP 0x34u                  /* WORDSWAP_NONFRAME, BYTESWAP_DATA, WORDSWAP_DATA */
#define MODE_FLAGS 0x04120000u           /* MAC_ATTN, HOST_SEND_BDS, TX_NO_PHDR_CSUM */
#define BD_END 4u
#define BD_ERROR 0x400u
#define RCB_DISABLED 2u

#define ASIC_5761 0x5761
#define ASIC_5784 0x5784

#define PHY 1
#define PHY_BMCR 0
#define PHY_BMSR 1
#define PHY_ANAR 4
#define PHY_1000CTL 9
#define PHY_AUX_STATUS 0x19

#define RING 512u                        /* standard receive, return and send rings */
#define TX_BUFFERS 64u
#define BUFFER_SIZE 2048u

struct host_address { dosxc_u32 high, low; };
struct rx_bd { struct host_address address; unsigned short length, index, flags, type, tcp_udp_csum, ip_csum, vlan, error; dosxc_u32 reserved, opaque; };
struct tx_bd { struct host_address address; unsigned short flags, length, vlan, mss; };
struct status_block {
    dosxc_u32 status, tag;
    unsigned short jumbo_cons, std_cons, mini_cons, reserved;
    struct { unsigned short tx_cons, rx_prod; } index[16];
};

struct adapter {
    volatile void *regs;
    struct dk_pci_device device;
    unsigned asic;
    int pcie_cap;
    struct rx_bd *std_ring, *return_ring;
    struct tx_bd *tx_ring;
    volatile struct status_block *status;
    unsigned char *rx_buffers, *tx_buffers;
    unsigned std_prod, return_cons, tx_prod, tx_cons;
    dosxc_u32 mac_mode;
    int handle, link;
    dosxc_u64 next_link_check, next_coalesce;
    unsigned char mac[6];
};

static const struct dk_api *dk;
static const struct dk_pci_id ids[] = {
    {0x14e4, 0x1684}, /* BCM5764M: HP Z400 onboard */
    {0x14e4, 0x1680}, /* BCM5761E */
    {0x14e4, 0x1681}, /* BCM5761 */
    {0x14e4, 0x1698}, /* BCM5784M */
    {0, 0},
};

static dosxc_u32 rd(struct adapter *a, unsigned offset) { return dk_read32(a->regs, offset); }
static void wr(struct adapter *a, unsigned offset, dosxc_u32 value) { dk_write32(a->regs, offset, value); }
static void delay_us(unsigned us) { dk->sleep((dosxc_u64)us * 1000); }

static void mem_write(struct adapter *a, unsigned offset, dosxc_u32 value) {
    dk->pci_write32(&a->device, PCI_MEMWIN_BASE, offset);
    dk->pci_write32(&a->device, PCI_MEMWIN_DATA, value);
    dk->pci_write32(&a->device, PCI_MEMWIN_BASE, 0);
}
static dosxc_u32 mem_read(struct adapter *a, unsigned offset) {
    dk->pci_write32(&a->device, PCI_MEMWIN_BASE, offset);
    dosxc_u32 value = dk->pci_read32(&a->device, PCI_MEMWIN_DATA);
    dk->pci_write32(&a->device, PCI_MEMWIN_BASE, 0);
    return value;
}

static int wait_bits(struct adapter *a, unsigned offset, dosxc_u32 mask, dosxc_u32 value, const char *what) {
    for (unsigned i = 0; i < 100000; ++i) {
        if ((rd(a, offset) & mask) == value) return 0;
        delay_us(10);
    }
    dk->log("TG3: timed out waiting for %s (register %x = %x)", what, offset, rd(a, offset));
    return -1;
}

static int phy_read(struct adapter *a, unsigned reg, unsigned *out) {
    wr(a, MI_COMM, MI_READ | MI_BUSY | PHY << 21 | reg << 16);
    for (unsigned i = 0; i < 5000; ++i) {
        delay_us(10);
        dosxc_u32 v = rd(a, MI_COMM);
        if (!(v & MI_BUSY)) {
            delay_us(5);
            v = rd(a, MI_COMM);
            if (v & MI_FAIL) return -1;
            *out = v & 0xffff;
            return 0;
        }
    }
    return -1;
}

static int phy_write(struct adapter *a, unsigned reg, unsigned value) {
    wr(a, MI_COMM, MI_WRITE | MI_BUSY | PHY << 21 | reg << 16 | value);
    for (unsigned i = 0; i < 5000; ++i) {
        delay_us(10);
        if (!(rd(a, MI_COMM) & MI_BUSY)) { delay_us(5); return 0; }
    }
    dk->log("TG3: PHY write timed out (register %u)", reg);
    return -1;
}

static int find_pcie(const struct dk_pci_device *device) {
    if (!(dk->pci_read16(device, 6) & 0x10)) return 0;
    unsigned at = dk->pci_read8(device, 0x34) & 0xfc;
    for (int hops = 0; at && hops < 32; ++hops, at = dk->pci_read8(device, (unsigned char)(at + 1)) & 0xfc)
        if (dk->pci_read8(device, (unsigned char)at) == 0x10) return (int)at;
    return 0;
}

/* The global reset.  The firmware handshake tells us when it is safe. */
static int chip_reset(struct adapter *a) {
    dosxc_u32 mac_mode = rd(a, MAC_MODE) & (MAC_HALF | MAC_PORT_MASK);
    wr(a, NVRAM_SWARB, 2);
    for (unsigned i = 0; i < 8000 && !(rd(a, NVRAM_SWARB) & 0x200); ++i) delay_us(20);
    dosxc_u32 cachesize = dk->pci_read32(&a->device, 0x0c), command = dk->pci_read32(&a->device, 0x04);
    dk->pci_write32(&a->device, PCI_MISC_CTL, MISC_RESET);
    wr(a, FASTBOOT_PC, 0);
    mem_write(a, 0xb50, FW_MAGIC);
    dosxc_u32 reset = 1 | 0x41 << 1 | 1u << 29;   /* core clocks, 66 MHz timer, keep the PCIe link */
    if (rd(a, 0x7e2c) == 0x60) wr(a, 0x7e2c, 0x20);    /* PCIe 1.0 */
    wr(a, MISC_CFG, 1u << 29);
    wr(a, MISC_CFG, reset);
    delay_us(100000);
    if (a->pcie_cap) {
        unsigned char control = (unsigned char)(a->pcie_cap + 8);
        unsigned short devctl = dk->pci_read16(&a->device, control);
        dk->pci_write16(&a->device, control, (unsigned short)(devctl & ~0x810u));
        dk->pci_write16(&a->device, (unsigned char)(control + 2), 0x000f);         /* clear error status */
    }
    dk->pci_write32(&a->device, PCI_MISC_CTL, MISC_RESET);
    dk->pci_write32(&a->device, PCI_PCISTATE, PCISTATE_ROM);
    dk->pci_write32(&a->device, 0x0c, cachesize);
    dk->pci_write32(&a->device, 0x04, command);
    wr(a, MARB_MODE, 2);
    wr(a, MODE_CTL, MODE_SWAP);
    wr(a, MAC_MODE, (rd(a, MAC_MODE) & ~(MAC_HALF | MAC_PORT_MASK)) | mac_mode);
    delay_us(40);
    int acknowledged = 0;
    for (unsigned i = 0; i < 100000 && !acknowledged; ++i) {
        delay_us(10);
        acknowledged = mem_read(a, 0xb50) == ~FW_MAGIC;
    }
    if (!acknowledged) dk->log("TG3: firmware did not answer the reset (found %x)", mem_read(a, 0xb50));
    wr(a, 0x7c00, rd(a, 0x7c00) | 1u << 25);  /* data FIFO protection */
    return 0;
}

static int read_mac(struct adapter *a) {
    dosxc_u32 high = mem_read(a, 0xc14), low = mem_read(a, 0xc18);
    if (high >> 16 == 0x484b) {
        a->mac[0] = (unsigned char)(high >> 8);
        a->mac[1] = (unsigned char)high;
        a->mac[2] = (unsigned char)(low >> 24);
        a->mac[3] = (unsigned char)(low >> 16);
        a->mac[4] = (unsigned char)(low >> 8);
        a->mac[5] = (unsigned char)low;
        return 0;
    }
    high = rd(a, MAC_ADDR1_LO);
    low = rd(a, MAC_ADDR1_HI);
    if (!high && !low) return -1;
    a->mac[0] = (unsigned char)(high >> 8);
    a->mac[1] = (unsigned char)high;
    a->mac[2] = (unsigned char)(low >> 24);
    a->mac[3] = (unsigned char)(low >> 16);
    a->mac[4] = (unsigned char)(low >> 8);
    a->mac[5] = (unsigned char)low;
    return 0;
}

/* A control block in the memory window: host address, maxlen/flags, NIC address. */
static void rcb(struct adapter *a, unsigned offset, dosxc_u64 host, dosxc_u32 flags, dosxc_u32 nic) {
    wr(a, MEMWIN + offset, (dosxc_u32)(host >> 32));
    wr(a, MEMWIN + offset + 4, (dosxc_u32)host);
    wr(a, MEMWIN + offset + 8, flags);
    wr(a, MEMWIN + offset + 12, nic);
}

static int chip_init(struct adapter *a) {
    dk->pci_write32(&a->device, PCI_MISC_CTL, MISC_INIT);
    wr(a, PCI_MEMWIN_BASE, 0);
    for (unsigned i = 0x300; i < 0xb00; i += 4) wr(a, MEMWIN + i, 0);    /* statistics and */
    for (unsigned i = 0xb00; i < 0xb50; i += 4) wr(a, MEMWIN + i, 0);    /* status blocks */

    dosxc_u32 dma = 6u << 24 | 7u << 28;
    unsigned payload = 128;
    if (a->pcie_cap) payload <<= (dk->pci_read16(&a->device, (unsigned char)(a->pcie_cap + 8)) >> 5) & 7;
    dma |= (payload >= 256 ? 7u : 3u) << 19;
    dk->pci_write32(&a->device, PCI_DMA_RW, dma);
    wr(a, MODE_CTL, MODE_SWAP | MODE_FLAGS);
    dk->pci_write16(&a->device, 4, (unsigned short)(dk->pci_read16(&a->device, 4) & ~0x10u));
    wr(a, MISC_CFG, 0x41 << 1);

    wr(a, BMAN_READDMA_LOWAT, 0);
    wr(a, BMAN_MACRX_LOWAT, 0x10);
    wr(a, BMAN_HIWAT, 0x60);
    wr(a, BMAN_DESC_LOWAT, 5);
    wr(a, BMAN_DESC_HIWAT, 10);
    wr(a, BMAN_MODE, 2 | 0x10);
    if (wait_bits(a, BMAN_MODE, 2, 2, "the buffer manager")) return -1;
    wr(a, FTQ_RESET, 0xffffffffu);
    wr(a, FTQ_RESET, 0);
    if (wait_bits(a, FTQ_RESET, 0xffffffffu, 0, "the flow-through queues")) return -1;

    dosxc_u64 address = dk->dma_address(a->std_ring);
    wr(a, RX_STD_RCB_HI, (dosxc_u32)(address >> 32));
    wr(a, RX_STD_RCB_LO, (dosxc_u32)address);
    wr(a, RX_STD_RCB_FLAGS, RING << 16);
    wr(a, RX_STD_RCB_NIC, 0x6000);
    wr(a, MBX_RX_STD_PROD, 0);
    wr(a, RBDI_STD_THRESH, 8);

    wr(a, PCI_MEMWIN_BASE, 0);
    rcb(a, 0x100, dk->dma_address(a->tx_ring), RING << 16, 0x4000);
    rcb(a, 0x200, dk->dma_address(a->return_ring), RING << 16, 0);
    wr(a, MBX_RX_CONS0, 0);

    unsigned sum = 0;
    for (int i = 0; i < 6; ++i) sum += a->mac[i];
    wr(a, TX_RANDOM_BACKOFF, sum & 0x3ff);
    wr(a, TX_LENGTHS, 0x2620);
    wr(a, RX_RULES_CFG, 8);
    wr(a, RXLP_CFG, 0x181);
    wr(a, RXLP_STATS_ENABLE, 0x7fffff);
    wr(a, RXLP_STATS_CTL, 1);

    wr(a, HCC_MODE, 0);
    if (wait_bits(a, HCC_MODE, HCC_ENABLE, 0, "the coalescing engine to stop")) return -1;
    wr(a, HCC_RX_TICKS, 20);
    wr(a, HCC_TX_TICKS, 20);
    wr(a, HCC_RX_BDS, 1);
    wr(a, HCC_TX_BDS, 1);
    wr(a, HCC_RX_BDS_INT, 1);
    wr(a, HCC_TX_BDS_INT, 1);
    address = dk->dma_address((const void *)a->status);
    wr(a, HCC_STATUS_HI, (dosxc_u32)(address >> 32));
    wr(a, HCC_STATUS_LO, (dosxc_u32)address);
    wr(a, HCC_MODE, HCC_32BYTE | HCC_ENABLE);

    wr(a, RBDC_MODE, 2 | 4);
    wr(a, RXLP_MODE, 2);
    a->mac_mode = MAC_ENABLE | MAC_PORT_MII;
    wr(a, MAC_MODE, a->mac_mode | 0x9000);   /* clear the statistics */
    delay_us(40);
    wr(a, WDMA_MODE, 2 | 0x3fc | 0x20000000u);
    delay_us(40);
    dosxc_u32 rdma = 2 | 0x3fc | 0x30000u;
    if (a->asic == ASIC_5784) rdma |= 0x3800;
    wr(a, RDMA_RSRVCTRL, rd(a, RDMA_RSRVCTRL) | 4);
    wr(a, RDMA_MODE, rdma);
    delay_us(40);
    wr(a, RDC_MODE, 2);
    wr(a, RBDI_MODE, 2);
    wr(a, RDBDI_MODE, 2);
    wr(a, SBDC_MODE, 2);
    wr(a, SDC_MODE, 2 | (a->asic == ASIC_5761 ? 0x10 : 0));
    wr(a, SDI_MODE, 2);
    wr(a, SBDI_MODE, 2);
    wr(a, SRS_MODE, 2);
    wr(a, SDI_STATS_ENABLE, 0x7fffff);
    wr(a, SDI_STATS_CTL, 3);
    wr(a, MAC_STS, 0x401018);             /* acknowledge link and configuration changes */
    wr(a, MI_STS, 0);
    wr(a, MI_MODE, MI_MODE_500KHZ);
    delay_us(80);
    wr(a, MAC_STS, 0x401018);

    wr(a, RX_MTU, 1522);
    wr(a, MAC_ADDR1_LO, (dosxc_u32)a->mac[0] << 8 | a->mac[1]);
    wr(a, MAC_ADDR1_HI, (dosxc_u32)a->mac[2] << 24 | (dosxc_u32)a->mac[3] << 16 | (dosxc_u32)a->mac[4] << 8 | a->mac[5]);
    for (unsigned i = 0; i < 4; ++i) wr(a, MAR0 + i * 4, 0);

    for (unsigned i = 0; i < RING; ++i) {
        struct rx_bd *b = &a->std_ring[i];
        dosxc_u64 buffer = dk->dma_address(a->rx_buffers + i * BUFFER_SIZE);
        b->address.high = (dosxc_u32)(buffer >> 32);
        b->address.low = (dosxc_u32)buffer;
        b->length = BUFFER_SIZE;
        b->index = (unsigned short)i;
        b->flags = BD_END;
    }
    a->std_prod = RING - 1;
    a->return_cons = a->tx_prod = a->tx_cons = 0;
    dk_barrier();
    wr(a, MBX_RX_STD_PROD, RING - 1);
    wr(a, MBX_TX_HOST_PROD0, 0);
    wr(a, MBX_TX_NIC_PROD0, 0);

    wr(a, TX_MODE, rd(a, TX_MODE) | 0x100 | 2);    /* MBUF_LOCKUP_FIX, ENABLE */
    delay_us(100);
    for (int i = 0; i < 4; ++i) wr(a, MAR0 + 4u * (unsigned)i, 0xffffffffu);
    wr(a, RX_MODE, rd(a, RX_MODE) | 0x01000000u | 2);
    delay_us(10);
    wr(a, MAX_RX_FRAME_LOWAT, 2);
    return 0;
}

static int transmit(void *instance, const void *frame, unsigned length) {
    struct adapter *a = instance;
    if (length > BUFFER_SIZE) return -1;
    a->tx_cons = a->status->index[0].tx_cons;
    if ((a->tx_prod - a->tx_cons) % RING >= TX_BUFFERS - 1) return -1;
    unsigned slot = a->tx_prod % TX_BUFFERS;
    unsigned char *buffer = a->tx_buffers + slot * BUFFER_SIZE;
    __builtin_memcpy(buffer, frame, length);
    struct tx_bd *b = &a->tx_ring[a->tx_prod];
    dosxc_u64 address = dk->dma_address(buffer);
    b->address.high = (dosxc_u32)(address >> 32);
    b->address.low = (dosxc_u32)address;
    b->flags = BD_END;
    b->length = (unsigned short)length;
    b->vlan = 0;
    b->mss = 0;
    a->tx_prod = (a->tx_prod + 1) % RING;
    dk_barrier();
    wr(a, MBX_TX_HOST_PROD0, a->tx_prod);
    return 0;
}

static const struct dk_net_ops ops = {transmit};

/* Set the MAC to what the PHY negotiated. */
static void check_link(struct adapter *a) {
    unsigned bmsr = 0, aux = 0;
    phy_read(a, PHY_BMSR, &bmsr);
    if (phy_read(a, PHY_BMSR, &bmsr)) return;
    int up = (bmsr & 4) != 0;
    if (!up) {
        if (a->link) { a->link = 0; dk->net_link(a->handle, 0, 0); dk->log("TG3: link down"); }
        return;
    }
    if (phy_read(a, PHY_AUX_STATUS, &aux)) return;
    unsigned mode = (aux >> 8) & 7, speed = mode >= 6 ? 1000 : mode >= 3 ? 100 : 10;
    int half = mode == 1 || mode == 3 || mode == 6;
    dosxc_u32 wanted = MAC_ENABLE | (speed == 1000 ? MAC_PORT_GMII : MAC_PORT_MII) | (half ? MAC_HALF : 0);
    if (wanted != a->mac_mode) {
        a->mac_mode = wanted;
        wr(a, MAC_MODE, wanted);
        delay_us(40);
    }
    if (!a->link) {
        a->link = 1;
        dk->net_link(a->handle, 1, speed);
        dk->log("TG3: link up, %u Mbit/s %s duplex", speed, half ? "half" : "full");
    }
}

static void poll(void *instance) {
    struct adapter *a = instance;
    dk_write32(a->regs, MBX_IRQ0, 0);
    unsigned prod = a->status->index[0].rx_prod;
    int any = 0;
    while (a->return_cons != prod) {
        struct rx_bd *r = &a->return_ring[a->return_cons];
        unsigned index = r->index % RING;
        if (!(r->flags & BD_ERROR) && r->length > 4 + 14)
            dk->net_receive(a->handle, a->rx_buffers + index * BUFFER_SIZE, r->length - 4u);
        struct rx_bd *b = &a->std_ring[a->std_prod];
        dosxc_u64 buffer = dk->dma_address(a->rx_buffers + index * BUFFER_SIZE);
        b->address.high = (dosxc_u32)(buffer >> 32);
        b->address.low = (dosxc_u32)buffer;
        b->length = BUFFER_SIZE;
        b->index = (unsigned short)index;
        b->flags = BD_END;
        a->std_prod = (a->std_prod + 1) % RING;
        a->return_cons = (a->return_cons + 1) % RING;
        prod = a->status->index[0].rx_prod;
        any = 1;
    }
    if (any) {
        dk_barrier();
        wr(a, MBX_RX_CONS0, a->return_cons);
        wr(a, MBX_RX_STD_PROD, a->std_prod);
    }
    dosxc_u64 now = dk->nanoseconds();
    if (now >= a->next_coalesce) {
        a->next_coalesce = now + 200000ull;
        wr(a, HCC_MODE, HCC_32BYTE | HCC_ENABLE | HCC_COAL_NOW);
    }
    if (now >= a->next_link_check) {
        a->next_link_check = now + 500000000ull;
        check_link(a);
    }
}

static int load(const struct dk_api *api) { dk = api; return 0; }

static void remove_adapter(void *instance) {
    struct adapter *a = instance;
    wr(a, RX_MODE, rd(a, RX_MODE) & ~2u);
    wr(a, TX_MODE, rd(a, TX_MODE) & ~2u);
    wr(a, MAC_MODE, a->mac_mode & ~0x00600000u);
    delay_us(100);
    chip_reset(a);
    dk->net_unregister(a->handle);
    dk->pci_write16(&a->device, 4, (unsigned short)(dk->pci_read16(&a->device, 4) & ~4u));
    dk->dma_free(a->std_ring);
    dk->dma_free(a->return_ring);
    dk->dma_free(a->tx_ring);
    dk->dma_free((void *)a->status);
    dk->dma_free(a->rx_buffers);
    dk->dma_free(a->tx_buffers);
    dk->free(a);
}

static int probe(const struct dk_pci_device *device, void **instance) {
    int io = 0;
    dosxc_u64 bar = dk->pci_bar(device, 0, &io);
    if (!bar || io) { dk->log("TG3: register BAR is not usable"); return -1; }
    volatile void *regs = dk->mmio_map(bar, 0x10000);
    if (!regs) return -1;
    dk->pci_enable(device, 1, 0);
    struct adapter *a = dk->alloc(sizeof(*a));
    if (!a) return -1;
    a->regs = regs;
    a->device = *device;
    dk->pci_write32(device, PCI_MISC_CTL, MISC_INIT);
    dosxc_u32 chip = dk->pci_read32(device, PCI_MISC_CTL) >> 16;
    if (chip >> 12 == 0x0f) chip = dk->pci_read32(device, PCI_PRODID_ASICREV);
    a->asic = chip >> 12;
    dk->log("TG3: %04x:%04x at %02x:%02x.%u, chip %x, PCI state %x", device->vendor, device->id, device->bus, device->device,
            device->function, chip, dk->pci_read32(device, PCI_PCISTATE));
    if (a->asic != ASIC_5784 && a->asic != ASIC_5761) {
        dk->log("TG3: chip family %x is not supported", a->asic);
        dk->free(a);
        return -1;
    }
    if (dk->pci_read32(device, PCI_PCISTATE) & PCISTATE_APE)
        dk->log("TG3: management firmware (APE) appears active; this driver does not coordinate with it");
    a->pcie_cap = find_pcie(device);
    a->std_ring = dk->dma_alloc(RING * sizeof(struct rx_bd), 4096);
    a->return_ring = dk->dma_alloc(RING * sizeof(struct rx_bd), 4096);
    a->tx_ring = dk->dma_alloc(RING * sizeof(struct tx_bd), 4096);
    a->status = dk->dma_alloc(4096, 4096);
    a->rx_buffers = dk->dma_alloc(RING * BUFFER_SIZE, 4096);
    a->tx_buffers = dk->dma_alloc(TX_BUFFERS * BUFFER_SIZE, 4096);
    if (!a->std_ring || !a->return_ring || !a->tx_ring || !a->status || !a->rx_buffers || !a->tx_buffers) {
        dk->log("TG3: out of DMA memory");
        return -1;
    }
    if (chip_reset(a)) return -1;
    if (read_mac(a)) { dk->log("TG3: no MAC address found"); return -1; }
    if (chip_init(a)) return -1;
    a->handle = dk->net_register(&ops, a, a->mac, "Broadcom");
    if (a->handle < 0) return -1;
    phy_write(a, PHY_ANAR, 0x01e1);            /* advertise every copper speed */
    phy_write(a, PHY_1000CTL, 0x0300);
    phy_write(a, PHY_BMCR, 0x1200);            /* restart autonegotiation */
    dk->log("TG3: MAC %02x:%02x:%02x:%02x:%02x:%02x", a->mac[0], a->mac[1], a->mac[2], a->mac[3], a->mac[4], a->mac[5]);
    *instance = a;
    return 0;
}

DK_DRIVER = {DK_ABI_VERSION, sizeof(struct dk_driver), "Broadcom NetXtreme", "1.0",
             "BCM5761/5764/5784 Gigabit Ethernet (HP Z400)", ids, load, probe, poll, remove_adapter, 0};
