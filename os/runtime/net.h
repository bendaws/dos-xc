/*
 * DOS-XC /os/runtime/net.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once
#include "runtime.h"
#include "usb.h"
#include "../libc/include/dosxc/driver.h"

#define NET_FRAME 1514
#define NET_ETH 14
#define NET_IP 20
#define NET_MSS (1500 - NET_IP - 20)
#define NET_L4 (NET_ETH + NET_IP)
#define NET_IP6 40
#define NET_MSS6 (1500 - NET_IP6 - 20)
#define NET_MAX_DEVICES 4
#define NET_TCP_OFF 34

#define PROTO_ICMP 1
#define PROTO_TCP 6
#define PROTO_UDP 17

static inline u16 get16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static inline u32 get32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static inline void put16(u8 *p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static inline void put32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

typedef struct { u8 b[16]; } net_addr;

static inline net_addr net_addr_v4(u32 ip) {
    net_addr a;
    memset(&a, 0, sizeof(a));
    a.b[10] = a.b[11] = 0xff;
    put32(a.b + 12, ip);
    return a;
}
static inline int net_addr_is_v4(const net_addr *a) {
    for (int i = 0; i < 10; ++i) if (a->b[i]) return 0;
    return a->b[10] == 0xff && a->b[11] == 0xff;
}
static inline u32 net_addr_get_v4(const net_addr *a) { return get32(a->b + 12); }
static inline int net_addr_equal(const net_addr *a, const net_addr *b) { return !memcmp(a->b, b->b, 16); }
/* The wildcard of either family: :: or ::ffff:0.0.0.0. */
static inline int net_addr_any(const net_addr *a) {
    if (a->b[12] | a->b[13] | a->b[14] | a->b[15]) return 0;
    if (net_addr_is_v4(a)) return 1;
    for (int i = 0; i < 12; ++i) if (a->b[i]) return 0;
    return 1;
}

/* ---- drivers.c --------------------------------------------------------- */
struct driver;
void drivers_init(void);
void drivers_poll(void);
void drivers_shutdown(void);
int drivers_active(void);
i64 driver_syscall(int operation, long a, void *buffer, dosxc_size size);
int driver_transmit(struct driver *d, const struct dk_net_ops *ops, void *instance, const void *frame, u32 length);
int drivers_fault(uptr vector, uptr ip);     /* from runtime_exception */
struct driver *driver_probing(void);

/* ---- net.c -------------------------------------------------------------- */
struct net_device;
void net_init(void);
void net_poll(void);                 /* do everything pending */
void net_tick(void);                 /* net_poll() at most every 250 us */
int net_active(void);
int net_register(const struct dk_net_ops *ops, void *instance, const u8 mac[6], const char *name);
void net_receive(int handle, const void *frame, u32 length);
void net_link(int handle, int up, u32 megabits);
void net_unregister(int handle);
int net_is_local(u32 ip);
u32 net_source_for(u32 destination);     /* our address towards DESTINATION; 0 without a route */
int net_ready(void);                     /* some adapter has an address */
u16 net_checksum(const void *data, uptr length, u32 start);
u32 net_pseudo(u32 source, u32 destination, u8 protocol, uptr length);
/* FRAME has room for the headers; PAYLOAD bytes start at frame + NET_L4. */
int net_output(u8 *frame, uptr payload, u8 protocol, u32 source, u32 destination);
i64 net_syscall_net(int operation, long a, void *b);
void net_device_send(int handle, u8 *frame, uptr length);
int net_loop_queue(const u8 *packet, uptr length);

void net6_init(void);
void net6_link(int handle, int up, const u8 *mac);
void net6_input(int handle, const u8 *packet, uptr length);   /* handle -1: loopback */
void net6_timers(u64 now);
i64 net6_syscall(int operation, long a, void *b);
int net_is_local_any(const net_addr *a);
int net_source(const net_addr *destination, net_addr *source);   /* 0 without a route */
u32 net_pseudo_any(const net_addr *source, const net_addr *destination, u8 protocol, uptr length);
/* Either family.  PAYLOAD bytes start at frame + NET_L4; the frame has room
   for the longer IPv6 header if the payload fits the MTU. */
int net_send(u8 *frame, uptr payload, u8 protocol, const net_addr *source, const net_addr *destination);

/* ---- sockets (socket.c, tcp.c) -------------------------------------------- */
enum tcp_state {
    TCP_CLOSED, TCP_LISTEN, TCP_SYN_SENT, TCP_SYN_RCVD, TCP_ESTABLISHED, TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2, TCP_CLOSE_WAIT, TCP_CLOSING, TCP_LAST_ACK, TCP_TIME_WAIT,
};
#define SOCKET_BUFFER 65535u
#define SOCKET_BACKLOG 16

struct socket {
    int used, type, closed;           /* closed: no descriptor refers to it any more */
    int error;                        /* errno to report, 0 when none */
    int reuse, read_shut, write_shut;
    int family, v6only;               /* DOSXC_AF_INET or DOSXC_AF_INET6 */
    net_addr local, remote;           /* IPv4 connections use mapped addresses */
    u16 local_port, remote_port;
    u64 receive_timeout, send_timeout;   /* nanoseconds, 0 = none */
    u8 *rx, *tx;
    uptr rx_head, rx_used, tx_head, tx_used;

    /* TCP */
    int state, passive;
    u32 snd_una, snd_nxt, snd_max, snd_wnd, snd_wl1, snd_wl2, iss;
    u32 rcv_nxt, irs, advertised;
    u32 mss, cwnd, ssthresh;
    u32 fin_seq;
    int fin_queued, fin_sent, peer_fin;
    u32 ooo_start, ooo_end;
    int ooo_used, dupacks, retries;
    u64 rto, srtt, rttvar;            /* nanoseconds */
    u64 rto_at, persist_at, persist_interval, expire_at, rtt_started;
    u32 rtt_seq;
    int rtt_active;

    /* listening sockets */
    struct socket *parent;
    struct socket *accept_queue[SOCKET_BACKLOG];
    int accept_count, backlog;
};

#define SOCKET_MAX 256
extern struct socket sockets[SOCKET_MAX];
struct socket *socket_new(int type, int family);
/* firewall.c */
int firewall_check(int inbound, int stream, u16 port, const net_addr *peer);   /* 0 or -EACCES */
void firewall_reload(void);
int firewall_enabled(void);
int socket_accepts(const struct socket *s, const net_addr *destination);
void socket_free(struct socket *s);
u16 socket_ephemeral_port(int type);
int socket_wait_interrupted(void);       /* Ctrl+C pending at a blocking call */

i64 socket_read(struct socket *s, void *buffer, uptr size, int flags, int nonblock);
i64 socket_write(struct socket *s, const void *buffer, uptr size, int flags, int nonblock);
void socket_close(struct socket *s);
int socket_poll(struct socket *s, int events);
i64 socket_syscall(uptr number, uptr a, uptr b, uptr c, uptr d, uptr e);

void udp_input(const net_addr *source, const net_addr *destination, const u8 *segment, uptr length);

void tcp_input(const net_addr *source, const net_addr *destination, const u8 *segment, uptr length);
void tcp_path_mtu(const net_addr *local, u16 local_port, const net_addr *remote, u16 remote_port, u32 mtu);
void tcp_timers(u64 now);
int tcp_connect(struct socket *s);
int tcp_listen(struct socket *s, int backlog);
struct socket *tcp_accept_take(struct socket *s);
i64 tcp_send(struct socket *s, const void *buffer, uptr size);
uptr tcp_receive(struct socket *s, void *buffer, uptr size, int peek);
void tcp_close(struct socket *s);
void tcp_shutdown_write(struct socket *s);
void tcp_after_read(struct socket *s);
int tcp_can_write(const struct socket *s);

/* ---- files.c ---------------------------------------------------------------- */
int files_socket_open(struct socket *s);
struct socket *files_socket(int fd, int *flags);
int files_set_flags(int fd, int flags);
