/*
 * DOS-XC C library - netdb.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#ifndef _NETDB_H
#define _NETDB_H
#include <netinet/in.h>

struct hostent {
    char *h_name;
    char **h_aliases;
    int h_addrtype, h_length;
    char **h_addr_list;
};
#define h_addr h_addr_list[0]
struct servent { char *s_name; char **s_aliases; int s_port; char *s_proto; };
struct addrinfo {
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;
    struct addrinfo *ai_next;
};
#define AI_PASSIVE 1
#define AI_CANONNAME 2
#define AI_NUMERICHOST 4
#define AI_NUMERICSERV 0x400
#define AI_ADDRCONFIG 0x20
#define NI_NUMERICHOST 1
#define NI_NUMERICSERV 2
#define NI_MAXHOST 1025
#define NI_MAXSERV 32
#define EAI_BADFLAGS (-1)
#define EAI_NONAME (-2)
#define EAI_AGAIN (-3)
#define EAI_FAIL (-4)
#define EAI_FAMILY (-6)
#define EAI_SOCKTYPE (-7)
#define EAI_SERVICE (-8)
#define EAI_MEMORY (-10)
#define EAI_SYSTEM (-11)
#define HOST_NOT_FOUND 1
#define TRY_AGAIN 2
#define NO_RECOVERY 3
#define NO_DATA 4
extern int h_errno;
__BEGIN_DECLS
struct hostent *gethostbyname(const char *name);
int getaddrinfo(const char *__restrict node, const char *__restrict service, const struct addrinfo *__restrict hints, struct addrinfo **__restrict result);
void freeaddrinfo(struct addrinfo *info);
const char *gai_strerror(int error);
int getnameinfo(const struct sockaddr *__restrict address, socklen_t length, char *__restrict host, socklen_t host_size, char *__restrict service, socklen_t service_size, int flags);
struct servent *getservbyname(const char *name, const char *protocol);
const char *hstrerror(int error);
__END_DECLS
#endif
