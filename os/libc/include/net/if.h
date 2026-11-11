/*
 * DOS-XC /os/libc/include/net/if.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _NET_IF_H
#define _NET_IF_H
#define IF_NAMESIZE 16
#define IFNAMSIZ IF_NAMESIZE
#ifdef __cplusplus
extern "C" {
#endif
unsigned if_nametoindex(const char *);
char *if_indextoname(unsigned, char *);
#ifdef __cplusplus
}
#endif
#endif
