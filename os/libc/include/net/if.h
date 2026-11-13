/*
 * DOS-XC /os/libc/include/net/if.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _NET_IF_H
#define _NET_IF_H
#define IF_NAMESIZE 16
#define IFNAMSIZ IF_NAMESIZE
#define IFF_UP 0x1
#define IFF_BROADCAST 0x2
#define IFF_LOOPBACK 0x8
#define IFF_POINTOPOINT 0x10
#define IFF_RUNNING 0x40
#define IFF_MULTICAST 0x1000
#ifdef __cplusplus
extern "C" {
#endif
unsigned if_nametoindex(const char *);
char *if_indextoname(unsigned, char *);
#ifdef __cplusplus
}
#endif
#endif
