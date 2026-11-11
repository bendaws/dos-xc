/*
 * DOS-XC /ethernet/virtio-net.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include <dosxc/driver.h>

#define QUEUE_RX 0
#define QUEUE_TX 1
#define QUEUE_SIZE 256              /* at most; a device may offer fewer */
#define BUFFER_SIZE 2048

#define F_MAC (1u << 5)
#define F_STATUS (1u << 16)
#define F_VERSION_1_HIGH 1u           /* feature 32, in the high word */

#define S_ACK 1
#define S_DRIVER 2
#define S_DRIVER_OK 4
#define S_FEATURES_OK 8
#define S_FAILED 128

#define L_DEVICE_FEATURES 0
#define L_GUEST_FEATURES 4
#define L_QUEUE_ADDRESS 8
#define L_QUEUE_SIZE 12
#define L_QUEUE_SELECT 14
#define L_QUEUE_NOTIFY 16
#define L_STATUS 18
#define L_ISR 19
#define L_CONFIG 20

#define C_DEVICE_FEATURE_SELECT 0
#define C_DEVICE_FEATURE 4
#define C_GUEST_FEATURE_SELECT 8
#define C_GUEST_FEATURE 12
#define C_STATUS 20
#define C_QUEUE_SELECT 22
#define C_QUEUE_SIZE 24
#define C_QUEUE_ENABLE 28
#define C_QUEUE_NOTIFY_OFF 30
#define C_QUEUE_DESC 32
#define C_QUEUE_AVAIL 40
#define C_QUEUE_USED 48

struct descriptor { dosxc_u64 address; dosxc_u32 length; unsigned short flags, next; };
#define DESC_WRITE 2

struct queue {
    unsigned size;
    struct descriptor *desc;
    volatile unsigned short *avail;      /* flags, idx, ring[] */
    volatile unsigned short *used;       /* flags, idx, {id, len}[] */
    unsigned short last_used;
    unsigned char *buffers;
    volatile void *notify;
    unsigned free_head;
};

struct adapter {
    int legacy;
    unsigned short io;
    volatile void *common, *device_config;
    volatile unsigned char *notify_base;
    dosxc_u32 notify_multiplier;
    struct queue rx, tx;
    unsigned header;                    /* size of struct virtio_net_hdr in use */
    int handle, link;
    int has_status;
    dosxc_u64 next_link_check;
    struct dk_pci_device device;
};

static const struct dk_api *dk;
static const struct dk_pci_id ids[] = {{0x1af4, 0x1000}, {0x1af4, 0x1041}, {0, 0}};

static void set_status(struct adapter *a, unsigned char status) {
    if (a->legacy) dk_outb((unsigned short)(a->io + L_STATUS), status);
    else dk_write8(a->common, C_STATUS, status);
}
static unsigned char get_status(struct adapter *a) {
    return a->legacy ? dk_inb((unsigned short)(a->io + L_STATUS)) : dk_read8(a->common, C_STATUS);
}

static void notify(struct adapter *a, struct queue *q, unsigned index) {
    dk_barrier();
    if (a->legacy) dk_outw((unsigned short)(a->io + L_QUEUE_NOTIFY), (unsigned short)index);
    else dk_write16(q->notify, 0, (unsigned short)index);
}

static int setup_queue(struct adapter *a, struct queue *q, unsigned index) {
    unsigned size;
    if (a->legacy) {
        dk_outw((unsigned short)(a->io + L_QUEUE_SELECT), (unsigned short)index);
        size = dk_inw((unsigned short)(a->io + L_QUEUE_SIZE));
    } else {
        dk_write16(a->common, C_QUEUE_SELECT, (unsigned short)index);
        size = dk_read16(a->common, C_QUEUE_SIZE);
    }
    if (!size) return -1;
    if (!a->legacy && size > QUEUE_SIZE) {
        size = QUEUE_SIZE;
        dk_write16(a->common, C_QUEUE_SIZE, (unsigned short)size);
    }
    dosxc_size desc_bytes = 16 * (dosxc_size)size, avail_bytes = 6 + 2 * (dosxc_size)size;
    dosxc_size used_offset = (desc_bytes + avail_bytes + 4095) & ~(dosxc_size)4095;
    dosxc_size used_bytes = 6 + 8 * (dosxc_size)size;
    unsigned char *memory = dk->dma_alloc(used_offset + used_bytes, 4096);
    q->buffers = dk->dma_alloc((dosxc_size)size * BUFFER_SIZE, 4096);
    if (!memory || !q->buffers) return -1;
    q->size = size;
    q->desc = (struct descriptor *)memory;
    q->avail = (volatile unsigned short *)(memory + desc_bytes);
    q->used = (volatile unsigned short *)(memory + used_offset);
    if (a->legacy) {
        dk_outl((unsigned short)(a->io + L_QUEUE_ADDRESS), (dosxc_u32)(dk->dma_address(memory) >> 12));
    } else {
        dosxc_u64 d = dk->dma_address(memory);
        dk_write32(a->common, C_QUEUE_DESC, (dosxc_u32)d);
        dk_write32(a->common, C_QUEUE_DESC + 4, (dosxc_u32)(d >> 32));
        d += desc_bytes;
        dk_write32(a->common, C_QUEUE_AVAIL, (dosxc_u32)d);
        dk_write32(a->common, C_QUEUE_AVAIL + 4, (dosxc_u32)(d >> 32));
        d = dk->dma_address(memory) + used_offset;
        dk_write32(a->common, C_QUEUE_USED, (dosxc_u32)d);
        dk_write32(a->common, C_QUEUE_USED + 4, (dosxc_u32)(d >> 32));
        q->notify = a->notify_base + dk_read16(a->common, C_QUEUE_NOTIFY_OFF) * a->notify_multiplier;
        dk_write16(a->common, C_QUEUE_ENABLE, 1);
    }
    return 0;
}

static void post_rx(struct adapter *a, struct queue *q, unsigned slot) {
    q->desc[slot].address = dk->dma_address(q->buffers + slot * BUFFER_SIZE);
    q->desc[slot].length = BUFFER_SIZE;
    q->desc[slot].flags = DESC_WRITE;
    unsigned short idx = q->avail[1];
    q->avail[2 + idx % q->size] = (unsigned short)slot;
    dk_barrier();
    q->avail[1] = (unsigned short)(idx + 1);
}

static int transmit(void *instance, const void *frame, unsigned length) {
    struct adapter *a = instance;
    struct queue *q = &a->tx;
    if (length + a->header > BUFFER_SIZE) return -1;
    unsigned used = q->used[1];
    while (q->last_used != (unsigned short)used) {
        ++q->last_used;
    }
    unsigned short idx = q->avail[1];
    if ((unsigned short)(idx - q->last_used) >= q->size) return -1;
    unsigned slot = idx % q->size;
    unsigned char *buffer = q->buffers + slot * BUFFER_SIZE;
    __builtin_memset(buffer, 0, a->header);
    __builtin_memcpy(buffer + a->header, frame, length);
    q->desc[slot].address = dk->dma_address(buffer);
    q->desc[slot].length = a->header + length;
    q->desc[slot].flags = 0;
    q->avail[2 + slot] = (unsigned short)slot;
    dk_barrier();
    q->avail[1] = (unsigned short)(idx + 1);
    notify(a, q, QUEUE_TX);
    return 0;
}

static const struct dk_net_ops ops = {transmit};

static void check_link(struct adapter *a) {
    int up = 1;
    if (a->has_status) {
        unsigned status = a->legacy ? dk_inw((unsigned short)(a->io + L_CONFIG + 6)) : dk_read16(a->device_config, 6);
        up = status & 1;
    }
    if (up == a->link) return;
    a->link = up;
    dk->net_link(a->handle, up, 1000);
    dk->log("VIRTIONET: link %s", up ? "up" : "down");
}

static void poll(void *instance) {
    struct adapter *a = instance;
    struct queue *q = &a->rx;
    int consumed = 0;
    while (q->last_used != q->used[1]) {
        volatile dosxc_u32 *element = (volatile dosxc_u32 *)(q->used + 2) + 2 * (q->last_used % q->size);
        unsigned slot = element[0] % q->size;
        dosxc_u32 length = element[1];
        if (length > a->header + 14)
            dk->net_receive(a->handle, q->buffers + slot * BUFFER_SIZE + a->header, length - a->header);
        post_rx(a, q, slot);
        ++q->last_used;
        consumed = 1;
    }
    if (consumed) notify(a, q, QUEUE_RX);
    dosxc_u64 now = dk->nanoseconds();
    if (now >= a->next_link_check) {
        a->next_link_check = now + 500000000ull;
        check_link(a);
    }
}

static int load(const struct dk_api *api) { dk = api; return 0; }

/* Find the modern capabilities.  Returns 0 when the common, notify and
   device configuration structures are all reachable. */
static int find_modern(struct adapter *a, const struct dk_pci_device *device) {
    if (!(dk->pci_read16(device, 6) & 0x10)) return -1;
    unsigned at = dk->pci_read8(device, 0x34) & 0xfc;
    int found = 0;
    for (int hops = 0; at && hops < 32; ++hops, at = dk->pci_read8(device, (unsigned char)(at + 1)) & 0xfc) {
        if (dk->pci_read8(device, (unsigned char)at) != 9) continue;
        unsigned type = dk->pci_read8(device, (unsigned char)(at + 3));
        unsigned bar = dk->pci_read8(device, (unsigned char)(at + 4));
        dosxc_u32 offset = dk->pci_read32(device, (unsigned char)(at + 8));
        if (bar > 5 || (type != 1 && type != 2 && type != 4)) continue;
        int io = 0;
        dosxc_u64 address = dk->pci_bar(device, (int)bar, &io);
        if (!address || io) continue;
        volatile unsigned char *mapped = dk->mmio_map(address + offset, 0x1000);
        if (!mapped) continue;
        if (type == 1) { a->common = mapped; found |= 1; }
        else if (type == 2) {
            a->notify_base = mapped;
            a->notify_multiplier = dk->pci_read32(device, (unsigned char)(at + 16));
            found |= 2;
        } else { a->device_config = mapped; found |= 4; }
    }
    return found == 7 ? 0 : -1;
}

static int probe(const struct dk_pci_device *device, void **instance) {
    struct adapter *a = dk->alloc(sizeof(*a));
    if (!a) return -1;
    a->device = *device;
    int io = 0;
    if (find_modern(a, device)) {
        dosxc_u64 bar = dk->pci_bar(device, 0, &io);
        if (!bar || !io || device->id != 0x1000) { dk->log("VIRTIONET: no usable transport"); dk->free(a); return -1; }
        a->legacy = 1;
        a->io = (unsigned short)bar;
    }
    dk->pci_enable(device, !a->legacy, a->legacy);
    set_status(a, 0);
    set_status(a, S_ACK);
    set_status(a, S_ACK | S_DRIVER);
    dosxc_u32 features, wanted;
    if (a->legacy) {
        features = dk_inl((unsigned short)(a->io + L_DEVICE_FEATURES));
        wanted = features & (F_MAC | F_STATUS);
        dk_outl((unsigned short)(a->io + L_GUEST_FEATURES), wanted);
        a->header = 10;
    } else {
        dk_write32(a->common, C_DEVICE_FEATURE_SELECT, 0);
        features = dk_read32(a->common, C_DEVICE_FEATURE);
        wanted = features & (F_MAC | F_STATUS);
        dk_write32(a->common, C_GUEST_FEATURE_SELECT, 0);
        dk_write32(a->common, C_GUEST_FEATURE, wanted);
        dk_write32(a->common, C_GUEST_FEATURE_SELECT, 1);
        dk_write32(a->common, C_GUEST_FEATURE, F_VERSION_1_HIGH);
        set_status(a, S_ACK | S_DRIVER | S_FEATURES_OK);
        if (!(get_status(a) & S_FEATURES_OK)) { dk->log("VIRTIONET: feature negotiation refused"); set_status(a, S_FAILED); return -1; }
        a->header = 12;
    }
    a->has_status = (wanted & F_STATUS) != 0;
    if (!(wanted & F_MAC)) { dk->log("VIRTIONET: device has no MAC address"); set_status(a, S_FAILED); return -1; }
    if (setup_queue(a, &a->rx, QUEUE_RX) || setup_queue(a, &a->tx, QUEUE_TX)) {
        dk->log("VIRTIONET: queue setup failed");
        set_status(a, S_FAILED);
        return -1;
    }
    unsigned char mac[6];
    for (int i = 0; i < 6; ++i)
        mac[i] = a->legacy ? dk_inb((unsigned short)(a->io + L_CONFIG + i)) : dk_read8(a->device_config, (unsigned)i);
    unsigned status = S_ACK | S_DRIVER | S_DRIVER_OK | (a->legacy ? 0 : S_FEATURES_OK);
    set_status(a, (unsigned char)status);
    for (unsigned i = 0; i < a->rx.size; ++i) post_rx(a, &a->rx, i);
    notify(a, &a->rx, QUEUE_RX);
    char name[16];
    dk->format(name, sizeof(name), "VIRTIO/%s", a->legacy ? "legacy" : "modern");
    a->handle = dk->net_register(&ops, a, mac, name);
    if (a->handle < 0) return -1;
    dk->log("VIRTIONET: %02x:%02x.%u %s, MAC %02x:%02x:%02x:%02x:%02x:%02x, queues %u/%u", device->bus, device->device,
            device->function, a->legacy ? "legacy" : "modern", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], a->rx.size, a->tx.size);
    check_link(a);
    *instance = a;
    return 0;
}

static void remove_adapter(void *instance) {
    struct adapter *a = instance;
    set_status(a, 0);
    dk->net_unregister(a->handle);
    dk->pci_write16(&a->device, 4, (unsigned short)(dk->pci_read16(&a->device, 4) & ~4u));
}

DK_DRIVER = {DK_ABI_VERSION, sizeof(struct dk_driver), "virtio-net", "1.0",
             "Paravirtual Ethernet (legacy and modern)", ids, load, probe, poll, remove_adapter, 0};
