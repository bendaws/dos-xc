/*
 * DOS-XC /os/libc/include/resolv.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _RESOLV_H
#define _RESOLV_H
enum { ns_c_in = 1 };
enum { ns_t_a = 1, ns_t_ns = 2, ns_t_cname = 5, ns_t_txt = 16, ns_t_aaaa = 28 };
struct __res_state { int options; char defdname[256]; char *dnsrch[7]; };
typedef struct __res_state *res_state;
#ifdef __cplusplus
extern "C" {
#endif
int res_init(void);
int res_query(const char *, int, int, unsigned char *, int);
int res_search(const char *, int, int, unsigned char *, int);
#ifdef __cplusplus
}
#endif

#endif
