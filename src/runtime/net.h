/*
 * DOS-XC /runtime/net.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * The network stack: drivers.c loads the adapters' drivers, net.c does
 * Ethernet, ARP, IPv4, ICMP, DHCP and UDP, tcp.c does TCP, and socket.c
 * connects all of it to the system calls.  Nothing is interrupt driven:
 * net_poll() runs from system calls and from every loop that waits.
 * IPv4 addresses are in host byte order everywhere in here.
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
#define NET_TCP_OFF 34

#define PROTO_ICMP 1
#define PROTO_TCP 6
#define PROTO_UDP 17

static inline u16 get16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static inline u32 get32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static inline void put16(u8 *p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static inline void put32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

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
    u32 local_ip, remote_ip;
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

#define SOCKET_MAX 64
extern struct socket sockets[SOCKET_MAX];
struct socket *socket_new(int type);
void socket_free(struct socket *s);
u16 socket_ephemeral_port(int type);
int socket_wait_interrupted(void);       /* Ctrl+C pending at a blocking call */

i64 socket_read(struct socket *s, void *buffer, uptr size, int flags, int nonblock);
i64 socket_write(struct socket *s, const void *buffer, uptr size, int flags, int nonblock);
void socket_close(struct socket *s);
int socket_poll(struct socket *s, int events);
i64 socket_syscall(uptr number, uptr a, uptr b, uptr c, uptr d, uptr e);

void udp_input(u32 source, u32 destination, const u8 *segment, uptr length);

void tcp_input(u32 source, u32 destination, const u8 *segment, uptr length);
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
