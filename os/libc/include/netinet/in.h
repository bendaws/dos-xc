/*
 * DOS-XC /os/libc/include/netinet/in.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _NETINET_IN_H
#define _NETINET_IN_H
#include <stdint.h>
#include <sys/socket.h>

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;
struct in_addr { in_addr_t s_addr; };
struct sockaddr_in {
    sa_family_t sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    unsigned char sin_zero[8];
};
struct in6_addr { unsigned char s6_addr[16]; };
struct sockaddr_in6 {
    sa_family_t sin6_family;
    in_port_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};

#define IPPROTO_IP 0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define IPPROTO_IPV6 41
#define IPPROTO_RAW 255
#define INADDR_ANY ((in_addr_t)0)
#define INADDR_BROADCAST ((in_addr_t)0xffffffff)
#define INADDR_NONE ((in_addr_t)0xffffffff)
#define INADDR_LOOPBACK ((in_addr_t)0x7f000001)
#define INET_ADDRSTRLEN 16
#define INET6_ADDRSTRLEN 46
#define IPV6_V6ONLY 26
extern const struct in6_addr in6addr_any, in6addr_loopback;
struct ip_mreq { struct in_addr imr_multiaddr, imr_interface; };
#define IP_MTU_DISCOVER 10
#define IP_PMTUDISC_DONT 0
#define IP_PMTUDISC_WANT 1
#define IP_PMTUDISC_DO 2
#define IP_PMTUDISC_PROBE 3
#define IP_TOS 1
#define IP_TTL 2

static __inline__ uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
static __inline__ uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }
static __inline__ uint16_t htons(uint16_t v) { return __builtin_bswap16(v); }
static __inline__ uint16_t ntohs(uint16_t v) { return __builtin_bswap16(v); }
#endif
