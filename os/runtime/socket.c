/*
 * DOS-XC /os/runtime/socket.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "net.h"

struct socket sockets[SOCKET_MAX];
static u16 next_port;

/* ---- lifetime --------------------------------------------------------------- */

struct socket *socket_new(int type, int family) {
    for (int i = 0; i < SOCKET_MAX; ++i) {
        struct socket *s = &sockets[i];
        if (s->used) continue;
        memset(s, 0, sizeof(*s));
        s->rx = memory_alloc(SOCKET_BUFFER, PAGE, OWNER_KERNEL, 0);
        if (!s->rx) return 0;
        if (type == DOSXC_SOCK_STREAM) {
            s->tx = memory_alloc(SOCKET_BUFFER, PAGE, OWNER_KERNEL, 0);
            if (!s->tx) { memory_free(s->rx, OWNER_KERNEL); return 0; }
        }
        s->used = 1;
        s->type = type;
        s->family = family;
        s->state = TCP_CLOSED;
        return s;
    }
    return 0;
}

void socket_free(struct socket *s) {
    if (!s->used) return;
    if (s->rx) memory_free(s->rx, OWNER_KERNEL);
    if (s->tx) memory_free(s->tx, OWNER_KERNEL);
    memset(s, 0, sizeof(*s));
}

/* May S, bound or wildcard, receive a packet sent to DESTINATION? */
int socket_accepts(const struct socket *s, const net_addr *destination) {
    int v4 = net_addr_is_v4(destination);
    if (s->family == DOSXC_AF_INET ? !v4 : v4 && s->v6only) return 0;
    return net_addr_any(&s->local) || net_addr_equal(&s->local, destination);
}

static int port_taken(int type, u16 port, const struct socket *except) {
    for (int i = 0; i < SOCKET_MAX; ++i) {
        const struct socket *s = &sockets[i];
        if (s->used && s != except && s->type == type && s->local_port == port && s->state != TCP_TIME_WAIT) return 1;
    }
    return 0;
}

u16 socket_ephemeral_port(int type) {
    if (!next_port) next_port = (u16)(49152 + cpu_nanoseconds() % 16000);
    for (int tries = 0; tries < 16384; ++tries) {
        u16 port = next_port;
        next_port = next_port >= 65535 ? 49152 : (u16)(next_port + 1);
        if (!port_taken(type, port, 0)) return port;
    }
    return 0;
}

int socket_wait_interrupted(void) {
    if (!terminal_interrupted()) return 0;
    rt_print("^C\r\n");
    process_exit(DOSXC_SIGNALED(2));
}

/* Wait until READY(s), the deadline (0 = none) or an error passes.  Returns
   0, -EAGAIN at the deadline, or never (Ctrl+C). */
static int wait_for(struct socket *s, int (*ready)(const struct socket *), u64 timeout) {
    u64 deadline = timeout ? cpu_nanoseconds() + timeout : 0;
    for (;;) {
        net_poll();
        if (ready(s)) return 0;
        if (deadline && cpu_nanoseconds() >= deadline) return -DOSXC_EAGAIN;
        socket_wait_interrupted();
        __asm__ volatile("pause");
    }
}

/* ---- UDP -------------------------------------------------------------------- */

struct datagram { net_addr ip; u16 port; u16 length; };

static void ring_put(struct socket *s, const void *data, uptr length) {
    uptr at = (s->rx_head + s->rx_used) % SOCKET_BUFFER;
    uptr first = SOCKET_BUFFER - at;
    if (first > length) first = length;
    memcpy(s->rx + at, data, first);
    memcpy(s->rx, (const u8 *)data + first, length - first);
    s->rx_used += length;
}

static void ring_get(struct socket *s, void *data, uptr length, int peek) {
    uptr first = SOCKET_BUFFER - s->rx_head;
    if (first > length) first = length;
    memcpy(data, s->rx + s->rx_head, first);
    memcpy((u8 *)data + first, s->rx, length - first);
    if (!peek) {
        s->rx_head = (s->rx_head + length) % SOCKET_BUFFER;
        s->rx_used -= length;
    }
}

void udp_input(const net_addr *source, const net_addr *destination, const u8 *p, uptr length) {
    u16 sport = get16(p), dport = get16(p + 2);
    uptr size = length - 8;
    for (int i = 0; i < SOCKET_MAX; ++i) {
        struct socket *s = &sockets[i];
        if (!s->used || s->type != DOSXC_SOCK_DGRAM || s->local_port != dport) continue;
        if (!socket_accepts(s, destination)) continue;
        if (s->remote_port && (s->remote_port != sport || !net_addr_equal(&s->remote, source))) continue;
        if (s->rx_used + sizeof(struct datagram) + size > SOCKET_BUFFER) return;
        struct datagram d = {*source, sport, (u16)size};
        ring_put(s, &d, sizeof(d));
        ring_put(s, p + 8, size);
        return;
    }
}

static int udp_send(struct socket *s, const void *data, uptr size, const net_addr *ip, u16 port) {
    int v4 = net_addr_is_v4(ip);
    if (size > 1500 - (v4 ? NET_IP : NET_IP6) - 8) return -DOSXC_EMSGSIZE;
    net_addr source = s->local;
    if (net_addr_any(&source) && !net_source(ip, &source)) {
        if (!v4 || net_addr_get_v4(ip) != 0xffffffffu) return net_ready() ? -DOSXC_ENETUNREACH : -DOSXC_ENETDOWN;
        source = net_addr_v4(0);
    }
    if (!s->local_port && !(s->local_port = socket_ephemeral_port(DOSXC_SOCK_DGRAM))) return -DOSXC_EADDRINUSE;
    u8 frame[NET_FRAME + 16];
    u8 *u = frame + NET_L4;
    put16(u, s->local_port);
    put16(u + 2, port);
    put16(u + 4, (u16)(8 + size));
    put16(u + 6, 0);
    memcpy(u + 8, data, size);
    u16 sum = net_checksum(u, 8 + size, net_pseudo_any(&source, ip, PROTO_UDP, 8 + size));
    put16(u + 6, sum ? sum : 0xffff);
    return net_send(frame, 8 + size, PROTO_UDP, &source, ip);
}

/* ---- readiness -------------------------------------------------------------- */

static int connect_done(const struct socket *s) {
    return s->state != TCP_SYN_SENT;
}
static int can_read(const struct socket *s) {
    if (s->type == DOSXC_SOCK_DGRAM) return s->rx_used || s->error || s->read_shut;
    if (s->state == TCP_LISTEN) {
        for (int i = 0; i < s->accept_count; ++i)
            if (s->accept_queue[i]->state != TCP_SYN_RCVD) return 1;
        return 0;
    }
    return s->rx_used || s->peer_fin || s->error || s->read_shut || s->state == TCP_CLOSED;
}
static int can_write(const struct socket *s) {
    if (s->type == DOSXC_SOCK_DGRAM) return 1;
    if (s->error || s->write_shut || s->state == TCP_CLOSED) return 1;
    if (s->state == TCP_SYN_SENT || s->state == TCP_SYN_RCVD) return 0;
    return tcp_can_write(s);
}

int socket_poll(struct socket *s, int events) {
    net_poll();
    int ready = 0;
    if ((events & DOSXC_POLL_IN) && can_read(s)) ready |= DOSXC_POLL_IN;
    if ((events & DOSXC_POLL_OUT) && can_write(s)) ready |= DOSXC_POLL_OUT;
    if (s->error) ready |= DOSXC_POLL_ERR;
    if (s->type == DOSXC_SOCK_STREAM && s->peer_fin && !s->rx_used) ready |= DOSXC_POLL_HUP;
    if (s->type == DOSXC_SOCK_STREAM && s->state == TCP_CLOSED && !s->error && !s->rx_used && s->remote_port) ready |= DOSXC_POLL_HUP;
    return ready;
}

/* ---- reading and writing ---------------------------------------------------- */

static int take_error(struct socket *s) {
    int e = s->error;
    s->error = 0;
    return e;
}

i64 socket_read(struct socket *s, void *buffer, uptr size, int flags, int nonblock) {
    if (!size) return 0;
    if (s->type == DOSXC_SOCK_STREAM && (s->state == TCP_LISTEN || (s->state == TCP_CLOSED && !s->remote_port && !s->error))) return -DOSXC_ENOTCONN;
    if ((flags & DOSXC_MSG_DONTWAIT)) nonblock = 1;
    uptr total = 0;
    for (;;) {
        if (!can_read(s)) {
            if (total && !(flags & DOSXC_MSG_WAITALL)) return (i64)total;
            if (nonblock) return total ? (i64)total : -DOSXC_EAGAIN;
            int r = wait_for(s, can_read, s->receive_timeout);
            if (r) return total ? (i64)total : r;
        }
        if (s->type == DOSXC_SOCK_DGRAM) {
            if (!s->rx_used) return s->read_shut ? 0 : -take_error(s);
            struct datagram d;
            ring_get(s, &d, sizeof(d), 0);
            uptr n = d.length < size ? d.length : size;
            ring_get(s, buffer, n, flags & DOSXC_MSG_PEEK);
            if (flags & DOSXC_MSG_PEEK) {
                s->rx_head = (s->rx_head + SOCKET_BUFFER - sizeof(d)) % SOCKET_BUFFER;
                s->rx_used += sizeof(d);
            } else if (d.length > n) {
                u8 skip[256];
                uptr left = d.length - n;
                while (left) {
                    uptr c = left < sizeof(skip) ? left : sizeof(skip);
                    ring_get(s, skip, c, 0);
                    left -= c;
                }
            }
            return (i64)n;
        }
        uptr n = tcp_receive(s, (u8 *)buffer + total, size - total, flags & DOSXC_MSG_PEEK);
        total += n;
        if (flags & DOSXC_MSG_PEEK) return (i64)total;
        if (total == size) return (i64)total;
        if (!s->rx_used) {
            if (s->error) { if (total) return (i64)total; return -take_error(s); }
            if (s->peer_fin || s->read_shut || s->state == TCP_CLOSED) return (i64)total;
        }
        if (total && !(flags & DOSXC_MSG_WAITALL)) return (i64)total;
    }
}

i64 socket_write(struct socket *s, const void *buffer, uptr size, int flags, int nonblock) {
    if (s->type == DOSXC_SOCK_DGRAM) {
        if (!s->remote_port) return -DOSXC_EDESTADDRREQ;
        int r = udp_send(s, buffer, size, &s->remote, s->remote_port);
        return r ? r : (i64)size;
    }
    if ((flags & DOSXC_MSG_DONTWAIT)) nonblock = 1;
    uptr total = 0;
    while (total < size) {
        if (s->error) { if (total) return (i64)total; return -take_error(s); }
        if (s->write_shut || s->state == TCP_CLOSED || s->fin_queued) return total ? (i64)total : -DOSXC_EPIPE;
        if (s->state == TCP_SYN_SENT || s->state == TCP_SYN_RCVD) {
            if (nonblock) return total ? (i64)total : -DOSXC_EAGAIN;
            wait_for(s, connect_done, 0);
            continue;
        }
        if (!tcp_can_write(s)) {
            if (nonblock) return total ? (i64)total : -DOSXC_EAGAIN;
            int r = wait_for(s, can_write, s->send_timeout);
            if (r) return total ? (i64)total : r;
            continue;
        }
        total += (uptr)tcp_send(s, (const u8 *)buffer + total, size - total);
    }
    net_poll();
    return (i64)total;
}

void socket_close(struct socket *s) {
    if (s->type == DOSXC_SOCK_STREAM) tcp_close(s);
    else socket_free(s);
}

/* ---- system calls ------------------------------------------------------------ */

static int fetch_address(const struct socket *s, const void *address, uptr length, net_addr *ip, u16 *port) {
    if (!address) return -DOSXC_EINVAL;
    if (s->family == DOSXC_AF_INET6) {
        const struct dosxc_sockaddr_in6 *a = address;
        if (length < sizeof(*a)) return -DOSXC_EINVAL;
        if (a->family != DOSXC_AF_INET6) return -DOSXC_EAFNOSUPPORT;
        memcpy(ip->b, a->address, 16);
        *port = get16((const u8 *)&a->port);
        return 0;
    }
    const struct dosxc_sockaddr_in *a = address;
    if (length < sizeof(*a)) return -DOSXC_EINVAL;
    if (a->family != DOSXC_AF_INET) return -DOSXC_EAFNOSUPPORT;
    *ip = net_addr_v4(get32((const u8 *)&a->address));
    *port = get16((const u8 *)&a->port);
    return 0;
}

static void store_address(const struct socket *s, void *address, dosxc_u32 *length, const net_addr *ip, u16 port) {
    if (!address || !length) return;
    union { struct dosxc_sockaddr_in v4; struct dosxc_sockaddr_in6 v6; } a;
    dosxc_u32 size;
    memset(&a, 0, sizeof(a));
    if (s->family == DOSXC_AF_INET6) {
        a.v6.family = DOSXC_AF_INET6;
        put16((u8 *)&a.v6.port, port);
        memcpy(a.v6.address, ip->b, 16);
        size = sizeof(a.v6);
    } else {
        a.v4.family = DOSXC_AF_INET;
        put16((u8 *)&a.v4.port, port);
        put32((u8 *)&a.v4.address, net_addr_get_v4(ip));
        size = sizeof(a.v4);
    }
    memcpy(address, &a, *length < size ? *length : size);
    *length = size;
}

static i64 sys_connect(struct socket *s, const void *address, uptr length, int nonblock) {
    net_addr ip, source;
    u16 port;
    int r = fetch_address(s, address, length, &ip, &port);
    if (r) return r;
    if (s->type == DOSXC_SOCK_DGRAM) {
        s->remote = ip;
        s->remote_port = port;
        if (net_addr_any(&s->local) && net_source(&ip, &source)) s->local = source;
        if (!s->local_port) s->local_port = socket_ephemeral_port(DOSXC_SOCK_DGRAM);
        return 0;
    }
    if (s->state == TCP_ESTABLISHED) return -DOSXC_EISCONN;
    if (s->state == TCP_SYN_SENT) return nonblock ? -DOSXC_EALREADY : 0;
    if (s->state != TCP_CLOSED) return -DOSXC_EINVAL;
    if (net_addr_any(&ip) || !port) return -DOSXC_EADDRNOTAVAIL;
    if (!net_source(&ip, &source)) return net_ready() ? -DOSXC_ENETUNREACH : -DOSXC_ENETDOWN;
    s->local = source;
    if (!s->local_port && !(s->local_port = socket_ephemeral_port(DOSXC_SOCK_STREAM))) return -DOSXC_EADDRINUSE;
    s->remote = ip;
    s->remote_port = port;
    s->error = 0;
    tcp_connect(s);
    if (nonblock) return -DOSXC_EINPROGRESS;
    wait_for(s, connect_done, 0);
    if (s->state == TCP_CLOSED) {
        int e = take_error(s);
        return -(e ? e : DOSXC_ECONNREFUSED);
    }
    return 0;
}

static i64 sys_accept(struct socket *s, void *address, dosxc_u32 *length, int nonblock) {
    if (s->state != TCP_LISTEN) return -DOSXC_EINVAL;
    if (!can_read(s)) {
        if (nonblock) return -DOSXC_EAGAIN;
        int r = wait_for(s, can_read, s->receive_timeout);
        if (r) return r;
    }
    struct socket *c = tcp_accept_take(s);
    if (!c) return -DOSXC_EAGAIN;
    int fd = files_socket_open(c);
    if (fd < 0) {
        c->closed = 1;
        tcp_close(c);
        return fd;
    }
    c->closed = 0;
    store_address(c, address, length, &c->remote, c->remote_port);
    return fd;
}

static i64 sys_sockopt(struct socket *s, int set, int level, int name, struct dosxc_sockopt *o) {
    enum { SOL_SOCKET = 1, IPPROTO_TCP = 6, SO_REUSEADDR = 2, SO_TYPE = 3, SO_ERROR = 4, SO_BROADCAST = 6, SO_SNDBUF = 7,
           SO_RCVBUF = 8, SO_KEEPALIVE = 9, SO_LINGER = 13, SO_RCVTIMEO = 20, SO_SNDTIMEO = 21, TCP_NODELAY = 1 };
    if (!o || !o->length) return -DOSXC_EFAULT;
    if (level == IPPROTO_TCP) {
        if (name != TCP_NODELAY) return -DOSXC_ENOPROTOOPT;
        if (!set) { *(int *)o->value = 1; *o->length = sizeof(int); }
        return 0;
    }
    if (level == 0) {
        /* UDP never fragments here, which is what IP_MTU_DISCOVER asks for.
           IP_PKTINFO and IP_RECVTOS are accepted and simply add no control
           data to received messages. */
        enum { IP_PKTINFO = 8, IP_MTU_DISCOVER = 10, IP_RECVERR = 11, IP_RECVTOS = 13 };
        if (name != IP_PKTINFO && name != IP_MTU_DISCOVER && name != IP_RECVERR && name != IP_RECVTOS) return -DOSXC_ENOPROTOOPT;
        if (!set) { *(int *)o->value = 0; *o->length = sizeof(int); }
        return 0;
    }
    if (level == 41) {
        enum { IPV6_UNICAST_HOPS = 16, IPV6_MTU_DISCOVER = 23, IPV6_RECVERR = 25, IPV6_V6ONLY = 26,
               IPV6_RECVPKTINFO = 49, IPV6_RECVTCLASS = 66 };
        if (s->family != DOSXC_AF_INET6) return -DOSXC_ENOPROTOOPT;
        if (name == IPV6_V6ONLY) {
            if (set) s->v6only = *(int *)o->value != 0;
            else { *(int *)o->value = s->v6only; *o->length = sizeof(int); }
            return 0;
        }
        if (name != IPV6_UNICAST_HOPS && name != IPV6_MTU_DISCOVER && name != IPV6_RECVERR &&
            name != IPV6_RECVPKTINFO && name != IPV6_RECVTCLASS) return -DOSXC_ENOPROTOOPT;
        if (!set) { *(int *)o->value = 0; *o->length = sizeof(int); }
        return 0;
    }
    if (level != SOL_SOCKET) return -DOSXC_ENOPROTOOPT;
    switch (name) {
    case SO_REUSEADDR: if (set) s->reuse = *(int *)o->value != 0; else *(int *)o->value = s->reuse; break;
    case SO_ERROR: if (set) return -DOSXC_EINVAL; *(int *)o->value = take_error(s); break;
    case SO_TYPE: if (set) return -DOSXC_EINVAL; *(int *)o->value = s->type; break;
    case SO_RCVBUF: case SO_SNDBUF: if (!set) *(int *)o->value = (int)SOCKET_BUFFER; break;
    case SO_BROADCAST: case SO_KEEPALIVE: case SO_LINGER: if (!set) *(int *)o->value = 0; break;
    case SO_RCVTIMEO: case SO_SNDTIMEO: {
        long *time = o->value;
        u64 *field = name == SO_RCVTIMEO ? &s->receive_timeout : &s->send_timeout;
        if (set) *field = (u64)time[0] * 1000000000ull + (u64)time[1] * 1000ull;
        else { time[0] = (long)(*field / 1000000000ull); time[1] = (long)(*field % 1000000000ull / 1000); }
        return 0;
    }
    default: return -DOSXC_ENOPROTOOPT;
    }
    if (!set) *o->length = sizeof(int);
    return 0;
}

i64 socket_syscall(uptr number, uptr a, uptr b, uptr c, uptr d, uptr e) {
    if (number == DOSXC_SYS_SOCKET) {
        int type = (int)b & 7;
        if ((int)a != DOSXC_AF_INET && (int)a != DOSXC_AF_INET6) return -DOSXC_EAFNOSUPPORT;
        if (type != DOSXC_SOCK_STREAM && type != DOSXC_SOCK_DGRAM) return -DOSXC_EPROTONOSUPPORT;
        if (c && (int)c != (type == DOSXC_SOCK_STREAM ? PROTO_TCP : PROTO_UDP)) return -DOSXC_EPROTONOSUPPORT;
        struct socket *s = socket_new(type, (int)a);
        if (!s) return -DOSXC_ENOBUFS;
        int fd = files_socket_open(s);
        if (fd < 0) { socket_free(s); return fd; }
        return fd;
    }
    int flags = 0;
    struct socket *s = files_socket((int)a, &flags);
    if (!s) return -DOSXC_ENOTSOCK;
    int nonblock = (flags & DOSXC_O_NONBLOCK) != 0;
    switch (number) {
    case DOSXC_SYS_CONNECT: return sys_connect(s, (const void *)b, c, nonblock);
    case DOSXC_SYS_BIND: {
        net_addr ip;
        u16 port;
        int r = fetch_address(s, (const void *)b, c, &ip, &port);
        if (r) return r;
        if (s->local_port) return -DOSXC_EINVAL;
        if (!net_addr_any(&ip) && !net_is_local_any(&ip)) return -DOSXC_EADDRNOTAVAIL;
        if (port && !s->reuse && port_taken(s->type, port, s)) return -DOSXC_EADDRINUSE;
        s->local = ip;
        s->local_port = port ? port : socket_ephemeral_port(s->type);
        return 0;
    }
    case DOSXC_SYS_LISTEN:
        if (s->type != DOSXC_SOCK_STREAM) return -DOSXC_EOPNOTSUPP;
        if (!s->local_port && !(s->local_port = socket_ephemeral_port(DOSXC_SOCK_STREAM))) return -DOSXC_EADDRINUSE;
        return tcp_listen(s, (int)b);
    case DOSXC_SYS_ACCEPT: return sys_accept(s, (void *)b, (dosxc_u32 *)c, nonblock);
    case DOSXC_SYS_SENDTO: {
        struct dosxc_message *m = (struct dosxc_message *)b;
        if (m->address && s->type == DOSXC_SOCK_DGRAM) {
            net_addr ip;
            u16 port;
            int r = fetch_address(s, m->address, *m->address_length, &ip, &port);
            if (r) return r;
            r = udp_send(s, m->buffer, m->length, &ip, port);
            return r ? r : (i64)m->length;
        }
        return socket_write(s, m->buffer, m->length, m->flags, nonblock);
    }
    case DOSXC_SYS_RECVFROM: {
        struct dosxc_message *m = (struct dosxc_message *)b;
        if (s->type == DOSXC_SOCK_DGRAM && m->address && s->rx_used) {
            struct datagram header;
            ring_get(s, &header, sizeof(header), 1);
            store_address(s, m->address, m->address_length, &header.ip, header.port);
        } else if (s->type == DOSXC_SOCK_DGRAM && m->address) {
            if (!can_read(s)) {
                if (nonblock || (m->flags & DOSXC_MSG_DONTWAIT)) return -DOSXC_EAGAIN;
                int w = wait_for(s, can_read, s->receive_timeout);
                if (w) return w;
            }
            struct datagram header;
            if (!s->rx_used) return -take_error(s);
            ring_get(s, &header, sizeof(header), 1);
            store_address(s, m->address, m->address_length, &header.ip, header.port);
        } else if (m->address && s->type == DOSXC_SOCK_STREAM) {
            store_address(s, m->address, m->address_length, &s->remote, s->remote_port);
        }
        return socket_read(s, m->buffer, m->length, m->flags, nonblock);
    }
    case DOSXC_SYS_SHUTDOWN: {
        int how = (int)b;
        if (s->type == DOSXC_SOCK_DGRAM) return 0;
        if (s->state == TCP_CLOSED || s->state == TCP_LISTEN) return -DOSXC_ENOTCONN;
        if (how == 0 || how == 2) s->read_shut = 1;
        if (how == 1 || how == 2) tcp_shutdown_write(s);
        return 0;
    }
    case DOSXC_SYS_SOCKOPT: return sys_sockopt(s, (int)b, (int)c, (int)d, (struct dosxc_sockopt *)e);
    case DOSXC_SYS_SOCKNAME: {
        void *address = (void *)c;
        dosxc_u32 *length = (dosxc_u32 *)d;
        if (b) store_address(s, address, length, &s->remote, s->remote_port);
        else store_address(s, address, length, &s->local, s->local_port);
        return 0;
    }
    case DOSXC_SYS_FDPOLL:
        socket_wait_interrupted();
        return socket_poll(s, (int)b);
    }
    return -DOSXC_EINVAL;
}
