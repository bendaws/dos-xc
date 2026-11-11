/*
 * DOS-XC /hyperv/net.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "hyperv.h"

static const u8 net_guid[16] = HV_GUID(0xf8615163, 0xdf3e, 0x46c5, 0x91, 0x3f, 0xf2, 0xd2, 0xf9, 0x65, 0xed, 0x0e);

#define NVSP_INIT 1
#define NVSP_INIT_COMPLETE 2
#define NVSP_SEND_NDIS_VERSION 100
#define NVSP_SEND_RECEIVE_BUFFER 101
#define NVSP_SEND_RECEIVE_BUFFER_COMPLETE 102
#define NVSP_REVOKE_RECEIVE_BUFFER 103
#define NVSP_SEND_NDIS_CONFIG 125
#define NVSP_SEND_RNDIS 107
#define NVSP_SEND_RNDIS_COMPLETE 108
#define NVSP_SUCCESS 1
#define NVSP_MESSAGE 40               /* requests are padded to this size */
#define RECEIVE_BUFFER_ID 0xcafe
#define NO_SECTION 0xffffffffu

#define RNDIS_PACKET 1
#define RNDIS_INIT 2
#define RNDIS_QUERY 4
#define RNDIS_SET 5
#define RNDIS_INDICATE 7
#define RNDIS_COMPLETE 0x80000000u
#define RNDIS_STATUS_CONNECT 0x4001000bu
#define RNDIS_STATUS_DISCONNECT 0x4001000cu
#define OID_MAC 0x01010101u
#define OID_MEDIA_CONNECT 0x00010114u
#define OID_PACKET_FILTER 0x0001010eu
#define FILTER_DIRECTED 1u
#define FILTER_ALL_MULTICAST 4u
#define FILTER_BROADCAST 8u

#define RING_BYTES (256u * 1024)
#define RECEIVE_BYTES (4u * 1024 * 1024)
#define SEND_SLOTS 64
#define CONTROL_TRANSACTION 0x10000u
#define PACKET_BYTES 8192
#define WAIT_NS 5000000000ull
#define NOTHING 0xffffffffu           /* handle_packet: the ring is empty */

static struct vmbus_channel *channel;
static u8 *receive_buffer, *send_pages, *control_page, *packet;
static u32 receive_gpadl;
static u8 send_busy[SEND_SLOTS];
static int handle = -1, link_up, control_done;
static u32 control_id, last_offset;
static u8 control_reply[256];

static void receive_rndis(const u8 *m, u32 available);

static int nvsp(u32 type, u32 a, u32 b, u32 c) {
    u32 message[NVSP_MESSAGE / 4] = {type, a, b, c};
    return vmbus_send(channel, message, sizeof(message), type, VM_PKT_DATA_INBAND, VMBUS_COMPLETION_REQUESTED);
}

static u32 handle_packet(void) {
    struct vmbus_packet p;
    int r = vmbus_read(channel, packet, PACKET_BYTES, &p);
    if (r == 0) return NOTHING;
    if (r < 0) return 1;
    last_offset = p.data_offset;
    if (p.type == VM_PKT_COMP) {
        u32 type = *(u32 *)(packet + p.data_offset);
        if (type == NVSP_SEND_RNDIS_COMPLETE && p.transaction >= 1 && p.transaction <= SEND_SLOTS)
            send_busy[p.transaction - 1] = 0;
        return type;
    }
    if (p.type == VM_PKT_DATA_USING_XFER_PAGES) {
        u32 ranges = *(u32 *)(packet + 20);
        for (u32 i = 0; i < ranges && 24 + 8 * (i + 1) <= p.total; ++i) {
            u32 bytes = *(u32 *)(packet + 24 + 8 * i), offset = *(u32 *)(packet + 28 + 8 * i);
            if (offset < RECEIVE_BYTES && bytes <= RECEIVE_BYTES - offset) receive_rndis(receive_buffer + offset, bytes);
        }
        u32 done[NVSP_MESSAGE / 4] = {NVSP_SEND_RNDIS_COMPLETE, NVSP_SUCCESS};
        vmbus_send(channel, done, sizeof(done), p.transaction, VM_PKT_COMP, 0);
    }
    return 1;
}

static int wait_nvsp(u32 type) {
    u64 end = dk->nanoseconds() + WAIT_NS;
    while (dk->nanoseconds() < end) {
        u32 got = handle_packet();
        if (got == type) return 0;
        if (got == NOTHING) __asm__ volatile("pause");
    }
    dk->log("HYPERV NET: timed out waiting for NVSP message %u", (unsigned)type);
    return -1;
}

static void set_link(int up) {
    if (up == link_up) return;
    link_up = up;
    if (handle >= 0) dk->net_link(handle, up, up ? 10000 : 0);
}

static void receive_rndis(const u8 *m, u32 available) {
    if (available < 8) return;
    u32 type = *(const u32 *)m, length = *(const u32 *)(m + 4);
    if (length > available) return;
    if (type == RNDIS_PACKET) {
        if (length < 44) return;
        u32 offset = *(const u32 *)(m + 8), bytes = *(const u32 *)(m + 12);
        if (handle < 0 || offset > length - 8 || bytes > length - 8 - offset) return;
        dk->net_receive(handle, m + 8 + offset, bytes);
    } else if (type == RNDIS_INDICATE) {
        if (length < 12) return;
        u32 status = *(const u32 *)(m + 8);
        if (status == RNDIS_STATUS_CONNECT) set_link(1);
        else if (status == RNDIS_STATUS_DISCONNECT) set_link(0);
    } else if (type & RNDIS_COMPLETE) {
        if (length >= 12 && *(const u32 *)(m + 8) == control_id) {
            u32 n = length < sizeof(control_reply) ? length : sizeof(control_reply);
            memcpy(control_reply, m, n);
            control_done = 1;
        }
    }
}

/* Send a control message of LENGTH bytes (in control_page) and wait for the reply. */
static int rndis_request(u32 length) {
    u32 message[NVSP_MESSAGE / 4] = {NVSP_SEND_RNDIS, 1, NO_SECTION, 0};
    control_done = 0;
    control_id = *(u32 *)(control_page + 8);
    if (vmbus_send_gpa(channel, control_page, length, message, sizeof(message), CONTROL_TRANSACTION)) return -1;
    u64 end = dk->nanoseconds() + WAIT_NS;
    while (!control_done && dk->nanoseconds() < end)
        if (handle_packet() == NOTHING) __asm__ volatile("pause");
    if (!control_done) dk->log("HYPERV NET: no reply to RNDIS message %x", (unsigned)*(u32 *)control_page);
    return control_done ? 0 : -1;
}

static int rndis_query(u32 id, u32 oid, void *out, u32 capacity) {
    u32 *m = (u32 *)control_page;
    memset(control_page, 0, 64);
    m[0] = RNDIS_QUERY;
    m[1] = 28;
    m[2] = id;
    m[3] = oid;
    m[4] = 0;
    m[5] = 20;
    if (rndis_request(28)) return -1;
    u32 status = *(u32 *)(control_reply + 12), bytes = *(u32 *)(control_reply + 16), offset = *(u32 *)(control_reply + 20);
    if (status || bytes > capacity || offset + bytes > sizeof(control_reply) - 8) return -1;
    memcpy(out, control_reply + 8 + offset, bytes);
    return 0;
}

static int transmit(void *instance, const void *frame, unsigned length) {
    (void)instance;
    int slot = -1;
    for (int i = 0; i < SEND_SLOTS && slot < 0; ++i)
        if (!send_busy[i]) slot = i;
    if (slot < 0 || length > HV_PAGE - 44) return -1;

    u8 *page = send_pages + (u32)slot * HV_PAGE;
    u32 *m = (u32 *)page;
    memset(page, 0, 44);
    m[0] = RNDIS_PACKET;
    m[1] = 44 + length;
    m[2] = 36;
    m[3] = length;
    memcpy(page + 44, frame, length);

    u32 message[NVSP_MESSAGE / 4] = {NVSP_SEND_RNDIS, 0, NO_SECTION, 0};
    send_busy[slot] = 1;
    if (vmbus_send_gpa(channel, page, 44 + length, message, sizeof(message), (u64)slot + 1)) {
        send_busy[slot] = 0;
        return -1;
    }
    return 0;
}

static const struct dk_net_ops ops = {transmit};

int hv_net_start(void) {
    static const u32 versions[] = {0x50000, 0x40000, 0x30002, 2};
    channel = vmbus_find(net_guid, 0);
    if (!channel) return -1;
    packet = dk->alloc(PACKET_BYTES);
    control_page = dk->dma_alloc(HV_PAGE, HV_PAGE);
    send_pages = dk->dma_alloc(HV_PAGE * SEND_SLOTS, HV_PAGE);
    receive_buffer = dk->dma_alloc(RECEIVE_BYTES, HV_PAGE);
    if (!packet || !control_page || !send_pages || !receive_buffer) return -1;
    if (vmbus_open(channel, RING_BYTES)) return -1;

    u32 version = 0;
    for (unsigned i = 0; i < sizeof(versions) / sizeof(*versions) && !version; ++i) {
        if (nvsp(NVSP_INIT, versions[i], versions[i], 0) || wait_nvsp(NVSP_INIT_COMPLETE)) return -1;
        if (*(u32 *)(packet + last_offset + 12) == NVSP_SUCCESS) version = versions[i];
    }
    if (!version) {
        dk->log("HYPERV NET: no NVSP version was accepted");
        return -1;
    }
    if (version >= 0x30002) {
        u32 message[NVSP_MESSAGE / 4] = {NVSP_SEND_NDIS_CONFIG, 1514, 0, 0};
        if (vmbus_send(channel, message, sizeof(message), 4, VM_PKT_DATA_INBAND, 0)) return -1;
    }
    {
        u32 message[NVSP_MESSAGE / 4] = {NVSP_SEND_NDIS_VERSION, 6, version > 0x40000 ? 30u : 1u};
        if (vmbus_send(channel, message, sizeof(message), 1, VM_PKT_DATA_INBAND, 0)) return -1;
    }
    if (vmbus_gpadl(channel, receive_buffer, RECEIVE_BYTES, &receive_gpadl)) return -1;
    {
        u32 message[NVSP_MESSAGE / 4] = {NVSP_SEND_RECEIVE_BUFFER, receive_gpadl, RECEIVE_BUFFER_ID};
        if (vmbus_send(channel, message, sizeof(message), 2, VM_PKT_DATA_INBAND, VMBUS_COMPLETION_REQUESTED)) return -1;
        if (wait_nvsp(NVSP_SEND_RECEIVE_BUFFER_COMPLETE)) return -1;
        if (*(u32 *)(packet + last_offset + 4) != NVSP_SUCCESS) {
            dk->log("HYPERV NET: the receive buffer was refused");
            return -1;
        }
    }

    u32 *m = (u32 *)control_page;
    memset(control_page, 0, 64);
    m[0] = RNDIS_INIT;
    m[1] = 24;
    m[2] = 1;
    m[3] = 1;
    m[4] = 0;
    m[5] = 0x4000;
    if (rndis_request(24) || *(u32 *)(control_reply + 12)) {
        dk->log("HYPERV NET: RNDIS did not initialize");
        return -1;
    }

    u8 mac[6];
    if (rndis_query(2, OID_MAC, mac, 6)) {
        dk->log("HYPERV NET: could not read the MAC address");
        return -1;
    }
    u32 connected = 1;
    if (rndis_query(3, OID_MEDIA_CONNECT, &connected, 4)) connected = 0;

    m = (u32 *)control_page;
    memset(control_page, 0, 64);
    m[0] = RNDIS_SET;
    m[1] = 32;
    m[2] = 4;
    m[3] = OID_PACKET_FILTER;
    m[4] = 4;
    m[5] = 20;
    m[7] = FILTER_DIRECTED | FILTER_BROADCAST | FILTER_ALL_MULTICAST;
    if (rndis_request(32) || *(u32 *)(control_reply + 12)) {
        dk->log("HYPERV NET: could not set the packet filter");
        return -1;
    }

    handle = dk->net_register(&ops, 0, mac, "Hyper-V Network");
    if (handle < 0) return -1;
    link_up = -1;
    set_link(connected == 0);
    dk->log("HYPERV NET: adapter ready, NVSP %x, MAC %02x:%02x:%02x:%02x:%02x:%02x", (unsigned)version, mac[0], mac[1],
            mac[2], mac[3], mac[4], mac[5]);
    return 0;
}

void hv_net_poll(void) {
    if (!channel || handle < 0) return;
    for (int budget = 256; budget && handle_packet() != NOTHING; --budget) {}
}

void hv_net_stop(void) {
    if (handle >= 0) {
        dk->net_unregister(handle);
        handle = -1;
    }
    if (!channel || !channel->open) return;
    u32 message[NVSP_MESSAGE / 4] = {NVSP_REVOKE_RECEIVE_BUFFER, RECEIVE_BUFFER_ID};
    vmbus_send(channel, message, sizeof(message), 3, VM_PKT_DATA_INBAND, 0);
    vmbus_gpadl_free(channel, receive_gpadl);
    vmbus_close(channel);
}
