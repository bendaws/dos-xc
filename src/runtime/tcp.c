/*
 * DOS-XC /runtime/tcp.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * TCP: RFC 793 with slow start, fast retransmit (Reno), retransmission
 * timers (RFC 6298) and one out-of-order range kept for the receiver.
 * Windows are at most 64 KiB, so window scaling is not offered.
 */

#include "net.h"

#define FIN 1
#define SYN 2
#define RST 4
#define PSH 8
#define ACK 16

#define SEQ_LT(a, b) ((int)((a) - (b)) < 0)
#define SEQ_LE(a, b) ((int)((a) - (b)) <= 0)
#define SEQ_GT(a, b) ((int)((a) - (b)) > 0)
#define SEQ_GE(a, b) ((int)((a) - (b)) >= 0)

#define RTO_INITIAL 1000000000ull
#define RTO_MIN 200000000ull
#define RTO_MAX 30000000000ull
#define SYN_RETRIES 5
#define DATA_RETRIES 10
#define TIME_WAIT_NS 2000000000ull
#define FIN_WAIT_NS 30000000000ull

static u32 window_free(const struct socket *s) {
    uptr free_space = SOCKET_BUFFER - s->rx_used;
    return (u32)free_space;
}

static void emit(struct socket *s, u32 seq, u8 flags, uptr data_offset, uptr length) {
    u8 frame[NET_FRAME + 16];
    u8 *t = frame + NET_L4;
    uptr header = 20;
    u32 window = window_free(s);
    if (window > 65535) window = 65535;
    put16(t, s->local_port);
    put16(t + 2, s->remote_port);
    put32(t + 4, seq);
    put32(t + 8, (flags & ACK) ? s->rcv_nxt : 0);
    put16(t + 14, (u16)window);
    put16(t + 16, 0);
    put16(t + 18, 0);
    if (flags & SYN) {                 /* the MSS option */
        t[20] = 2;
        t[21] = 4;
        put16(t + 22, NET_MSS);
        header = 24;
    }
    t[12] = (u8)(header / 4 << 4);
    t[13] = flags;
    if (length) {
        uptr at = (s->tx_head + data_offset) % SOCKET_BUFFER;
        uptr first = SOCKET_BUFFER - at;
        if (first > length) first = length;
        memcpy(t + header, s->tx + at, first);
        memcpy(t + header + first, s->tx, length - first);
    }
    uptr total = header + length;
    put16(t + 16, net_checksum(t, total, net_pseudo(s->local_ip, s->remote_ip, PROTO_TCP, total)));
    net_output(frame, total, PROTO_TCP, s->local_ip, s->remote_ip);
    if (flags & ACK) s->advertised = window;
}

static void send_reset(u32 source, u32 destination, u16 sport, u16 dport, u32 seq, u32 ack, u8 flags) {
    u8 frame[NET_FRAME + 16];
    u8 *t = frame + NET_L4;
    memset(t, 0, 20);
    put16(t, sport);
    put16(t + 2, dport);
    put32(t + 4, seq);
    put32(t + 8, ack);
    t[12] = 5 << 4;
    t[13] = flags;
    put16(t + 16, net_checksum(t, 20, net_pseudo(source, destination, PROTO_TCP, 20)));
    net_output(frame, 20, PROTO_TCP, source, destination);
}

static void arm_retransmit(struct socket *s, int force) {
    if (force || !s->rto_at) s->rto_at = cpu_nanoseconds() + s->rto;
}

static void to_closed(struct socket *s, int error) {
    if (error) s->error = error;
    s->state = TCP_CLOSED;
    s->rto_at = s->persist_at = s->expire_at = 0;
    if (s->closed) socket_free(s);
}

static void output(struct socket *s) {
    if (s->state < TCP_ESTABLISHED || s->state == TCP_FIN_WAIT_2 || s->state == TCP_TIME_WAIT) return;
    for (;;) {
        u32 flight = s->snd_nxt - s->snd_una;
        uptr sent = flight > s->tx_used ? s->tx_used : flight;
        uptr unsent = s->tx_used - sent;
        u32 window = s->cwnd < s->snd_wnd ? s->cwnd : s->snd_wnd;
        u32 room = window > flight ? window - flight : 0;
        uptr n = unsent;
        if (n > s->mss) n = s->mss;
        if (n > room) n = room;
        int fin = s->fin_queued && !s->fin_sent && n == unsent;
        if (!n && !fin) break;
        if (!s->rtt_active && n) {
            s->rtt_active = 1;
            s->rtt_seq = s->snd_nxt + (u32)n;
            s->rtt_started = cpu_nanoseconds();
        }
        emit(s, s->snd_nxt, (u8)(ACK | (n && n == unsent ? PSH : 0) | (fin ? FIN : 0)), sent, n);
        s->snd_nxt += (u32)n + (fin ? 1 : 0);
        if (fin) { s->fin_sent = 1; s->fin_seq = s->snd_nxt - 1; }
        if (SEQ_GT(s->snd_nxt, s->snd_max)) s->snd_max = s->snd_nxt;
        arm_retransmit(s, 0);
    }
    u32 flight = s->snd_nxt - s->snd_una;
    if (!flight && s->tx_used && !s->snd_wnd && !s->persist_at) {
        s->persist_interval = 500000000ull;
        s->persist_at = cpu_nanoseconds() + s->persist_interval;
    }
}

/* ---- connections --------------------------------------------------------- */

static void start_sequence(struct socket *s) {
    s->iss = (u32)(cpu_nanoseconds() >> 4) ^ (u32)(s->local_port << 16 | s->remote_port);
    s->snd_una = s->snd_nxt = s->snd_max = s->iss;
    s->mss = 536;
    s->cwnd = 10 * NET_MSS;
    s->ssthresh = 65535;
    s->rto = RTO_INITIAL;
    s->srtt = s->rttvar = 0;
    s->retries = 0;
}

int tcp_connect(struct socket *s) {
    start_sequence(s);
    s->state = TCP_SYN_SENT;
    s->passive = 0;
    emit(s, s->iss, SYN, 0, 0);
    s->snd_nxt = s->snd_max = s->iss + 1;
    arm_retransmit(s, 1);
    return 0;
}

int tcp_listen(struct socket *s, int backlog) {
    if (backlog < 1) backlog = 1;
    if (backlog > SOCKET_BACKLOG) backlog = SOCKET_BACKLOG;
    s->backlog = backlog;
    s->state = TCP_LISTEN;
    return 0;
}

struct socket *tcp_accept_take(struct socket *s) {
    for (int i = 0; i < s->accept_count; ++i) {
        struct socket *child = s->accept_queue[i];
        if (child->state == TCP_SYN_RCVD) continue;
        memmove(&s->accept_queue[i], &s->accept_queue[i + 1], (uptr)(s->accept_count - i - 1) * sizeof(s->accept_queue[0]));
        --s->accept_count;
        child->parent = 0;
        return child;
    }
    return 0;
}

static struct socket *lookup_socket(u32 source, u16 sport, u32 destination, u16 dport) {
    struct socket *listener = 0;
    for (int i = 0; i < SOCKET_MAX; ++i) {
        struct socket *s = &sockets[i];
        if (!s->used || s->type != DOSXC_SOCK_STREAM || s->local_port != dport) continue;
        if (s->state == TCP_LISTEN) {
            if (!s->local_ip || s->local_ip == destination) listener = s;
            continue;
        }
        if (s->state != TCP_CLOSED && s->remote_port == sport && s->remote_ip == source &&
            (s->local_ip == destination || !s->local_ip)) return s;
    }
    return listener;
}

static void accept_syn(struct socket *listener, u32 source, u16 sport, u32 destination, u32 seq, u32 mss) {
    if (listener->accept_count >= listener->backlog) return;
    struct socket *c = socket_new(DOSXC_SOCK_STREAM);
    if (!c) return;
    c->local_ip = destination;
    c->local_port = listener->local_port;
    c->remote_ip = source;
    c->remote_port = sport;
    start_sequence(c);
    c->state = TCP_SYN_RCVD;
    c->passive = 1;
    c->closed = 1;                     /* until accept() hands it out */
    c->parent = listener;
    c->irs = seq;
    c->rcv_nxt = seq + 1;
    c->mss = mss > NET_MSS ? NET_MSS : mss;
    listener->accept_queue[listener->accept_count++] = c;
    emit(c, c->iss, SYN | ACK, 0, 0);
    c->snd_nxt = c->snd_max = c->iss + 1;
    arm_retransmit(c, 1);
}

static void drop_from_parent(struct socket *c) {
    struct socket *p = c->parent;
    if (!p) return;
    for (int i = 0; i < p->accept_count; ++i)
        if (p->accept_queue[i] == c) {
            memmove(&p->accept_queue[i], &p->accept_queue[i + 1], (uptr)(p->accept_count - i - 1) * sizeof(p->accept_queue[0]));
            --p->accept_count;
            break;
        }
    c->parent = 0;
}

/* ---- input --------------------------------------------------------------- */

static void store_data(struct socket *s, u32 seq, const u8 *data, uptr length, int *in_order) {
    u32 window = window_free(s);
    u32 offset = seq - s->rcv_nxt;
    if (offset >= window) return;
    if (length > window - offset) length = window - offset;
    uptr at = (s->rx_head + s->rx_used + offset) % SOCKET_BUFFER;
    uptr first = SOCKET_BUFFER - at;
    if (first > length) first = length;
    memcpy(s->rx + at, data, first);
    memcpy(s->rx, data + first, length - first);
    if (!offset) {
        s->rx_used += length;
        s->rcv_nxt += (u32)length;
        *in_order = 1;
        if (s->ooo_used && SEQ_GE(s->rcv_nxt, s->ooo_start)) {
            if (SEQ_GT(s->ooo_end, s->rcv_nxt)) {
                s->rx_used += s->ooo_end - s->rcv_nxt;
                s->rcv_nxt = s->ooo_end;
            }
            s->ooo_used = 0;
        }
        return;
    }
    u32 end = seq + (u32)length;
    if (s->ooo_used && SEQ_LE(seq, s->ooo_end) && SEQ_GE(end, s->ooo_start)) {
        if (SEQ_LT(seq, s->ooo_start)) s->ooo_start = seq;
        if (SEQ_GT(end, s->ooo_end)) s->ooo_end = end;
    } else if (!s->ooo_used) {
        s->ooo_used = 1;
        s->ooo_start = seq;
        s->ooo_end = end;
    }
}

static void update_rtt(struct socket *s, u32 ack) {
    if (!s->rtt_active || SEQ_LT(ack, s->rtt_seq)) return;
    s->rtt_active = 0;
    u64 sample = cpu_nanoseconds() - s->rtt_started;
    if (!s->srtt) {
        s->srtt = sample;
        s->rttvar = sample / 2;
    } else {
        u64 difference = s->srtt > sample ? s->srtt - sample : sample - s->srtt;
        s->rttvar = (3 * s->rttvar + difference) / 4;
        s->srtt = (7 * s->srtt + sample) / 8;
    }
    u64 rto = s->srtt + 4 * s->rttvar;
    s->rto = rto < RTO_MIN ? RTO_MIN : rto > RTO_MAX ? RTO_MAX : rto;
}

static void fin_acknowledged(struct socket *s) {
    switch (s->state) {
    case TCP_FIN_WAIT_1:
        s->state = TCP_FIN_WAIT_2;
        s->expire_at = s->closed ? cpu_nanoseconds() + FIN_WAIT_NS : 0;
        break;
    case TCP_CLOSING:
        s->state = TCP_TIME_WAIT;
        s->expire_at = cpu_nanoseconds() + TIME_WAIT_NS;
        break;
    case TCP_LAST_ACK:
        to_closed(s, 0);
        break;
    }
}

static void process_ack(struct socket *s, u32 ack, u32 seq, u32 window, uptr data_length, int flags) {
    u32 flight = s->snd_nxt - s->snd_una;
    if (SEQ_GT(ack, s->snd_una) && SEQ_LE(ack, s->snd_max)) {
        u32 acked = ack - s->snd_una;
        uptr data = acked > s->tx_used ? s->tx_used : acked;
        s->tx_head = (s->tx_head + data) % SOCKET_BUFFER;
        s->tx_used -= data;
        s->snd_una = ack;
        if (SEQ_LT(s->snd_nxt, ack)) s->snd_nxt = ack;
        update_rtt(s, ack);
        if (s->dupacks >= 3) s->cwnd = s->ssthresh;
        else if (s->cwnd < s->ssthresh) s->cwnd += acked < s->mss ? acked : s->mss;
        else s->cwnd += s->mss * s->mss / s->cwnd + 1;
        if (s->cwnd > 2 * SOCKET_BUFFER) s->cwnd = 2 * SOCKET_BUFFER;
        s->dupacks = 0;
        s->retries = 0;
        s->rto_at = 0;
        if (s->snd_nxt != s->snd_una) arm_retransmit(s, 1);
        if (s->fin_sent && SEQ_GT(ack, s->fin_seq)) {
            fin_acknowledged(s);
            if (!s->used) return;
        }
    } else if (ack == s->snd_una && flight && !data_length && !(flags & (SYN | FIN)) && window == s->snd_wnd) {
        if (++s->dupacks == 3) {
            s->ssthresh = flight / 2 > 2 * s->mss ? flight / 2 : 2 * s->mss;
            s->cwnd = s->ssthresh + 3 * s->mss;
            uptr n = s->tx_used < s->mss ? s->tx_used : s->mss;
            s->rtt_active = 0;
            emit(s, s->snd_una, ACK, 0, n);
            arm_retransmit(s, 1);
        } else if (s->dupacks > 3) {
            s->cwnd += s->mss;
        }
    }
    if (SEQ_LT(s->snd_wl1, seq) || (s->snd_wl1 == seq && SEQ_LE(s->snd_wl2, ack))) {
        s->snd_wnd = window;
        s->snd_wl1 = seq;
        s->snd_wl2 = ack;
        if (window) s->persist_at = 0;
    }
}

void tcp_input(u32 source, u32 destination, const u8 *t, uptr length) {
    if (length < 20) return;
    uptr header = (uptr)(t[12] >> 4) * 4;
    if (header < 20 || header > length) return;
    if (net_checksum(t, length, net_pseudo(source, destination, PROTO_TCP, length))) return;
    u16 sport = get16(t), dport = get16(t + 2);
    u32 seq = get32(t + 4), ack = get32(t + 8);
    u8 flags = t[13];
    u32 window = get16(t + 14);
    const u8 *data = t + header;
    uptr data_length = length - header;
    u32 mss = 536;
    for (uptr at = 20; at < header;) {
        u8 kind = t[at];
        if (kind == 0) break;
        if (kind == 1) { ++at; continue; }
        if (at + 1 >= header || t[at + 1] < 2 || at + t[at + 1] > header) break;
        if (kind == 2 && t[at + 1] == 4) mss = get16(t + at + 2);
        at += t[at + 1];
    }
    struct socket *s = lookup_socket(source, sport, destination, dport);
    if (!s || s->state == TCP_CLOSED) {
        if (flags & RST) return;
        if (flags & ACK) send_reset(destination, source, dport, sport, ack, 0, RST);
        else send_reset(destination, source, dport, sport, 0, seq + (u32)data_length + ((flags & SYN) ? 1 : 0) + ((flags & FIN) ? 1 : 0), RST | ACK);
        return;
    }
    if (s->state == TCP_LISTEN) {
        if ((flags & (SYN | ACK | RST)) == SYN) accept_syn(s, source, sport, destination, seq, mss);
        return;
    }
    if (s->state == TCP_SYN_SENT) {
        if ((flags & ACK) && (SEQ_LE(ack, s->iss) || SEQ_GT(ack, s->snd_nxt))) {
            if (!(flags & RST)) send_reset(destination, source, dport, sport, ack, 0, RST);
            return;
        }
        if (flags & RST) { if (flags & ACK) to_closed(s, DOSXC_ECONNREFUSED); return; }
        if (!(flags & SYN) || !(flags & ACK)) return;
        s->irs = seq;
        s->rcv_nxt = seq + 1;
        s->snd_una = ack;
        s->mss = mss > NET_MSS ? NET_MSS : mss;
        if (!s->mss) s->mss = 536;
        s->snd_wnd = window;
        s->snd_wl1 = seq;
        s->snd_wl2 = ack;
        s->state = TCP_ESTABLISHED;
        s->retries = 0;
        s->rto = RTO_INITIAL;
        s->rto_at = 0;
        emit(s, s->snd_nxt, ACK, 0, 0);
        output(s);
        return;
    }
    /* Everything else starts with the sequence number check. */
    u32 consumed = (u32)data_length + ((flags & SYN) ? 1 : 0) + ((flags & FIN) ? 1 : 0);
    u32 limit = window_free(s);
    int acceptable = SEQ_GE(seq, s->rcv_nxt)
        ? SEQ_LT(seq, s->rcv_nxt + (limit ? limit : 1))
        : SEQ_GT(seq + consumed, s->rcv_nxt) || !consumed;
    if (!acceptable && SEQ_LT(seq, s->rcv_nxt) && !consumed) acceptable = 1;
    if (!acceptable) {
        if (!(flags & RST)) emit(s, s->snd_nxt, ACK, 0, 0);
        return;
    }
    if (flags & RST) {
        drop_from_parent(s);
        to_closed(s, s->state == TCP_SYN_RCVD && s->passive ? DOSXC_ECONNREFUSED : DOSXC_ECONNRESET);
        return;
    }
    if (flags & SYN) {
        emit(s, s->snd_nxt, ACK, 0, 0);
        return;
    }
    if (!(flags & ACK)) return;
    if (s->state == TCP_SYN_RCVD) {
        if (ack != s->iss + 1) { send_reset(destination, source, dport, sport, ack, 0, RST); return; }
        s->state = TCP_ESTABLISHED;
        s->snd_una = ack;
        s->snd_wnd = window;
        s->snd_wl1 = seq;
        s->snd_wl2 = ack;
        s->rto_at = 0;
        s->retries = 0;
    }
    process_ack(s, ack, seq, window, data_length, flags);
    if (!s->used || s->state == TCP_CLOSED) return;
    int in_order = 0, need_ack = 0;
    if (data_length && (s->state == TCP_ESTABLISHED || s->state == TCP_FIN_WAIT_1 || s->state == TCP_FIN_WAIT_2)) {
        const u8 *payload = data;
        uptr size = data_length;
        u32 start = seq;
        if (SEQ_LT(start, s->rcv_nxt)) {
            u32 skip = s->rcv_nxt - start;
            if (skip >= size) { size = 0; }
            else { payload += skip; size -= skip; start += skip; }
        }
        if (size) store_data(s, start, payload, size, &in_order);
        need_ack = 1;
    }
    u32 fin_at = seq + (u32)data_length;
    if ((flags & FIN) && fin_at == s->rcv_nxt && !s->peer_fin) {
        s->peer_fin = 1;
        s->rcv_nxt += 1;
        need_ack = 1;
        switch (s->state) {
        case TCP_ESTABLISHED: s->state = TCP_CLOSE_WAIT; break;
        case TCP_FIN_WAIT_1: s->state = TCP_CLOSING; break;
        case TCP_FIN_WAIT_2:
            s->state = TCP_TIME_WAIT;
            s->expire_at = cpu_nanoseconds() + TIME_WAIT_NS;
            break;
        }
    }
    if (need_ack) emit(s, s->snd_nxt, ACK, 0, 0);
    output(s);
}

/* ---- timers ---------------------------------------------------------------- */

static void retransmit_timeout(struct socket *s) {
    s->rto_at = 0;
    s->rtt_active = 0;
    int limit = s->state == TCP_SYN_SENT || s->state == TCP_SYN_RCVD ? SYN_RETRIES : DATA_RETRIES;
    if (++s->retries > limit) {
        drop_from_parent(s);
        to_closed(s, DOSXC_ETIMEDOUT);
        return;
    }
    s->rto = s->rto * 2 > RTO_MAX ? RTO_MAX : s->rto * 2;
    if (s->state == TCP_SYN_SENT) {
        emit(s, s->iss, SYN, 0, 0);
        arm_retransmit(s, 1);
        return;
    }
    if (s->state == TCP_SYN_RCVD) {
        emit(s, s->iss, SYN | ACK, 0, 0);
        arm_retransmit(s, 1);
        return;
    }
    u32 flight = s->snd_nxt - s->snd_una;
    s->ssthresh = flight / 2 > 2 * s->mss ? flight / 2 : 2 * s->mss;
    s->cwnd = s->mss;
    s->dupacks = 0;
    s->snd_nxt = s->snd_una;                  /* go back and send again */
    s->fin_sent = 0;
    output(s);
    arm_retransmit(s, 1);
}

void tcp_timers(u64 now) {
    for (int i = 0; i < SOCKET_MAX; ++i) {
        struct socket *s = &sockets[i];
        if (!s->used || s->type != DOSXC_SOCK_STREAM || s->state == TCP_CLOSED || s->state == TCP_LISTEN) continue;
        if (s->rto_at && now >= s->rto_at) retransmit_timeout(s);
        if (!s->used || s->state == TCP_CLOSED) continue;
        if (s->persist_at && now >= s->persist_at) {        /* zero window: probe with one byte */
            if (s->tx_used && !s->snd_wnd && s->snd_nxt == s->snd_una) emit(s, s->snd_una, ACK, 0, 1);
            s->persist_interval = s->persist_interval * 2 > 5000000000ull ? 5000000000ull : s->persist_interval * 2;
            s->persist_at = s->tx_used && !s->snd_wnd ? now + s->persist_interval : 0;
        }
        if (s->expire_at && now >= s->expire_at) to_closed(s, 0);
    }
}

/* ---- calls from socket.c ------------------------------------------------------ */

int tcp_can_write(const struct socket *s) { return s->tx_used < SOCKET_BUFFER; }

i64 tcp_send(struct socket *s, const void *buffer, uptr size) {
    uptr room = SOCKET_BUFFER - s->tx_used;
    if (size > room) size = room;
    uptr at = (s->tx_head + s->tx_used) % SOCKET_BUFFER;
    uptr first = SOCKET_BUFFER - at;
    if (first > size) first = size;
    memcpy(s->tx + at, buffer, first);
    memcpy(s->tx, (const u8 *)buffer + first, size - first);
    s->tx_used += size;
    output(s);
    return (i64)size;
}

uptr tcp_receive(struct socket *s, void *buffer, uptr size, int peek) {
    if (size > s->rx_used) size = s->rx_used;
    uptr first = SOCKET_BUFFER - s->rx_head;
    if (first > size) first = size;
    memcpy(buffer, s->rx + s->rx_head, first);
    memcpy((u8 *)buffer + first, s->rx, size - first);
    if (!peek) {
        s->rx_head = (s->rx_head + size) % SOCKET_BUFFER;
        s->rx_used -= size;
        tcp_after_read(s);
    }
    return size;
}

/* The application made room: tell the sender when that matters. */
void tcp_after_read(struct socket *s) {
    u32 window = window_free(s);
    if (s->state != TCP_ESTABLISHED && s->state != TCP_FIN_WAIT_1 && s->state != TCP_FIN_WAIT_2) return;
    if (window >= s->advertised + 2 * s->mss || (!s->advertised && window)) emit(s, s->snd_nxt, ACK, 0, 0);
}

void tcp_shutdown_write(struct socket *s) {
    if (s->fin_queued) return;
    s->fin_queued = 1;
    if (s->state == TCP_ESTABLISHED) s->state = TCP_FIN_WAIT_1;
    else if (s->state == TCP_CLOSE_WAIT) s->state = TCP_LAST_ACK;
    output(s);
}

void tcp_close(struct socket *s) {
    s->closed = 1;
    switch (s->state) {
    case TCP_LISTEN:
        while (s->accept_count) {
            struct socket *c = s->accept_queue[--s->accept_count];
            c->parent = 0;
            c->closed = 1;
            emit(c, c->snd_nxt, RST | ACK, 0, 0);
            to_closed(c, 0);
        }
        to_closed(s, 0);
        return;
    case TCP_CLOSED: socket_free(s); return;
    case TCP_SYN_SENT: to_closed(s, 0); return;
    case TCP_SYN_RCVD:
    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
        if (s->rx_used) {                 /* the peer's data will never be read */
            emit(s, s->snd_nxt, RST | ACK, 0, 0);
            to_closed(s, 0);
            return;
        }
        tcp_shutdown_write(s);
        return;
    default:
        if (s->state == TCP_FIN_WAIT_2) s->expire_at = cpu_nanoseconds() + FIN_WAIT_NS;
        return;
    }
}
