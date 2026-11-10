/*
 * DOS-XC /runtime/net.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * Adapters, Ethernet, ARP, IPv4, ICMP echo and the DHCP client.  An
 * adapter comes from a driver (drivers.c); it starts with DHCP unless
 * \SYSTEM\NET.CFG sets a static address.  IPv4 fragments are not
 * reassembled and nothing above 1500 bytes is sent.
 */

#include "net.h"

#define NET_MAX_DEVICES 4
#define ARP_ENTRIES 32
#define LOOP_ENTRIES 64
#define BROADCAST 0xffffffffu
#define LOOPBACK(ip) (((ip) >> 24) == 127)

enum { ARP_FREE, ARP_PENDING, ARP_VALID };
enum { DHCP_OFF, DHCP_INIT, DHCP_SELECTING, DHCP_REQUESTING, DHCP_BOUND, DHCP_RENEWING };

struct net_device {
    int used, handle;
    struct driver *driver;
    const struct dk_net_ops *ops;
    void *instance;
    u8 mac[6];
    char name[16];
    int link;
    u32 speed;
    u32 ip, mask, gateway, dns[2];
    int dhcp;                          /* DHCP_* state; DHCP_OFF means static */
    u32 xid, server, offered, lease_seconds;
    u64 dhcp_next, lease_start;
    unsigned dhcp_tries;
    u64 rx_packets, tx_packets, rx_bytes, tx_bytes, rx_dropped;
};

struct arp_entry {
    int state;
    struct net_device *dev;
    u32 ip;
    u8 mac[6];
    u64 expires, retry_at;
    int tries;
    u8 packet[NET_FRAME];
    uptr packet_length;                /* frame waiting for the address, or 0 */
};

static struct net_device devices[NET_MAX_DEVICES];
static struct arp_entry arp_table[ARP_ENTRIES];
static struct { u8 packet[NET_FRAME - NET_ETH]; uptr length; } loop_queue[LOOP_ENTRIES];
static unsigned loop_head, loop_tail;
static u64 next_timer, last_tick;
static int polling;
static u16 ip_id;
static const u8 broadcast_mac[6] = {255, 255, 255, 255, 255, 255};

static struct { u16 id, sequence; u32 from; u64 sent, received; int waiting; } ping;

/* ---- helpers ------------------------------------------------------------ */

u16 net_checksum(const void *data, uptr length, u32 start) {
    const u8 *p = data;
    u32 sum = start;
    while (length >= 2) {
        sum += (u32)(p[0] << 8 | p[1]);
        p += 2;
        length -= 2;
    }
    if (length) sum += (u32)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (u16)~sum;
}

u32 net_pseudo(u32 source, u32 destination, u8 protocol, uptr length) {
    u32 sum = (source >> 16) + (source & 0xffff) + (destination >> 16) + (destination & 0xffff);
    return sum + protocol + (u32)length;
}

static void device_transmit(struct net_device *d, u8 *frame, uptr length) {
    if (length < 60) {                 /* adapters that pad get it right too */
        memset(frame + length, 0, 60 - length);
        length = 60;
    }
    if (driver_transmit(d->driver, d->ops, d->instance, frame, (u32)length)) { ++d->rx_dropped; return; }
    ++d->tx_packets;
    d->tx_bytes += length;
}

static void eth_header(u8 *frame, const struct net_device *d, const u8 *destination, u16 type) {
    memcpy(frame, destination, 6);
    memcpy(frame + 6, d->mac, 6);
    put16(frame + 12, type);
}

static int active(const struct net_device *d) { return d->used && d->link; }

int net_is_local(u32 ip) {
    if (LOOPBACK(ip)) return 1;
    for (int i = 0; i < NET_MAX_DEVICES; ++i)
        if (devices[i].used && devices[i].ip && devices[i].ip == ip) return 1;
    return 0;
}

static struct net_device *route_for(u32 destination, u32 *next_hop) {
    struct net_device *fallback = 0;
    for (int i = 0; i < NET_MAX_DEVICES; ++i) {
        struct net_device *d = &devices[i];
        if (!active(d) || !d->ip) continue;
        if ((destination & d->mask) == (d->ip & d->mask)) { *next_hop = destination; return d; }
        if (d->gateway && !fallback) fallback = d;
    }
    if (fallback) *next_hop = fallback->gateway;
    return fallback;
}

u32 net_source_for(u32 destination) {
    u32 hop;
    if (LOOPBACK(destination)) return 0x7f000001u;
    if (net_is_local(destination)) return destination;
    struct net_device *d = route_for(destination, &hop);
    return d ? d->ip : 0;
}

int net_ready(void) {
    for (int i = 0; i < NET_MAX_DEVICES; ++i)
        if (active(&devices[i]) && devices[i].ip) return 1;
    return 0;
}

int net_active(void) {
    for (int i = 0; i < NET_MAX_DEVICES; ++i)
        if (devices[i].used) return 1;
    for (int i = 0; i < SOCKET_MAX; ++i)
        if (sockets[i].used) return 1;
    return 0;
}

/* ---- ARP ------------------------------------------------------------------- */

static void arp_send(struct net_device *d, u16 operation, const u8 *target_mac, u32 target_ip) {
    u8 frame[64];
    eth_header(frame, d, operation == 1 ? broadcast_mac : target_mac, 0x0806);
    u8 *a = frame + NET_ETH;
    put16(a, 1);
    put16(a + 2, 0x0800);
    a[4] = 6;
    a[5] = 4;
    put16(a + 6, operation);
    memcpy(a + 8, d->mac, 6);
    put32(a + 14, d->ip);
    memcpy(a + 18, operation == 1 ? (const u8 *)"\0\0\0\0\0\0" : target_mac, 6);
    put32(a + 24, target_ip);
    device_transmit(d, frame, NET_ETH + 28);
}

static struct arp_entry *arp_find(const struct net_device *d, u32 ip) {
    for (int i = 0; i < ARP_ENTRIES; ++i)
        if (arp_table[i].state != ARP_FREE && arp_table[i].dev == d && arp_table[i].ip == ip) return &arp_table[i];
    return 0;
}

static struct arp_entry *arp_slot(void) {
    struct arp_entry *oldest = 0;
    for (int i = 0; i < ARP_ENTRIES; ++i) {
        if (arp_table[i].state == ARP_FREE) return &arp_table[i];
        if (arp_table[i].state == ARP_VALID && (!oldest || arp_table[i].expires < oldest->expires)) oldest = &arp_table[i];
    }
    return oldest;
}

static void arp_input(struct net_device *d, const u8 *a, uptr length) {
    if (length < 28 || get16(a) != 1 || get16(a + 2) != 0x0800 || a[4] != 6 || a[5] != 4) return;
    u32 sender = get32(a + 14), target = get32(a + 24);
    u16 operation = get16(a + 6);
    struct arp_entry *e = arp_find(d, sender);
    if (e) {
        memcpy(e->mac, a + 8, 6);
        e->state = ARP_VALID;
        e->expires = cpu_nanoseconds() + 600000000000ull;
        if (e->packet_length) {
            memcpy(e->packet, e->mac, 6);
            device_transmit(d, e->packet, e->packet_length);
            e->packet_length = 0;
        }
    }
    if (operation == 1 && d->ip && target == d->ip) {
        if (!e && sender && (e = arp_slot())) {
            e->state = ARP_VALID;
            e->dev = d;
            e->ip = sender;
            memcpy(e->mac, a + 8, 6);
            e->expires = cpu_nanoseconds() + 600000000000ull;
            e->packet_length = 0;
        }
        arp_send(d, 2, a + 8, sender);
    }
}

static void arp_resolve_and_send(struct net_device *d, u32 hop, u8 *frame, uptr length) {
    struct arp_entry *e = arp_find(d, hop);
    u64 now = cpu_nanoseconds();
    if (e && e->state == ARP_VALID && now < e->expires) {
        memcpy(frame, e->mac, 6);
        device_transmit(d, frame, length);
        return;
    }
    if (!e) {
        e = arp_slot();
        if (!e) { ++d->rx_dropped; return; }
        e->state = ARP_PENDING;
        e->dev = d;
        e->ip = hop;
        e->tries = 0;
        e->retry_at = 0;
    } else if (e->state == ARP_VALID) {
        e->state = ARP_PENDING;
        e->tries = 0;
        e->retry_at = 0;
    }
    memcpy(e->packet, frame, length);
    e->packet_length = length;
    if (now >= e->retry_at) {
        e->retry_at = now + 1000000000ull;
        ++e->tries;
        arp_send(d, 1, 0, hop);
    }
}

static void arp_timers(u64 now) {
    for (int i = 0; i < ARP_ENTRIES; ++i) {
        struct arp_entry *e = &arp_table[i];
        if (e->state != ARP_PENDING || now < e->retry_at) continue;
        if (e->tries >= 4) { e->state = ARP_FREE; e->packet_length = 0; continue; }
        e->retry_at = now + 1000000000ull;
        ++e->tries;
        arp_send(e->dev, 1, 0, e->ip);
    }
}

/* ---- IPv4 output ---------------------------------------------------------- */

int net_output(u8 *frame, uptr payload, u8 protocol, u32 source, u32 destination) {
    u8 *ip = frame + NET_ETH;
    uptr total = NET_IP + payload;
    if (total > 1500) return -DOSXC_EMSGSIZE;
    ip[0] = 0x45;
    ip[1] = 0;
    put16(ip + 2, (u16)total);
    put16(ip + 4, ++ip_id);
    put16(ip + 6, 0x4000);             /* don't fragment */
    ip[8] = 64;
    ip[9] = protocol;
    put16(ip + 10, 0);
    put32(ip + 12, source);
    put32(ip + 16, destination);
    put16(ip + 10, net_checksum(ip, NET_IP, 0));
    if (net_is_local(destination)) {
        unsigned next = (loop_tail + 1) % LOOP_ENTRIES;
        if (next == loop_head) return -DOSXC_ENOBUFS;
        memcpy(loop_queue[loop_tail].packet, ip, total);
        loop_queue[loop_tail].length = total;
        loop_tail = next;
        return 0;
    }
    if (destination == BROADCAST) {
        for (int i = 0; i < NET_MAX_DEVICES; ++i) {
            if (!active(&devices[i])) continue;
            eth_header(frame, &devices[i], broadcast_mac, 0x0800);
            device_transmit(&devices[i], frame, NET_ETH + total);
            return 0;
        }
        return -DOSXC_ENETDOWN;
    }
    u32 hop = 0;
    struct net_device *d = route_for(destination, &hop);
    if (!d) return net_ready() ? -DOSXC_ENETUNREACH : -DOSXC_ENETDOWN;
    if ((destination | d->mask) == BROADCAST) {
        eth_header(frame, d, broadcast_mac, 0x0800);
        device_transmit(d, frame, NET_ETH + total);
        return 0;
    }
    eth_header(frame, d, broadcast_mac, 0x0800);
    arp_resolve_and_send(d, hop, frame, NET_ETH + total);
    return 0;
}

/* ---- ICMP ------------------------------------------------------------------ */

static void icmp_input(u32 source, u32 destination, const u8 *p, uptr length) {
    if (length < 8 || net_checksum(p, length, 0)) return;
    if (p[0] == 8 && p[1] == 0 && length <= NET_FRAME - NET_L4) {
        u8 frame[NET_FRAME + 16];
        u8 *reply = frame + NET_L4;
        memcpy(reply, p, length);
        reply[0] = 0;
        put16(reply + 2, 0);
        put16(reply + 2, net_checksum(reply, length, 0));
        net_output(frame, length, PROTO_ICMP, destination, source);
    } else if (p[0] == 0 && ping.waiting && get16(p + 4) == ping.id && get16(p + 6) == ping.sequence) {
        ping.from = source;
        ping.received = cpu_nanoseconds();
        ping.waiting = 0;
    }
}

static i64 ping_host(u32 address, unsigned timeout_ms) {
    if (!address) return -DOSXC_EINVAL;
    u32 source = net_source_for(address);
    if (!source) return -DOSXC_ENETUNREACH;
    u8 frame[NET_FRAME + 16];
    u8 *p = frame + NET_L4;
    memset(p, 0, 64);
    p[0] = 8;
    ping.id = (u16)(cpu_nanoseconds() >> 10);
    ping.sequence = (u16)(ping.sequence + 1);
    put16(p + 4, ping.id);
    put16(p + 6, ping.sequence);
    for (int i = 8; i < 64; ++i) p[i] = (u8)i;
    put16(p + 2, net_checksum(p, 64, 0));
    ping.waiting = 1;
    ping.sent = cpu_nanoseconds();
    int r = net_output(frame, 64, PROTO_ICMP, source, address);
    if (r) { ping.waiting = 0; return r; }
    u64 deadline = ping.sent + (u64)timeout_ms * 1000000ull;
    while (ping.waiting) {
        net_poll();
        if (cpu_nanoseconds() >= deadline) { ping.waiting = 0; return -DOSXC_ETIMEDOUT; }
        if (socket_wait_interrupted()) { ping.waiting = 0; return -DOSXC_EINTR; }
    }
    return (i64)((ping.received - ping.sent) / 1000);
}

/* ---- DHCP ------------------------------------------------------------------ */

#define DHCP_CLIENT 68
#define DHCP_SERVER 67
#define BOOTP_SIZE 240

static void dhcp_send(struct net_device *d, u8 type, u32 requested, u32 server, u32 from_ip) {
    u8 frame[NET_FRAME + 16];
    memset(frame, 0, sizeof(frame));
    u8 *udp = frame + NET_L4;
    u8 *b = udp + 8;
    b[0] = 1; b[1] = 1; b[2] = 6;
    put32(b + 4, d->xid);
    put16(b + 8, (u16)(d->dhcp_tries));
    if (from_ip) put32(b + 12, from_ip);
    memcpy(b + 28, d->mac, 6);
    put32(b + 236, 0x63825363u);
    u8 *o = b + BOOTP_SIZE;
    *o++ = 53; *o++ = 1; *o++ = type;
    if (requested && !from_ip) { *o++ = 50; *o++ = 4; put32(o, requested); o += 4; }
    if (server && !from_ip) { *o++ = 54; *o++ = 4; put32(o, server); o += 4; }
    *o++ = 12; *o++ = 6; memcpy(o, "dos-xc", 6); o += 6;
    *o++ = 55; *o++ = 5; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 51; *o++ = 54;
    *o++ = 255;
    uptr payload = (uptr)(o - b);
    if (payload < 300) payload = 300;
    uptr udp_length = 8 + payload;
    put16(udp, DHCP_CLIENT);
    put16(udp + 2, DHCP_SERVER);
    put16(udp + 4, (u16)udp_length);
    put16(udp + 6, 0);
    u16 sum = net_checksum(udp, udp_length, net_pseudo(from_ip, BROADCAST, PROTO_UDP, udp_length));
    put16(udp + 6, sum ? sum : 0xffff);
    /* Broadcast by hand: there is no source address yet. */
    u8 *ip = frame + NET_ETH;
    uptr total = NET_IP + udp_length;
    ip[0] = 0x45;
    put16(ip + 2, (u16)total);
    put16(ip + 4, ++ip_id);
    ip[8] = 64;
    ip[9] = PROTO_UDP;
    put32(ip + 12, from_ip);
    put32(ip + 16, BROADCAST);
    put16(ip + 10, net_checksum(ip, NET_IP, 0));
    eth_header(frame, d, broadcast_mac, 0x0800);
    device_transmit(d, frame, NET_ETH + total);
}

static void dhcp_start(struct net_device *d) {
    d->dhcp = DHCP_INIT;
    d->xid = (u32)(cpu_nanoseconds() ^ ((u32)d->mac[5] << 24 | (u32)d->mac[4] << 16 | (u32)d->mac[3] << 8 | d->mac[2]));
    d->ip = d->mask = d->gateway = d->dns[0] = d->dns[1] = 0;
    d->dhcp_tries = 0;
    d->dhcp_next = 0;
}

static void text_ip(char *out, u32 ip) {
    uptr n = 0;
    for (int shift = 24; shift >= 0; shift -= 8) {
        unsigned v = (ip >> shift) & 255, divisor = 100;
        int started = 0;
        for (; divisor; divisor /= 10) {
            unsigned digit = (v / divisor) % 10;
            if (digit || started || divisor == 1) { out[n++] = (char)('0' + digit); started = 1; }
        }
        if (shift) out[n++] = '.';
    }
    out[n] = 0;
}

static void announce(const struct net_device *d) {
    char text[80], ip[16];
    text_ip(ip, d->ip);
    uptr n = rt_strlen(d->name);
    memcpy(text, d->name, n);
    rt_strcpy(text + n, ": network ready, address ");
    rt_strcpy(text + rt_strlen(text), ip);
    volume_post_notice(text);
}

static void dhcp_input(struct net_device *d, const u8 *b, uptr length) {
    if (length < BOOTP_SIZE || b[0] != 2 || get32(b + 4) != d->xid || get32(b + 236) != 0x63825363u) return;
    if (memcmp(b + 28, d->mac, 6) || d->dhcp == DHCP_OFF) return;
    u8 type = 0;
    u32 mask = 0, router = 0, lease = 3600, server = 0, dns[2] = {0, 0};
    for (uptr at = BOOTP_SIZE; at < length;) {
        u8 code = b[at++];
        if (code == 255) break;
        if (code == 0) continue;
        if (at >= length || at + 1 + b[at] > length) break;
        u8 n = b[at++];
        const u8 *v = b + at;
        at += n;
        if (code == 53 && n >= 1) type = v[0];
        if (code == 1 && n >= 4) mask = get32(v);
        if (code == 3 && n >= 4) router = get32(v);
        if (code == 6 && n >= 4) { dns[0] = get32(v); if (n >= 8) dns[1] = get32(v + 4); }
        if (code == 51 && n >= 4) lease = get32(v);
        if (code == 54 && n >= 4) server = get32(v);
    }
    u32 yours = get32(b + 16);
    if (type == 2 && d->dhcp == DHCP_SELECTING) {
        d->offered = yours;
        d->server = server;
        d->dhcp = DHCP_REQUESTING;
        d->dhcp_tries = 0;
        d->dhcp_next = 0;
    } else if (type == 6 && (d->dhcp == DHCP_REQUESTING || d->dhcp == DHCP_RENEWING)) {
        dhcp_start(d);                 /* NAK: start over */
    } else if (type == 5 && (d->dhcp == DHCP_REQUESTING || d->dhcp == DHCP_RENEWING)) {
        int fresh = d->ip != yours;
        d->ip = yours;
        d->mask = mask ? mask : 0xffffff00u;
        d->gateway = router;
        d->dns[0] = dns[0];
        d->dns[1] = dns[1];
        d->server = server ? server : d->server;
        d->lease_seconds = lease;
        d->lease_start = cpu_nanoseconds();
        d->dhcp = DHCP_BOUND;
        d->dhcp_next = d->lease_start + (u64)(lease / 2) * 1000000000ull;
        if (fresh) announce(d);
    }
}

static void dhcp_timer(struct net_device *d, u64 now) {
    if (d->dhcp == DHCP_OFF || !d->link || now < d->dhcp_next) return;
    u64 wait = (u64)(d->dhcp_tries < 4 ? 1u << d->dhcp_tries : 30u) * 1000000000ull;
    switch (d->dhcp) {
    case DHCP_INIT:
        d->dhcp = DHCP_SELECTING;
        /* fall through */
    case DHCP_SELECTING:
        dhcp_send(d, 1, 0, 0, 0);
        break;
    case DHCP_REQUESTING:
        if (d->dhcp_tries >= 5) { dhcp_start(d); return; }
        dhcp_send(d, 3, d->offered, d->server, 0);
        break;
    case DHCP_BOUND:
        d->dhcp = DHCP_RENEWING;
        d->dhcp_tries = 0;
        wait = 0;
        /* fall through */
    case DHCP_RENEWING: {
        u64 used = (now - d->lease_start) / 1000000000ull;
        if (used >= d->lease_seconds) { dhcp_start(d); return; }
        dhcp_send(d, 3, 0, 0, d->ip);
        wait = 30000000000ull;
        break;
    }
    }
    ++d->dhcp_tries;
    d->dhcp_next = now + wait;
}

/* ---- input ---------------------------------------------------------------- */

static void udp_or_dhcp(struct net_device *d, u32 source, u32 destination, const u8 *p, uptr length) {
    if (length < 8) return;
    uptr udp_length = get16(p + 4);
    if (udp_length < 8 || udp_length > length) return;
    if (get16(p + 6) && net_checksum(p, udp_length, net_pseudo(source, destination, PROTO_UDP, udp_length))) return;
    if (d && get16(p + 2) == DHCP_CLIENT) { dhcp_input(d, p + 8, udp_length - 8); return; }
    udp_input(source, destination, p, udp_length);
}

static void ip_input(struct net_device *d, const u8 *ip, uptr length) {
    if (length < NET_IP || (ip[0] >> 4) != 4) return;
    uptr header = (uptr)(ip[0] & 15) * 4, total = get16(ip + 2);
    if (header < NET_IP || total < header || total > length || net_checksum(ip, header, 0)) return;
    if (get16(ip + 6) & 0x3fff) return;   /* fragments are not reassembled */
    u32 source = get32(ip + 12), destination = get32(ip + 16);
    int mine = destination == BROADCAST || net_is_local(destination);
    if (d && !mine && d->ip && (destination | d->mask) == BROADCAST) mine = 1;
    if (d && !mine && !d->ip && ip[9] == PROTO_UDP) mine = 1;  /* a DHCP answer before we have an address */
    if (!mine) return;
    const u8 *payload = ip + header;
    uptr size = total - header;
    switch (ip[9]) {
    case PROTO_ICMP: icmp_input(source, destination, payload, size); break;
    case PROTO_UDP: udp_or_dhcp(d, source, destination, payload, size); break;
    case PROTO_TCP: tcp_input(source, destination, payload, size); break;
    }
}

/* Called by a driver's poll(), once per frame. */
void net_receive(int handle, const void *frame, u32 length) {
    if (handle < 0 || handle >= NET_MAX_DEVICES || !devices[handle].used || length < NET_ETH) return;
    struct net_device *d = &devices[handle];
    const u8 *f = frame;
    ++d->rx_packets;
    d->rx_bytes += length;
    switch (get16(f + 12)) {
    case 0x0806: arp_input(d, f + NET_ETH, length - NET_ETH); break;
    case 0x0800: ip_input(d, f + NET_ETH, length - NET_ETH); break;
    }
}

static void loop_drain(void) {
    while (loop_head != loop_tail) {
        unsigned at = loop_head;
        loop_head = (loop_head + 1) % LOOP_ENTRIES;
        ip_input(0, loop_queue[at].packet, loop_queue[at].length);
    }
}

void net_poll(void) {
    if (polling) return;
    polling = 1;
    drivers_poll();
    loop_drain();
    u64 now = cpu_nanoseconds();
    if (now >= next_timer) {
        next_timer = now + 10000000ull;
        arp_timers(now);
        for (int i = 0; i < NET_MAX_DEVICES; ++i)
            if (devices[i].used) dhcp_timer(&devices[i], now);
        tcp_timers(now);
        loop_drain();
    }
    polling = 0;
}

void net_tick(void) {
    u64 now = cpu_nanoseconds();
    if (now - last_tick < 250000ull) return;
    last_tick = now;
    if (net_active()) net_poll();
    else drivers_poll();                 /* input drivers need polling with no network in use */
}

/* ---- adapters -------------------------------------------------------------- */

static u32 parse_ip(const char *s) {
    u32 ip = 0;
    for (int part = 0; part < 4; ++part) {
        unsigned v = 0, digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 3) { v = v * 10 + (unsigned)(*s++ - '0'); ++digits; }
        if (!digits || v > 255) return 0;
        ip = ip << 8 | v;
        if (part < 3 && *s++ != '.') return 0;
    }
    return ip;
}

/* \SYSTEM\NET.CFG: IP=, MASK=, GATEWAY=, DNS= (first adapter only). */
static int read_config(struct net_device *d) {
    char path[32], text[512];
    path[0] = (char)rt_upper(ctx->drive);
    rt_strcpy(path + 1, ":\\SYSTEM\\NET.CFG");
    struct fat32_info info;
    if (fs_stat(path, &info) || !info.size) return 0;
    uptr size = info.size < sizeof(text) - 1 ? info.size : sizeof(text) - 1;
    if (fs_read_at(path, 0, text, (u32)size)) return 0;
    text[size] = 0;
    int found = 0;
    for (char *line = text; *line;) {
        char *end = line;
        while (*end && *end != '\r' && *end != '\n') ++end;
        char saved = *end;
        *end = 0;
        char *equal = line;
        while (*equal && *equal != '=') ++equal;
        if (*equal) {
            *equal = 0;
            u32 value = parse_ip(equal + 1);
            if (rt_equal_nocase(line, "IP")) { d->ip = value; found = value != 0; }
            else if (rt_equal_nocase(line, "MASK")) d->mask = value;
            else if (rt_equal_nocase(line, "GATEWAY")) d->gateway = value;
            else if (rt_equal_nocase(line, "DNS")) d->dns[d->dns[0] ? 1 : 0] = value;
        }
        *end = saved;
        line = end;
        while (*line == '\r' || *line == '\n') ++line;
    }
    return found;
}

int net_register(const struct dk_net_ops *ops, void *instance, const u8 mac[6], const char *name) {
    for (int i = 0; i < NET_MAX_DEVICES; ++i) {
        struct net_device *d = &devices[i];
        if (d->used) continue;
        memset(d, 0, sizeof(*d));
        d->used = 1;
        d->handle = i;
        d->driver = driver_probing();
        d->ops = ops;
        d->instance = instance;
        memcpy(d->mac, mac, 6);
        uptr n = rt_strlen(name);
        if (n >= sizeof(d->name)) n = sizeof(d->name) - 1;
        memcpy(d->name, name, n);
        d->dhcp = DHCP_INIT;
        if (i == 0 && read_config(d)) {
            d->dhcp = DHCP_OFF;
            if (!d->mask) d->mask = 0xffffff00u;
        } else {
            dhcp_start(d);
        }
        return i;
    }
    return -DOSXC_ENOMEM;
}

void net_link(int handle, int up, u32 megabits) {
    if (handle < 0 || handle >= NET_MAX_DEVICES || !devices[handle].used) return;
    struct net_device *d = &devices[handle];
    d->speed = megabits;
    if (!!up == d->link) return;
    d->link = !!up;
    if (up && d->dhcp != DHCP_OFF) dhcp_start(d);
    if (up && d->dhcp == DHCP_OFF && d->ip) announce(d);
}

void net_unregister(int handle) {
    if (handle < 0 || handle >= NET_MAX_DEVICES) return;
    struct net_device *d = &devices[handle];
    for (int i = 0; i < ARP_ENTRIES; ++i)
        if (arp_table[i].state != ARP_FREE && arp_table[i].dev == d) arp_table[i].state = ARP_FREE;
    d->used = 0;
}

i64 net_syscall_net(int operation, long a, void *b) {
    struct net_device *d = a >= 0 && a < NET_MAX_DEVICES && devices[a].used ? &devices[a] : 0;
    switch (operation) {
    case DOSXC_NET_INFO: {
        int count = 0;
        for (int i = 0; i < NET_MAX_DEVICES; ++i) count += devices[i].used != 0;
        if (!b) return count;
        if (!d) return -DOSXC_ENODEV;
        net_poll();
        struct dosxc_netinfo *o = b;
        memset(o, 0, sizeof(*o));
        rt_strcpy(o->name, d->name);
        memcpy(o->mac, d->mac, 6);
        o->link = (u8)d->link;
        o->dhcp = d->dhcp != DHCP_OFF;
        o->speed = d->speed;
        o->ip = d->ip;
        o->mask = d->mask;
        o->gateway = d->gateway;
        o->dns[0] = d->dns[0];
        o->dns[1] = d->dns[1];
        if (d->dhcp == DHCP_BOUND || d->dhcp == DHCP_RENEWING) {
            u64 used = (cpu_nanoseconds() - d->lease_start) / 1000000000ull;
            o->lease = used < d->lease_seconds ? (u32)(d->lease_seconds - used) : 0;
        }
        o->rx_packets = d->rx_packets;
        o->tx_packets = d->tx_packets;
        o->rx_bytes = d->rx_bytes;
        o->tx_bytes = d->tx_bytes;
        o->rx_dropped = d->rx_dropped;
        return count;
    }
    case DOSXC_NET_CONFIG: {
        const struct dosxc_netconfig *c = b;
        if (!d || !c) return -DOSXC_ENODEV;
        if (c->dhcp) { dhcp_start(d); return 0; }
        d->dhcp = DHCP_OFF;
        d->ip = c->ip;
        d->mask = c->mask;
        d->gateway = c->gateway;
        d->dns[0] = c->dns[0];
        d->dns[1] = c->dns[1];
        return 0;
    }
    case DOSXC_NET_RENEW:
        if (!d) return -DOSXC_ENODEV;
        if (d->dhcp == DHCP_OFF) return -DOSXC_EINVAL;
        dhcp_start(d);
        return 0;
    case DOSXC_NET_PING: return ping_host((u32)a, b ? (unsigned)(uptr)b : 2000);
    }
    return -DOSXC_EINVAL;
}

void net_init(void) {
    memset(devices, 0, sizeof(devices));
    memset(arp_table, 0, sizeof(arp_table));
    loop_head = loop_tail = 0;
    next_timer = 0;
}
