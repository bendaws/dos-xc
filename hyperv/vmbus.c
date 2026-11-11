/*
 * DOS-XC /hyperv/vmbus.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "hyperv.h"

#define MSR_GUEST_OS_ID 0x40000000u
#define MSR_HYPERCALL 0x40000001u
#define MSR_SCONTROL 0x40000080u
#define MSR_SIEFP 0x40000082u
#define MSR_SIMP 0x40000083u
#define MSR_EOM 0x40000084u
#define MSR_SINT0 0x40000090u

#define HVCALL_POST_MESSAGE 0x5c
#define HVCALL_SIGNAL_EVENT 0x5d
#define HV_FAST 0x10000u
#define HV_STATUS_INSUFFICIENT_BUFFERS 0x13

#define VMBUS_SINT 2
#define SINT_VECTOR 0x1c          /* a gate that does nothing; see build_idt in cpu.c */
#define SINT_AUTO_EOI (1u << 17)
#define MESSAGE_PORT_CONNECTION_OLD 1
#define MESSAGE_PORT_CONNECTION 4
#define MESSAGE_TYPE_CHANNEL 1

#define VERSION(major, minor) (((u32)(major) << 16) | (minor))
#define V5_0 VERSION(5, 0)
#define V5_3 VERSION(5, 3)

enum {
    MSG_OFFER = 1, MSG_RESCIND = 2, MSG_REQUEST_OFFERS = 3, MSG_ALL_OFFERS = 4, MSG_OPEN = 5, MSG_OPEN_RESULT = 6,
    MSG_CLOSE = 7, MSG_GPADL_HEADER = 8, MSG_GPADL_BODY = 9, MSG_GPADL_CREATED = 10, MSG_GPADL_TEARDOWN = 11,
    MSG_GPADL_TORNDOWN = 12, MSG_RELID_RELEASED = 13, MSG_INITIATE = 14, MSG_VERSION_RESPONSE = 15, MSG_UNLOAD = 16,
    MSG_UNLOAD_RESPONSE = 17,
};

#define MAX_CHANNELS 32
#define MESSAGE_PAYLOAD 240
#define WAIT_NS 5000000000ull

struct vmbus_ring {
    u32 write_index, read_index, interrupt_mask, pending_send;
    u32 reserved[12];
    u32 features;
    u8 padding[HV_PAGE - 68];
    u8 data[];
} PACKED;

struct hv_message {
    u32 type;
    u8 payload_size, flags, reserved[2];
    u64 sender;
    u8 payload[MESSAGE_PAYLOAD];
} PACKED;

struct post_message {
    u32 connection, reserved, type, size;
    u8 payload[MESSAGE_PAYLOAD];
} PACKED;

struct offer {
    u32 type, padding;
    u8 interface[16], instance[16];
    u64 reserved1, reserved2;
    u16 flags, mmio;
    u8 user[120];
    u16 sub_index, reserved3;
    u32 relid;
    u8 monitor, monitor_flags;
    u16 interrupt_flags;
    u32 connection;
} PACKED;

static u8 *hypercall_page, *message_page, *event_page, *monitor_pages;
static struct post_message *post_page;
static struct vmbus_channel channels[MAX_CHANNELS];
static u32 message_connection, next_gpadl = 0xe1e10;
static int connected;

static u64 rdmsr(u32 index) {
    u32 low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(index));
    return (u64)high << 32 | low;
}

static void wrmsr(u32 index, u64 value) {
    __asm__ volatile("wrmsr" : : "c"(index), "a"((u32)value), "d"((u32)(value >> 32)));
}

static u64 hypercall(u64 control, u64 input, u64 output) {
#if defined(__x86_64__)
    u64 status;
    register u64 r8 __asm__("r8") = output;
    __asm__ volatile("call *%[page]" : "=a"(status), "+c"(control), "+d"(input), "+r"(r8)
                     : [page] "m"(hypercall_page) : "r9", "r10", "r11", "memory", "cc");
    return status;
#else
    u32 low = (u32)control, high = (u32)(control >> 32);
    u8 *volatile page = hypercall_page;
    u32 input_low = (u32)input, input_high = (u32)(input >> 32);
    u32 output_low = (u32)output, output_high = (u32)(output >> 32);
    __asm__ volatile("call *%[page]" : "+a"(low), "+d"(high), "+c"(input_low), "+b"(input_high), "+S"(output_low), "+D"(output_high)
                     : [page] "m"(page) : "memory", "cc");
    return (u64)high << 32 | low;
#endif
}

static int hypervisor_present(void) {
    u32 a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(c & (1u << 31))) return 0;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x40000000u), "c"(0));
    if (b != 0x7263694d || c != 0x666f736f || d != 0x76482074) return 0;   /* "Microsoft Hv" */
    if (a < 0x40000003u) return 0;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x40000001u), "c"(0));
    if (a != 0x31237648) return 0;                                         /* "Hv#1" */
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x40000003u), "c"(0));
    return (a & (1u << 2)) && (a & (1u << 5));
}

/* ---- control messages ---- */

static int post(const void *data, u32 size) {
    u64 end = dk->nanoseconds() + WAIT_NS;
    for (;;) {
        post_page->connection = message_connection;
        post_page->reserved = 0;
        post_page->type = MESSAGE_TYPE_CHANNEL;
        post_page->size = size;
        memcpy(post_page->payload, data, size);
        u64 status = hypercall(HVCALL_POST_MESSAGE, dk->dma_address(post_page), 0) & 0xffff;
        if (!status) return 0;
        if (status != HV_STATUS_INSUFFICIENT_BUFFERS || dk->nanoseconds() > end) {
            dk->log("HYPERV: post message failed, status %x", (unsigned)status);
            return -1;
        }
        dk->sleep(100000);
    }
}

static void take_offer(const struct offer *o) {
    for (int i = 0; i < MAX_CHANNELS; ++i) {
        struct vmbus_channel *c = &channels[i];
        if (c->used) continue;
        memset(c, 0, sizeof(*c));
        c->used = 1;
        memcpy(c->type, o->interface, 16);
        memcpy(c->instance, o->instance, 16);
        c->relid = o->relid;
        c->connection = o->connection;
        return;
    }
}

/* One waiting message, if any, copied to OUT after handling offers and
   rescinds.  Returns its type, or 0. */
static u32 next_message(u8 *out) {
    volatile struct hv_message *m = (volatile struct hv_message *)(message_page + VMBUS_SINT * sizeof(struct hv_message));
    if (!m->type) return 0;
    dk_barrier();
    memcpy(out, (const void *)m->payload, MESSAGE_PAYLOAD);
    u8 flags = m->flags;
    m->type = 0;
    dk_barrier();
    if (flags & 1) wrmsr(MSR_EOM, 0);
    u32 type = *(u32 *)out;
    if (type == MSG_OFFER) {
        take_offer((const struct offer *)out);
    } else if (type == MSG_RESCIND) {
        u32 relid = *(u32 *)(out + 8);
        for (int i = 0; i < MAX_CHANNELS; ++i)
            if (channels[i].used && channels[i].relid == relid && !channels[i].open) channels[i].used = 0;
    }
    return type;
}

/* Wait for a message of TYPE whose word at OFFSET (if KEY_OFFSET) equals KEY. */
static int wait_message(u32 type, int key_offset, u32 key, u8 *out) {
    u64 end = dk->nanoseconds() + WAIT_NS;
    while (dk->nanoseconds() < end) {
        u32 got = next_message(out);
        if (got == type && (key_offset < 0 || *(u32 *)(out + key_offset) == key)) return 0;
        if (!got) __asm__ volatile("pause");
    }
    dk->log("HYPERV: timed out waiting for message type %u", (unsigned)type);
    return -1;
}

void vmbus_poll(void) {
    if (!connected) return;
    u8 buffer[MESSAGE_PAYLOAD];
    while (next_message(buffer)) {}
}

static int connect_version(u32 version) {
    struct PACKED {
        u32 type, padding, version, target_vcpu;
        u8 sint, vtl, reserved[2];
        u32 features;
        u64 monitor1, monitor2;
    } request;
    memset(&request, 0, sizeof(request));
    request.type = MSG_INITIATE;
    request.version = version;
    request.sint = VMBUS_SINT;
    request.monitor1 = dk->dma_address(monitor_pages);
    request.monitor2 = request.monitor1 + HV_PAGE;
    message_connection = version >= V5_0 ? MESSAGE_PORT_CONNECTION : MESSAGE_PORT_CONNECTION_OLD;
    if (post(&request, sizeof(request))) return -1;
    u8 reply[MESSAGE_PAYLOAD];
    if (wait_message(MSG_VERSION_RESPONSE, -1, 0, reply)) return -1;
    if (!reply[8]) return 1;
    if (version >= V5_3) message_connection = *(u32 *)(reply + 12);
    return 0;
}

int vmbus_start(void) {
    static const u32 versions[] = {VERSION(5, 3), VERSION(5, 2), VERSION(5, 1), VERSION(5, 0), VERSION(4, 1), VERSION(4, 0)};
    if (!hypervisor_present()) return -1;
    if (dk->firmware_services()) {
        dk->log("HYPERV: UEFI boot: the firmware owns VMBus, so a Generation 1 VM is needed");
        return -1;
    }
    hypercall_page = dk->dma_alloc(HV_PAGE, HV_PAGE);
    message_page = dk->dma_alloc(HV_PAGE, HV_PAGE);
    event_page = dk->dma_alloc(HV_PAGE, HV_PAGE);
    monitor_pages = dk->dma_alloc(HV_PAGE * 2, HV_PAGE);
    post_page = dk->dma_alloc(HV_PAGE, HV_PAGE);
    if (!hypercall_page || !message_page || !event_page || !monitor_pages || !post_page) return -1;

    wrmsr(MSR_GUEST_OS_ID, (u64)1 << 63 | (u64)0x7f << 56 | 2);   /* open source, other OS, ABI 2 */
    wrmsr(MSR_HYPERCALL, dk->dma_address(hypercall_page) | 1);
    if (!(rdmsr(MSR_HYPERCALL) & 1)) {
        dk->log("HYPERV: the hypercall page was refused");
        return -1;
    }
    wrmsr(MSR_SIMP, dk->dma_address(message_page) | 1);
    wrmsr(MSR_SIEFP, dk->dma_address(event_page) | 1);
    wrmsr(MSR_SINT0 + VMBUS_SINT, SINT_VECTOR | SINT_AUTO_EOI);
    wrmsr(MSR_SCONTROL, 1);

    int r = 1;
    u32 chosen = 0;
    for (unsigned i = 0; i < sizeof(versions) / sizeof(*versions) && r > 0; ++i) {
        chosen = versions[i];
        r = connect_version(chosen);
    }
    if (r) {
        dk->log("HYPERV: no VMBus version was accepted");
        return -1;
    }
    connected = 1;
    dk->log("HYPERV: VMBus %u.%u connected", (unsigned)(chosen >> 16), (unsigned)(chosen & 0xffff));

    struct { u32 type, padding; } request = {MSG_REQUEST_OFFERS, 0};
    if (post(&request, sizeof(request))) return -1;
    u8 reply[MESSAGE_PAYLOAD];
    if (wait_message(MSG_ALL_OFFERS, -1, 0, reply)) return -1;
    return 0;
}

void vmbus_stop(void) {
    if (connected) {
        struct { u32 type, padding; } request = {MSG_UNLOAD, 0};
        u8 reply[MESSAGE_PAYLOAD];
        if (!post(&request, sizeof(request))) wait_message(MSG_UNLOAD_RESPONSE, -1, 0, reply);
        connected = 0;
    }
    if (hypercall_page && (rdmsr(MSR_SCONTROL) & 1)) {
        wrmsr(MSR_SCONTROL, 0);
        wrmsr(MSR_SINT0 + VMBUS_SINT, 1u << 16);
        wrmsr(MSR_SIMP, 0);
        wrmsr(MSR_SIEFP, 0);
        wrmsr(MSR_HYPERCALL, 0);
        wrmsr(MSR_GUEST_OS_ID, 0);
    }
}

struct vmbus_channel *vmbus_find(const u8 type[16], int index) {
    for (int i = 0; i < MAX_CHANNELS; ++i)
        if (channels[i].used && !memcmp(channels[i].type, type, 16) && !index--) return &channels[i];
    return 0;
}

/* ---- shared memory ---- */

int vmbus_gpadl(struct vmbus_channel *c, void *buffer, u32 bytes, u32 *out) {
    u32 pages = (bytes + HV_PAGE - 1) / HV_PAGE;
    u64 first = dk->dma_address(buffer) >> 12;
    u32 handle = next_gpadl++;

    struct PACKED {
        u32 type, padding, relid, gpadl;
        u16 range_bytes, ranges;
        u32 byte_count, byte_offset;
        u64 pfn[26];
    } header;
    memset(&header, 0, sizeof(header));
    header.type = MSG_GPADL_HEADER;
    header.relid = c->relid;
    header.gpadl = handle;
    header.range_bytes = (u16)(8 + 8 * pages);
    header.ranges = 1;
    header.byte_count = pages * HV_PAGE;
    u32 here = pages < 26 ? pages : 26;
    for (u32 i = 0; i < here; ++i) header.pfn[i] = first + i;
    if (post(&header, 28 + 8 * here)) return -1;

    for (u32 sent = here; sent < pages;) {
        struct PACKED {
            u32 type, padding, number, gpadl;
            u64 pfn[28];
        } body;
        memset(&body, 0, sizeof(body));
        body.type = MSG_GPADL_BODY;
        body.gpadl = handle;
        u32 n = pages - sent < 28 ? pages - sent : 28;
        for (u32 i = 0; i < n; ++i) body.pfn[i] = first + sent + i;
        if (post(&body, 16 + 8 * n)) return -1;
        sent += n;
    }

    u8 reply[MESSAGE_PAYLOAD];
    if (wait_message(MSG_GPADL_CREATED, 12, handle, reply)) return -1;
    u32 status = *(u32 *)(reply + 16);
    if (status) {
        dk->log("HYPERV: shared memory refused, status %x", (unsigned)status);
        return -1;
    }
    *out = handle;
    return 0;
}

void vmbus_gpadl_free(struct vmbus_channel *c, u32 handle) {
    struct { u32 type, padding, relid, gpadl; } request = {MSG_GPADL_TEARDOWN, 0, c->relid, handle};
    u8 reply[MESSAGE_PAYLOAD];
    if (!post(&request, sizeof(request))) wait_message(MSG_GPADL_TORNDOWN, 8, handle, reply);
}

int vmbus_open(struct vmbus_channel *c, u32 ring_bytes) {
    u32 total = ring_bytes * 2;
    c->ring_memory = dk->dma_alloc(total, HV_PAGE);
    if (!c->ring_memory) return -1;
    c->ring_bytes = total;
    c->out = (volatile struct vmbus_ring *)c->ring_memory;
    c->in = (volatile struct vmbus_ring *)(c->ring_memory + ring_bytes);
    c->out_size = c->in_size = ring_bytes - HV_PAGE;
    c->in->interrupt_mask = 1;
    if (vmbus_gpadl(c, c->ring_memory, total, &c->ring_gpadl)) return -1;

    struct PACKED {
        u32 type, padding, relid, open_id, gpadl, target_vp, downstream_offset;
        u8 user[120];
    } request;
    memset(&request, 0, sizeof(request));
    request.type = MSG_OPEN;
    request.relid = c->relid;
    request.open_id = c->relid;
    request.gpadl = c->ring_gpadl;
    request.downstream_offset = ring_bytes / HV_PAGE;
    if (post(&request, sizeof(request))) return -1;
    u8 reply[MESSAGE_PAYLOAD];
    if (wait_message(MSG_OPEN_RESULT, 8, c->relid, reply)) return -1;
    u32 status = *(u32 *)(reply + 16);
    if (status) {
        dk->log("HYPERV: channel %u refused to open, status %x", (unsigned)c->relid, (unsigned)status);
        return -1;
    }
    c->open = 1;
    return 0;
}

void vmbus_close(struct vmbus_channel *c) {
    if (!c || !c->open) return;
    struct { u32 type, padding, relid; } request = {MSG_CLOSE, 0, c->relid};
    post(&request, sizeof(request));
    c->open = 0;
    vmbus_gpadl_free(c, c->ring_gpadl);
}

/* ---- ring buffers ---- */

static void signal_host(struct vmbus_channel *c) {
    hypercall(HVCALL_SIGNAL_EVENT | HV_FAST, c->connection, 0);
}

static void ring_copy_in(volatile struct vmbus_ring *r, u32 size, u32 *at, const void *from, u32 bytes) {
    const u8 *p = from;
    u32 first = size - *at < bytes ? size - *at : bytes;
    memcpy((u8 *)r->data + *at, p, first);
    if (bytes > first) memcpy((u8 *)r->data, p + first, bytes - first);
    *at = (*at + bytes) % size;
}

static void ring_copy_out(volatile struct vmbus_ring *r, u32 size, u32 at, void *to, u32 bytes) {
    u8 *p = to;
    u32 first = size - at < bytes ? size - at : bytes;
    memcpy(p, (const u8 *)r->data + at, first);
    if (bytes > first) memcpy(p + first, (const u8 *)r->data, bytes - first);
}

static int ring_write(struct vmbus_channel *c, const void *first, u32 first_bytes, const void *second, u32 second_bytes) {
    volatile struct vmbus_ring *r = c->out;
    u32 bytes = first_bytes + second_bytes;
    u32 padded = (bytes + 7) & ~7u;
    u32 needed = padded + 8;
    u32 write = r->write_index, read = r->read_index;
    u32 free_bytes = write >= read ? c->out_size - (write - read) : read - write;
    if (free_bytes <= needed) return -1;

    static const u8 zero[8];
    u32 start = write, at = write;
    ring_copy_in(r, c->out_size, &at, first, first_bytes);
    if (second_bytes) ring_copy_in(r, c->out_size, &at, second, second_bytes);
    if (padded > bytes) ring_copy_in(r, c->out_size, &at, zero, padded - bytes);
    u64 trailer = (u64)start << 32;
    ring_copy_in(r, c->out_size, &at, &trailer, 8);
    dk_barrier();
    r->write_index = at;
    dk_barrier();
    if (!r->interrupt_mask && start == r->read_index) signal_host(c);
    return 0;
}

int vmbus_send(struct vmbus_channel *c, const void *data, u32 length, u64 transaction, u16 type, u16 flags) {
    struct PACKED { u16 type, offset8, len8, flags; u64 transaction; } d;
    d.type = type;
    d.offset8 = 2;
    d.len8 = (u16)((16 + length + 7) >> 3);
    d.flags = flags;
    d.transaction = transaction;
    return ring_write(c, &d, sizeof(d), data, length);
}

int vmbus_send_gpa(struct vmbus_channel *c, const void *buffer, u32 bytes, const void *data, u32 length, u64 transaction) {
    struct PACKED {
        u16 type, offset8, len8, flags;
        u64 transaction;
        u32 reserved, ranges, byte_count, byte_offset;
        u64 pfn[34];
    } d;
    u64 address = dk->dma_address(buffer);
    u32 offset = (u32)(address & (HV_PAGE - 1));
    u32 pages = (offset + bytes + HV_PAGE - 1) / HV_PAGE;
    if (pages > 34) return -1;
    u32 descriptor = 16 + 8 + 8 + 8 * pages;
    d.type = VM_PKT_DATA_USING_GPA_DIRECT;
    d.offset8 = (u16)(descriptor >> 3);
    d.len8 = (u16)((descriptor + length + 7) >> 3);
    d.flags = VMBUS_COMPLETION_REQUESTED;
    d.transaction = transaction;
    d.reserved = 0;
    d.ranges = 1;
    d.byte_count = bytes;
    d.byte_offset = offset;
    for (u32 i = 0; i < pages; ++i) d.pfn[i] = (address >> 12) + i;
    return ring_write(c, &d, descriptor, data, length);
}

int vmbus_read(struct vmbus_channel *c, void *out, u32 capacity, struct vmbus_packet *packet) {
    volatile struct vmbus_ring *r = c->in;
    u32 read = r->read_index, write = r->write_index;
    if (read == write) return 0;
    dk_barrier();

    struct PACKED { u16 type, offset8, len8, flags; u64 transaction; } d;
    ring_copy_out(r, c->in_size, read, &d, sizeof(d));
    u32 total = (u32)d.len8 << 3;
    int result = 1;
    if (total > capacity) result = -1;
    else ring_copy_out(r, c->in_size, read, out, total);
    packet->type = d.type;
    packet->total = total;
    packet->data_offset = (u32)d.offset8 << 3;
    packet->transaction = d.transaction;

    dk_barrier();
    r->read_index = (read + total + 8) % c->in_size;
    dk_barrier();
    if (r->pending_send) signal_host(c);
    return result;
}
