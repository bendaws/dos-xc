/*
 * DOS-XC /os/libc/include/netinet/ip.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _NETINET_IP_H
#define _NETINET_IP_H
#include <netinet/in.h>
#include <netinet/in_systm.h>
#define IPVERSION 4
#define IP_MAXPACKET 65535
#define IPTOS_TOS_MASK 0x1e
#define IPTOS_LOWDELAY 0x10
#define IPTOS_THROUGHPUT 0x08
#define IPTOS_RELIABILITY 0x04
#define IPTOS_MINCOST 0x02
#define IPTOS_DSCP_MASK 0xfc
#define IPTOS_DSCP_AF11 0x28
#define IPTOS_DSCP_AF12 0x30
#define IPTOS_DSCP_AF13 0x38
#define IPTOS_DSCP_AF21 0x48
#define IPTOS_DSCP_AF22 0x50
#define IPTOS_DSCP_AF23 0x58
#define IPTOS_DSCP_AF31 0x68
#define IPTOS_DSCP_AF32 0x70
#define IPTOS_DSCP_AF33 0x78
#define IPTOS_DSCP_AF41 0x88
#define IPTOS_DSCP_AF42 0x90
#define IPTOS_DSCP_AF43 0x98
#define IPTOS_DSCP_CS0 0x00
#define IPTOS_DSCP_CS1 0x20
#define IPTOS_DSCP_CS2 0x40
#define IPTOS_DSCP_CS3 0x60
#define IPTOS_DSCP_CS4 0x80
#define IPTOS_DSCP_CS5 0xa0
#define IPTOS_DSCP_CS6 0xc0
#define IPTOS_DSCP_CS7 0xe0
#define IPTOS_DSCP_EF 0xb8
#define IPTOS_DSCP_LE 0x04
/* An IPv4 header. */
struct ip {
    uint8_t ip_vhl;              /* version (high 4 bits) and header length in words */
    uint8_t ip_tos;
    uint16_t ip_len, ip_id, ip_off;
    uint8_t ip_ttl, ip_p;
    uint16_t ip_sum;
    struct in_addr ip_src, ip_dst;
};
#endif
