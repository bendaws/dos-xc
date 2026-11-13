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
#define IPPORT_RESERVED 1024
#define INADDR_BROADCAST ((in_addr_t)0xffffffff)
#define INADDR_NONE ((in_addr_t)0xffffffff)
#define INADDR_LOOPBACK ((in_addr_t)0x7f000001)
#define INET_ADDRSTRLEN 16
#define INET6_ADDRSTRLEN 46
#define IPV6_V6ONLY 26
extern const struct in6_addr in6addr_any, in6addr_loopback;
struct ip_mreq { struct in_addr imr_multiaddr, imr_interface; };
#define IN_MULTICAST(a) ((((unsigned)(a)) & 0xf0000000u) == 0xe0000000u)
#define IN_CLASSD(a) IN_MULTICAST(a)
#define IN6_IS_ADDR_MULTICAST(a) (((const unsigned char *)(a))[0] == 0xff)
static __inline int __in6_zeros(const void *a, int n) {
    const unsigned char *p = (const unsigned char *)a;
    for (int i = 0; i < n; ++i)
        if (p[i]) return 0;
    return 1;
}
#define IN6_IS_ADDR_UNSPECIFIED(a) __in6_zeros((a), 16)
#define IN6_IS_ADDR_LOOPBACK(a) (__in6_zeros((a), 15) && ((const unsigned char *)(a))[15] == 1)
#define IN6_IS_ADDR_LINKLOCAL(a) (((const unsigned char *)(a))[0] == 0xfe && (((const unsigned char *)(a))[1] & 0xc0) == 0x80)
#define IN6_IS_ADDR_SITELOCAL(a) (((const unsigned char *)(a))[0] == 0xfe && (((const unsigned char *)(a))[1] & 0xc0) == 0xc0)
#define IN6_IS_ADDR_V4MAPPED(a) (__in6_zeros((a), 10) && ((const unsigned char *)(a))[10] == 0xff && ((const unsigned char *)(a))[11] == 0xff)
#define IN_LOOPBACKNET 127
struct ip_mreqn { struct in_addr imr_multiaddr, imr_address; int imr_ifindex; };
struct ip_mreq_source { struct in_addr imr_multiaddr, imr_interface, imr_sourceaddr; };
struct ipv6_mreq { struct in6_addr ipv6mr_multiaddr; unsigned ipv6mr_interface; };
#ifndef IP_MULTICAST_IF
#define IP_MULTICAST_IF 32
#define IP_MULTICAST_TTL 33
#define IP_MULTICAST_LOOP 34
#define IP_ADD_MEMBERSHIP 35
#define IP_DROP_MEMBERSHIP 36
#define IP_ADD_SOURCE_MEMBERSHIP 39
#define IP_DROP_SOURCE_MEMBERSHIP 40
#define IPV6_MULTICAST_IF 17
#define IPV6_MULTICAST_HOPS 18
#define IPV6_MULTICAST_LOOP 19
#define IPV6_JOIN_GROUP 20
#define IPV6_LEAVE_GROUP 21
#define IPV6_ADD_MEMBERSHIP IPV6_JOIN_GROUP
#define IPV6_DROP_MEMBERSHIP IPV6_LEAVE_GROUP
#define IPV6_RECVPKTINFO 49
#define IPV6_PKTINFO 50
#define IPV6_RECVHOPLIMIT 51
#define IPV6_HOPLIMIT 52
struct in6_pktinfo { struct in6_addr ipi6_addr; unsigned ipi6_ifindex; };
#endif
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
