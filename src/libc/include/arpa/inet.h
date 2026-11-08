/*
 * DOS-XC C library - arpa/inet.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#ifndef _ARPA_INET_H
#define _ARPA_INET_H
#include <netinet/in.h>
__BEGIN_DECLS
in_addr_t inet_addr(const char *text);
int inet_aton(const char *text, struct in_addr *out);
char *inet_ntoa(struct in_addr address);
int inet_pton(int family, const char *__restrict text, void *__restrict out);
const char *inet_ntop(int family, const void *__restrict address, char *__restrict out, socklen_t size);
__END_DECLS
#endif
