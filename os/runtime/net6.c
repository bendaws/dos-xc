/*
 * DOS-XC /os/runtime/net6.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "net.h"

#define NEIGHBOURS 32
#define PROTO_ICMP6 58
#define SECOND 1000000000ull

enum { N_FREE, N_PENDING, N_VALID };
enum { ADDRESS_NONE, ADDRESS_TENTATIVE, ADDRESS_READY };

struct v6_device {
    int used, link;
    u8 mac[6];
    net_addr link_local, global, router, dns[2];
    int global_state, solicits;
    u8 identifier[8];
    u64 global_expires, router_expires, dad_at, next_solicit;
};

struct neighbour {
    int state, device, tries;
    net_addr ip;
    u8 mac[6];
    u64 expires, retry_at;
    u8 packet[NET_FRAME];
    uptr packet_length;                /* frame waiting for the address, or 0 */
};

static struct v6_device v6[NET_MAX_DEVICES];
static struct neighbour neighbours[NEIGHBOURS];
static struct { u16 id, sequence; u64 sent, received; int waiting; } ping6;
static const net_addr loopback = {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}};
static const net_addr all_nodes = {{0xff, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}};
static const net_addr all_routers = {{0xff, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2}};
static const net_addr unspecified;

static int is_link_local(const net_addr *a) { return a->b[0] == 0xfe && (a->b[1] & 0xc0) == 0x80; }
static int is_multicast(const net_addr *a) { return a->b[0] == 0xff; }

static int is_ours(const net_addr *a) {
    if (net_addr_equal(a, &loopback)) return 1;
    for (int i = 0; i < NET_MAX_DEVICES; ++i) {
        const struct v6_device *v = &v6[i];
        if (!v->used) continue;
        if (net_addr_equal(a, &v->link_local)) return 1;
        if (v->global_state == ADDRESS_READY && net_addr_equal(a, &v->global)) return 1;
    }
    return 0;
}

/* ---- addresses of either family ------------------------------------------- */

int net_is_local_any(const net_addr *a) {
    return net_addr_is_v4(a) ? net_is_local(net_addr_get_v4(a)) : is_ours(a);
}

u32 net_pseudo_any(const net_addr *source, const net_addr *destination, u8 protocol, uptr length) {
    if (net_addr_is_v4(source) && net_addr_is_v4(destination))
        return net_pseudo(net_addr_get_v4(source), net_addr_get_v4(destination), protocol, length);
    u32 sum = protocol + (u32)(length >> 16) + (u32)(length & 0xffff);
    for (int i = 0; i < 16; i += 2) sum += get16(source->b + i) + get16(destination->b + i);
    return sum;
}

static int route(const net_addr *destination, net_addr *via) {
    u64 now = cpu_nanoseconds();
    int fallback = -1;
    for (int i = 0; i < NET_MAX_DEVICES; ++i) {
        const struct v6_device *v = &v6[i];
        if (!v->used || !v->link) continue;
        if (is_link_local(destination) || is_multicast(destination)) { *via = *destination; return i; }
        if (v->global_state != ADDRESS_READY) continue;
        if (!memcmp(destination->b, v->global.b, 8)) { *via = *destination; return i; }
        if (fallback < 0 && v->router_expires > now) fallback = i;
    }
    if (fallback >= 0) *via = v6[fallback].router;
    return fallback;
}

int net_source(const net_addr *destination, net_addr *source) {
    if (net_addr_is_v4(destination)) {
        u32 ip = net_source_for(net_addr_get_v4(destination));
        if (!ip) return 0;
        *source = net_addr_v4(ip);
        return 1;
    }
    if (is_ours(destination)) { *source = *destination; return 1; }
    net_addr via;
    int h = route(destination, &via);
    if (h < 0) return 0;
    *source = is_link_local(destination) || is_multicast(destination) ? v6[h].link_local : v6[h].global;
    return 1;
}

/* ---- output --------------------------------------------------------------- */

static void make_header(u8 *frame, uptr payload, u8 next, const net_addr *source, const net_addr *destination, u8 hops) {
    memmove(frame + NET_ETH + NET_IP6, frame + NET_L4, payload);
    u8 *ip = frame + NET_ETH;
    ip[0] = 0x60;
    ip[1] = ip[2] = ip[3] = 0;
    put16(ip + 4, (u32)payload);
    ip[6] = next;
    ip[7] = hops;
    memcpy(ip + 8, source->b, 16);
    memcpy(ip + 24, destination->b, 16);
}

static void solicited_node(net_addr *out, const net_addr *target) {
    memset(out, 0, sizeof(*out));
    out->b[0] = 0xff;
    out->b[1] = 2;
    out->b[11] = 1;
    out->b[12] = 0xff;
    memcpy(out->b + 13, target->b + 13, 3);
}

/* Frames are built with the ICMPv6 message at frame + NET_L4, like everything
   else that goes through make_header. */
static void icmp6_checksum(u8 *message, uptr size, const net_addr *source, const net_addr *destination) {
    put16(message + 2, 0);
    put16(message + 2, net_checksum(message, size, net_pseudo_any(source, destination, PROTO_ICMP6, size)));
}

static void send_multicast(int h, u8 *frame, uptr payload, const net_addr *source, const net_addr *destination) {
    make_header(frame, payload, PROTO_ICMP6, source, destination, 255);
    frame[0] = frame[1] = 0x33;
    memcpy(frame + 2, destination->b + 12, 4);
    memcpy(frame + 6, v6[h].mac, 6);
    put16(frame + 12, 0x86dd);
    net_device_send(h, frame, NET_ETH + NET_IP6 + payload);
}

static void send_solicitation(int h, const net_addr *target, int duplicate_check) {
    u8 frame[NET_FRAME + 16];
    u8 *m = frame + NET_L4;
    uptr size = duplicate_check ? 24 : 32;
    const net_addr *source = duplicate_check ? &unspecified : &v6[h].link_local;
    net_addr destination;
    solicited_node(&destination, target);
    memset(m, 0, 32);
    m[0] = 135;
    memcpy(m + 8, target->b, 16);
    m[24] = 1;
    m[25] = 1;
    memcpy(m + 26, v6[h].mac, 6);
    icmp6_checksum(m, size, source, &destination);
    send_multicast(h, frame, size, source, &destination);
}

static void send_router_solicitation(int h) {
    u8 frame[NET_FRAME + 16];
    u8 *m = frame + NET_L4;
    memset(m, 0, 16);
    m[0] = 133;
    m[8] = 1;
    m[9] = 1;
    memcpy(m + 10, v6[h].mac, 6);
    icmp6_checksum(m, 16, &v6[h].link_local, &all_routers);
    send_multicast(h, frame, 16, &v6[h].link_local, &all_routers);
}

static struct neighbour *neighbour_find(int h, const net_addr *ip) {
    for (int i = 0; i < NEIGHBOURS; ++i)
        if (neighbours[i].state != N_FREE && neighbours[i].device == h && net_addr_equal(&neighbours[i].ip, ip)) return &neighbours[i];
    return 0;
}

static struct neighbour *neighbour_slot(void) {
    struct neighbour *oldest = 0;
    for (int i = 0; i < NEIGHBOURS; ++i) {
        if (neighbours[i].state == N_FREE) return &neighbours[i];
        if (neighbours[i].state == N_VALID && (!oldest || neighbours[i].expires < oldest->expires)) oldest = &neighbours[i];
    }
    return oldest;
}

static void learn(int h, const net_addr *ip, const u8 *mac, int create) {
    struct neighbour *n = neighbour_find(h, ip);
    if (!n) {
        if (!create || !(n = neighbour_slot())) return;
        n->device = h;
        n->ip = *ip;
        n->packet_length = 0;
    }
    memcpy(n->mac, mac, 6);
    n->state = N_VALID;
    n->expires = cpu_nanoseconds() + 600 * SECOND;
    if (n->packet_length) {
        memcpy(n->packet, mac, 6);
        net_device_send(h, n->packet, n->packet_length);
        n->packet_length = 0;
    }
}

static void resolve_and_send(int h, const net_addr *hop, u8 *frame, uptr length) {
    struct neighbour *n = neighbour_find(h, hop);
    u64 now = cpu_nanoseconds();
    if (n && n->state == N_VALID && now < n->expires) {
        memcpy(frame, n->mac, 6);
        net_device_send(h, frame, length);
        return;
    }
    if (!n) {
        if (!(n = neighbour_slot())) return;
        n->device = h;
        n->ip = *hop;
        n->retry_at = 0;
        n->tries = 0;
    } else if (n->state == N_VALID) {
        n->retry_at = 0;
        n->tries = 0;
    }
    n->state = N_PENDING;
    memcpy(n->packet, frame, length);
    n->packet_length = length;
    if (now >= n->retry_at) {
        n->retry_at = now + SECOND;
        ++n->tries;
        send_solicitation(h, hop, 0);
    }
}

/* The IPv6 counterpart of net_output().  The payload starts at frame + NET_L4
   and is moved up to make room for the longer header. */
static int net6_output(u8 *frame, uptr payload, u8 protocol, const net_addr *source, const net_addr *destination) {
    if (NET_IP6 + payload > 1500) return -DOSXC_EMSGSIZE;
    if (is_ours(destination)) {
        make_header(frame, payload, protocol, source, destination, 64);
        return net_loop_queue(frame + NET_ETH, NET_IP6 + payload);
    }
    net_addr via;
    int h = route(destination, &via);
    if (h < 0) return -DOSXC_ENETUNREACH;
    make_header(frame, payload, protocol, source, destination, 64);
    memcpy(frame + 6, v6[h].mac, 6);
    put16(frame + 12, 0x86dd);
    uptr length = NET_ETH + NET_IP6 + payload;
    if (is_multicast(destination)) {
        frame[0] = frame[1] = 0x33;
        memcpy(frame + 2, destination->b + 12, 4);
        net_device_send(h, frame, length);
    } else {
        resolve_and_send(h, &via, frame, length);
    }
    return 0;
}

int net_send(u8 *frame, uptr payload, u8 protocol, const net_addr *source, const net_addr *destination) {
    if (net_addr_is_v4(destination))
        return net_output(frame, payload, protocol, net_addr_get_v4(source), net_addr_get_v4(destination));
    return net6_output(frame, payload, protocol, source, destination);
}

/* ---- neighbour discovery input --------------------------------------------- */

static const u8 *find_option(const u8 *m, uptr size, uptr start, u8 type) {
    for (uptr at = start; at + 2 <= size && m[at + 1];) {
        uptr length = (uptr)m[at + 1] * 8;
        if (at + length > size) return 0;
        if (m[at] == type) return m + at;
        at += length;
    }
    return 0;
}

static void assign_global(int h, const u8 *prefix, u64 expires) {
    struct v6_device *v = &v6[h];
    memcpy(v->global.b, prefix, 8);
    memcpy(v->global.b + 8, v->identifier, 8);
    v->global_state = ADDRESS_TENTATIVE;
    v->global_expires = expires;
    v->dad_at = cpu_nanoseconds() + SECOND;
    send_solicitation(h, &v->global, 1);
}

static void router_advertisement(int h, const net_addr *source, const u8 *m, uptr size) {
    struct v6_device *v = &v6[h];
    if (size < 16 || !is_link_local(source)) return;
    u64 now = cpu_nanoseconds();
    u32 lifetime = get16(m + 6);
    if (lifetime) { v->router = *source; v->router_expires = now + lifetime * SECOND; }
    else if (net_addr_equal(&v->router, source)) v->router_expires = 0;
    const u8 *link = find_option(m, size, 16, 1);
    if (link && link[1] == 1) learn(h, source, link + 2, 1);
    for (uptr at = 16; at + 2 <= size && m[at + 1];) {
        const u8 *o = m + at;
        uptr length = (uptr)o[1] * 8;
        if (at + length > size) break;
        at += length;
        if (o[0] == 3 && o[1] == 4 && o[2] == 64 && (o[3] & 0x40)) {   /* prefix, autonomous */
            u32 valid = get32(o + 4);
            if (!valid || (o[16] == 0xfe && (o[17] & 0xc0) == 0x80)) continue;
            u64 expires = valid == 0xffffffffu ? ~0ull : now + valid * SECOND;
            if (v->global_state == ADDRESS_NONE) assign_global(h, o + 16, expires);
            else if (!memcmp(v->global.b, o + 16, 8)) v->global_expires = expires;
        } else if (o[0] == 25 && o[1] >= 3) {
            memcpy(v->dns[0].b, o + 8, 16);
            if (o[1] >= 5) memcpy(v->dns[1].b, o + 24, 16);
        }
    }
}

static void neighbour_solicitation(int h, const net_addr *source, const net_addr *destination, const u8 *m, uptr size) {
    struct v6_device *v = &v6[h];
    if (size < 24) return;
    net_addr target;
    memcpy(target.b, m + 8, 16);
    int tentative = v->global_state == ADDRESS_TENTATIVE && net_addr_equal(&target, &v->global);
    int unsolicited_source = net_addr_equal(source, &unspecified);
    if (tentative) {
        if (unsolicited_source) v->global_state = ADDRESS_NONE;
        return;
    }
    if (!net_addr_equal(&target, &v->link_local) &&
        !(v->global_state == ADDRESS_READY && net_addr_equal(&target, &v->global))) return;
    if (!unsolicited_source) {
        const u8 *link = find_option(m, size, 24, 1);
        if (link && link[1] == 1) learn(h, source, link + 2, 1);
    }
    (void)destination;
    u8 frame[NET_FRAME + 16];
    u8 *a = frame + NET_L4;
    memset(a, 0, 32);
    a[0] = 136;
    a[4] = unsolicited_source ? 0x20 : 0x60;      /* override, and solicited */
    memcpy(a + 8, target.b, 16);
    a[24] = 2;
    a[25] = 1;
    memcpy(a + 26, v->mac, 6);
    const net_addr *to = unsolicited_source ? &all_nodes : source;
    icmp6_checksum(a, 32, &target, to);
    if (is_multicast(to)) { send_multicast(h, frame, 32, &target, to); return; }
    make_header(frame, 32, PROTO_ICMP6, &target, to, 255);
    memcpy(frame + 6, v->mac, 6);
    put16(frame + 12, 0x86dd);
    resolve_and_send(h, to, frame, NET_ETH + NET_IP6 + 32);
}

static void neighbour_advertisement(int h, const u8 *m, uptr size) {
    struct v6_device *v = &v6[h];
    if (size < 24) return;
    net_addr target;
    memcpy(target.b, m + 8, 16);
    if (v->global_state == ADDRESS_TENTATIVE && net_addr_equal(&target, &v->global)) {
        v->global_state = ADDRESS_NONE;
        return;
    }
    const u8 *link = find_option(m, size, 24, 2);
    if (link && link[1] == 1) learn(h, &target, link + 2, 0);
}

/* ---- ICMPv6 --------------------------------------------------------------- */

static void packet_too_big(const u8 *m, uptr size) {
    if (size < 8 + NET_IP6 + 4 || m[8 + 6] != PROTO_TCP) return;
    net_addr local, remote;
    memcpy(local.b, m + 8 + 8, 16);
    memcpy(remote.b, m + 8 + 24, 16);
    const u8 *t = m + 8 + NET_IP6;
    tcp_path_mtu(&local, get16(t), &remote, get16(t + 2), get32(m + 4));
}

static void icmp6_input(int h, const net_addr *source, const net_addr *destination, const u8 *m, uptr size, u8 hops) {
    if (size < 8 || net_checksum(m, size, net_pseudo_any(source, destination, PROTO_ICMP6, size))) return;
    switch (m[0]) {
    case 2: packet_too_big(m, size); break;
    case 128:
        if (!is_multicast(destination) && size <= 1500 - NET_IP6) {
            u8 frame[NET_FRAME + 16];
            u8 *reply = frame + NET_L4;
            memcpy(reply, m, size);
            reply[0] = 129;
            icmp6_checksum(reply, size, destination, source);
            net6_output(frame, size, PROTO_ICMP6, destination, source);
        }
        break;
    case 129:
        if (ping6.waiting && get16(m + 4) == ping6.id && get16(m + 6) == ping6.sequence) {
            ping6.received = cpu_nanoseconds();
            ping6.waiting = 0;
        }
        break;
    case 134: if (h >= 0 && hops == 255 && !m[1]) router_advertisement(h, source, m, size); break;
    case 135: if (h >= 0 && hops == 255 && !m[1]) neighbour_solicitation(h, source, destination, m, size); break;
    case 136: if (h >= 0 && hops == 255 && !m[1]) neighbour_advertisement(h, m, size); break;
    }
}

/* H is the adapter, or -1 for a packet from the loopback queue. */
void net6_input(int h, const u8 *ip, uptr length) {
    if (length < NET_IP6 || (ip[0] >> 4) != 6) return;
    uptr size = get16(ip + 4);
    if (NET_IP6 + size > length) return;
    net_addr source, destination;
    memcpy(source.b, ip + 8, 16);
    memcpy(destination.b, ip + 24, 16);
    int multicast = is_multicast(&destination);
    if (!multicast && !is_ours(&destination) && h >= 0) {
        /* A neighbour solicitation or advertisement can address a tentative
           address; those are the only messages for it that matter. */
        if (!(v6[h].global_state == ADDRESS_TENTATIVE && net_addr_equal(&destination, &v6[h].global) && ip[6] == PROTO_ICMP6)) return;
    }
    const u8 *p = ip + NET_IP6;
    u8 next = ip[6];
    while (next == 0 || next == 43 || next == 60) {
        if (size < 8) return;
        uptr extra = ((uptr)p[1] + 1) * 8;
        if (extra > size) return;
        next = p[0];
        p += extra;
        size -= extra;
    }
    if (next == PROTO_ICMP6) { icmp6_input(h, &source, &destination, p, size, ip[7]); return; }
    if (multicast) return;
    if (next == PROTO_TCP) { tcp_input(&source, &destination, p, size); return; }
    if (next == PROTO_UDP) {
        if (size < 8) return;
        uptr udp_length = get16(p + 4);
        if (udp_length < 8 || udp_length > size || !get16(p + 6)) return;
        if (net_checksum(p, udp_length, net_pseudo_any(&source, &destination, PROTO_UDP, udp_length))) return;
        udp_input(&source, &destination, p, udp_length);
    }
}

/* ---- timers and adapters ---------------------------------------------------- */

void net6_timers(u64 now) {
    for (int i = 0; i < NET_MAX_DEVICES; ++i) {
        struct v6_device *v = &v6[i];
        if (!v->used || !v->link) continue;
        if (v->global_state == ADDRESS_TENTATIVE && now >= v->dad_at) v->global_state = ADDRESS_READY;
        if (v->global_state != ADDRESS_NONE && now >= v->global_expires) v->global_state = ADDRESS_NONE;
        if (v->global_state == ADDRESS_NONE && now >= v->next_solicit) {
            send_router_solicitation(i);
            v->next_solicit = now + (v->solicits < 3 ? 4 : 60) * SECOND;
            ++v->solicits;
        }
    }
    for (int i = 0; i < NEIGHBOURS; ++i) {
        struct neighbour *n = &neighbours[i];
        if (n->state != N_PENDING || now < n->retry_at) continue;
        if (n->tries >= 3) { n->state = N_FREE; n->packet_length = 0; continue; }
        n->retry_at = now + SECOND;
        ++n->tries;
        send_solicitation(n->device, &n->ip, 0);
    }
}

void net6_link(int h, int up, const u8 *mac) {
    struct v6_device *v = &v6[h];
    for (int i = 0; i < NEIGHBOURS; ++i)
        if (neighbours[i].state != N_FREE && neighbours[i].device == h) { neighbours[i].state = N_FREE; neighbours[i].packet_length = 0; }
    if (!up) { memset(v, 0, sizeof(*v)); return; }
    u8 identifier[8];
    if (random_syscall(identifier, sizeof(identifier)) != (long)sizeof(identifier)) {
        memcpy(identifier, mac, 6);
        identifier[6] = (u8)cpu_nanoseconds();
        identifier[7] = (u8)(cpu_nanoseconds() >> 8);
    }
    memset(v, 0, sizeof(*v));
    v->used = v->link = 1;
    memcpy(v->mac, mac, 6);
    memcpy(v->identifier, identifier, 8);
    v->link_local.b[0] = 0xfe;
    v->link_local.b[1] = 0x80;
    memcpy(v->link_local.b + 8, mac, 3);
    v->link_local.b[8] ^= 2;
    v->link_local.b[11] = 0xff;
    v->link_local.b[12] = 0xfe;
    memcpy(v->link_local.b + 13, mac + 3, 3);
}

/* ---- system calls ----------------------------------------------------------- */

static i64 ping_host6(const u8 *address, unsigned timeout_ms) {
    net_addr destination, source;
    memcpy(destination.b, address, 16);
    if (!net_source(&destination, &source)) return -DOSXC_ENETUNREACH;
    u8 frame[NET_FRAME + 16];
    u8 *p = frame + NET_L4;
    memset(p, 0, 64);
    p[0] = 128;
    ping6.id = (u16)(cpu_nanoseconds() >> 10);
    ping6.sequence = (u16)(ping6.sequence + 1);
    put16(p + 4, ping6.id);
    put16(p + 6, ping6.sequence);
    for (int i = 8; i < 64; ++i) p[i] = (u8)i;
    icmp6_checksum(p, 64, &source, &destination);
    ping6.waiting = 1;
    ping6.sent = cpu_nanoseconds();
    int r = net6_output(frame, 64, PROTO_ICMP6, &source, &destination);
    if (r) { ping6.waiting = 0; return r; }
    u64 deadline = ping6.sent + (u64)timeout_ms * 1000000ull;
    while (ping6.waiting) {
        net_poll();
        if (cpu_nanoseconds() >= deadline) { ping6.waiting = 0; return -DOSXC_ETIMEDOUT; }
        socket_wait_interrupted();
    }
    return (i64)((ping6.received - ping6.sent) / 1000);
}

i64 net6_syscall(int operation, long a, void *b) {
    if (operation == DOSXC_NET_PING6) return b ? ping_host6(b, a > 0 ? (unsigned)a : 2000) : -DOSXC_EINVAL;
    if (operation != DOSXC_NET_INFO6) return -DOSXC_EINVAL;
    if (a < 0 || a >= NET_MAX_DEVICES || !b) return -DOSXC_ENODEV;
    net_poll();
    struct dosxc_netinfo6 *o = b;
    const struct v6_device *v = &v6[a];
    memset(o, 0, sizeof(*o));
    if (!v->used) return 0;
    memcpy(o->link_local, v->link_local.b, 16);
    if (v->global_state == ADDRESS_READY) memcpy(o->global, v->global.b, 16);
    if (v->router_expires > cpu_nanoseconds()) memcpy(o->router, v->router.b, 16);
    memcpy(o->dns[0], v->dns[0].b, 16);
    memcpy(o->dns[1], v->dns[1].b, 16);
    return 0;
}

void net6_init(void) {
    memset(v6, 0, sizeof(v6));
    memset(neighbours, 0, sizeof(neighbours));
}
