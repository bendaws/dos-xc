/*
 * DOS-XC /hyperv/storage.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "hyperv.h"

static const u8 scsi_guid[16] = HV_GUID(0xba6163d9, 0x04a1, 0x4d29, 0xb6, 0x05, 0x72, 0xe2, 0xff, 0xb1, 0xdc, 0x7f);

#define OP_COMPLETE_IO 1
#define OP_EXECUTE_SRB 3
#define OP_BEGIN_INIT 7
#define OP_END_INIT 8
#define OP_QUERY_VERSION 9
#define OP_QUERY_PROPERTIES 10
#define REQUEST_COMPLETION 1
#define SRB_SUCCESS 1
#define SRB_FLAGS_IN 0x40u
#define SRB_FLAGS_OUT 0x80u
#define SRB_FLAGS_NO_SYNC 0x2000u
#define DATA_WRITE 0
#define DATA_READ 1
#define DATA_NONE 2

#define RING_BYTES (128u * 1024)
#define BOUNCE_BYTES (64u * 512)
#define MAX_CONTROLLERS 2
#define MAX_LUNS 64
#define MAX_DISKS 8
#define IO_WAIT_NS 30000000000ull
#define WAIT_NS 5000000000ull

struct PACKED srb {
    u16 length;
    u8 srb_status, scsi_status, port, path, target, lun, cdb_length, sense_length, data_in, reserved;
    u32 transfer_length;
    u8 cdb[20];
    u16 reserve;
    u8 queue_tag, queue_action;
    u32 flags, timeout, sort_key;
};

struct PACKED vstor_packet {
    u32 operation, flags, status;
    union {
        struct srb srb;
        struct PACKED { u16 major_minor, revision; } version;
        u8 buffer[52];
    };
};

struct controller {
    struct vmbus_channel *channel;
    u8 *bounce;
    u64 next_transaction;
};

struct disk {
    struct controller *controller;
    u8 lun;
    int handle;
};

static struct controller controllers[MAX_CONTROLLERS];
static struct disk disks[MAX_DISKS];
static int disk_total;

/* Send a packet and wait for the reply to it (copied back over P). */
static int exchange(struct controller *c, struct vstor_packet *p, const void *data, u32 bytes, u64 limit) {
    u64 transaction = ++c->next_transaction;
    p->flags = REQUEST_COMPLETION;
    int r = bytes ? vmbus_send_gpa(c->channel, data, bytes, p, sizeof(*p), transaction)
                  : vmbus_send(c->channel, p, sizeof(*p), transaction, VM_PKT_DATA_INBAND, VMBUS_COMPLETION_REQUESTED);
    if (r) return -1;
    u8 buffer[256];
    u64 end = dk->nanoseconds() + limit;
    while (dk->nanoseconds() < end) {
        struct vmbus_packet info;
        r = vmbus_read(c->channel, buffer, sizeof(buffer), &info);
        if (r == 0) { __asm__ volatile("pause"); continue; }
        if (r < 0 || info.transaction != transaction || info.type != VM_PKT_COMP) continue;
        u32 available = info.total - info.data_offset;
        memcpy(p, buffer + info.data_offset, available < sizeof(*p) ? available : sizeof(*p));
        return 0;
    }
    dk->log("HYPERV DISK: request timed out");
    return -1;
}

static int operation(struct controller *c, u32 op, int check) {
    struct vstor_packet p;
    memset(&p, 0, sizeof(p));
    p.operation = op;
    if (exchange(c, &p, 0, 0, WAIT_NS)) return -1;
    if (check && (p.operation != OP_COMPLETE_IO || p.status)) return -1;
    return 0;
}

/* A SCSI command.  Returns 0, or -1 with *SRB_STATUS set when it can be told. */
static int scsi(struct controller *c, u8 lun, const u8 *cdb, u8 cdb_length, int direction, u32 bytes, u8 *srb_status) {
    struct vstor_packet p;
    memset(&p, 0, sizeof(p));
    p.operation = OP_EXECUTE_SRB;
    p.srb.length = sizeof(struct srb);
    p.srb.lun = lun;
    p.srb.cdb_length = cdb_length;
    p.srb.sense_length = 20;
    p.srb.data_in = (u8)direction;
    p.srb.transfer_length = bytes;
    p.srb.flags = SRB_FLAGS_NO_SYNC | (direction == DATA_READ ? SRB_FLAGS_IN : direction == DATA_WRITE ? SRB_FLAGS_OUT : 0);
    p.srb.timeout = 60;
    memcpy(p.srb.cdb, cdb, cdb_length);
    if (exchange(c, &p, bytes ? c->bounce : 0, bytes, IO_WAIT_NS)) return -1;
    if (srb_status) *srb_status = p.srb.srb_status;
    return p.operation == OP_COMPLETE_IO && !p.status && p.srb.srb_status == SRB_SUCCESS && !p.srb.scsi_status ? 0 : -1;
}

static int disk_io(void *instance, dosxc_u64 lba, unsigned count, void *buffer, int write) {
    struct disk *d = instance;
    struct controller *c = d->controller;
    u32 bytes = count * 512;
    if (bytes > BOUNCE_BYTES) return -1;
    if (write) memcpy(c->bounce, buffer, bytes);
    u8 cdb[16];
    memset(cdb, 0, sizeof(cdb));
    u8 length;
    if (lba + count > 0xffffffffull) {
        cdb[0] = write ? 0x8a : 0x88;
        for (int i = 0; i < 8; ++i) cdb[2 + i] = (u8)(lba >> (56 - 8 * i));
        cdb[10] = (u8)(count >> 24);
        cdb[11] = (u8)(count >> 16);
        cdb[12] = (u8)(count >> 8);
        cdb[13] = (u8)count;
        length = 16;
    } else {
        cdb[0] = write ? 0x2a : 0x28;
        for (int i = 0; i < 4; ++i) cdb[2 + i] = (u8)(lba >> (24 - 8 * i));
        cdb[7] = (u8)(count >> 8);
        cdb[8] = (u8)count;
        length = 10;
    }
    int r = scsi(c, d->lun, cdb, length, write ? DATA_WRITE : DATA_READ, bytes, 0);
    if (!r && !write) memcpy(buffer, c->bounce, bytes);
    return r;
}

static const struct dk_disk_ops ops = {disk_io};

static void probe_lun(struct controller *c, u8 lun) {
    u8 status = 0;
    u8 inquiry[6] = {0x12, 0, 0, 0, 36, 0};
    memset(c->bounce, 0, 36);
    if (scsi(c, lun, inquiry, 6, DATA_READ, 36, &status)) return;
    if ((c->bounce[0] & 0x1f) != 0 || (c->bounce[0] >> 5) != 0) return;   /* a disk that is present */

    u64 sectors = 0;
    u32 block = 0;
    for (int tries = 0; tries < 4 && !block; ++tries) {
        u8 capacity[10] = {0x25};
        memset(c->bounce, 0, 8);
        if (scsi(c, lun, capacity, 10, DATA_READ, 8, 0)) continue;
        const u8 *b = c->bounce;
        sectors = ((u64)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3]) + 1;
        block = (u32)b[4] << 24 | (u32)b[5] << 16 | (u32)b[6] << 8 | b[7];
        if (sectors == 0x100000000ull) {
            u8 long_capacity[16] = {0x9e, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32};
            memset(c->bounce, 0, 32);
            if (scsi(c, lun, long_capacity, 16, DATA_READ, 32, 0)) { block = 0; continue; }
            sectors = 0;
            for (int i = 0; i < 8; ++i) sectors = sectors << 8 | b[i];
            ++sectors;
            block = (u32)b[8] << 24 | (u32)b[9] << 16 | (u32)b[10] << 8 | b[11];
        }
    }
    if (block != 512) {
        if (block) dk->log("HYPERV DISK: LUN %u uses %u-byte sectors, which are not supported", lun, (unsigned)block);
        return;
    }
    if (disk_total == MAX_DISKS) return;
    struct disk *d = &disks[disk_total];
    d->controller = c;
    d->lun = lun;
    d->handle = dk->disk_register(&ops, d, sectors, 0, "Hyper-V");
    if (d->handle < 0) return;
    ++disk_total;
    dk->log("HYPERV DISK: LUN %u, %lu MB", lun, (unsigned long)(sectors / 2048));
}

static int start_controller(struct controller *c, struct vmbus_channel *channel) {
    static const u16 versions[] = {0x0602, 0x0600, 0x0501};
    c->channel = channel;
    c->bounce = dk->dma_alloc(BOUNCE_BYTES, HV_PAGE);
    if (!c->bounce || vmbus_open(channel, RING_BYTES)) return -1;
    if (operation(c, OP_BEGIN_INIT, 1)) return -1;
    int accepted = 0;
    for (unsigned i = 0; i < sizeof(versions) / sizeof(*versions) && !accepted; ++i) {
        struct vstor_packet p;
        memset(&p, 0, sizeof(p));
        p.operation = OP_QUERY_VERSION;
        p.version.major_minor = versions[i];
        if (exchange(c, &p, 0, 0, WAIT_NS)) return -1;
        accepted = p.operation == OP_COMPLETE_IO && !p.status;
    }
    if (!accepted) {
        dk->log("HYPERV DISK: no storage protocol was accepted");
        return -1;
    }
    if (operation(c, OP_QUERY_PROPERTIES, 1) || operation(c, OP_END_INIT, 1)) return -1;
    for (int lun = 0; lun < MAX_LUNS; ++lun) probe_lun(c, (u8)lun);
    return 0;
}

int hv_storage_start(void) {
    int started = 0;
    for (int i = 0; i < MAX_CONTROLLERS; ++i) {
        struct vmbus_channel *channel = vmbus_find(scsi_guid, i);
        if (!channel) break;
        if (!start_controller(&controllers[i], channel)) ++started;
        else dk->log("HYPERV DISK: controller %d did not start", i);
    }
    return started ? 0 : -1;
}

void hv_storage_poll(void) {}

void hv_storage_stop(void) {
    for (int i = 0; i < disk_total; ++i) dk->disk_unregister(disks[i].handle);
    disk_total = 0;
    for (int i = 0; i < MAX_CONTROLLERS; ++i) vmbus_close(controllers[i].channel);
}
