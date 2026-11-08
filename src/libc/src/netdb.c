/*
 * DOS-XC C library - name lookup: a DNS stub resolver over UDP using the
 * servers the adapters got from DHCP (or \SYSTEM\NET.CFG).  IPv4 only.
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#include "libc.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

int h_errno;

static int dns_servers(uint32_t *out, int capacity) {
    int count = 0;
    struct dosxc_netinfo info;
    int adapters = dosxc_sys_net(DOSXC_NET_INFO, 0, 0);
    for (int a = 0; a < adapters && count < capacity; ++a) {
        if (dosxc_sys_net(DOSXC_NET_INFO, a, &info) < 0) continue;
        for (int i = 0; i < 2 && count < capacity; ++i)
            if (info.dns[i]) out[count++] = info.dns[i];
    }
    return count;
}

static size_t encode_name(unsigned char *out, const char *name) {
    size_t at = 0;
    while (*name) {
        const char *dot = strchr(name, '.');
        size_t n = dot ? (size_t)(dot - name) : strlen(name);
        if (!n || n > 63 || at + n + 2 > 254) return 0;
        out[at++] = (unsigned char)n;
        memcpy(out + at, name, n);
        at += n;
        name += n;
        if (*name == '.') ++name;
    }
    out[at++] = 0;
    return at;
}

/* Skip a possibly compressed name; 0 when it runs off the end. */
static size_t skip_name(const unsigned char *m, size_t size, size_t at) {
    while (at < size) {
        unsigned char n = m[at];
        if (n >= 0xc0) return at + 2 <= size ? at + 2 : 0;
        if (!n) return at + 1;
        at += (size_t)n + 1;
    }
    return 0;
}

/* Query SERVER for A records of NAME.  Returns the number of addresses
   stored in OUT (0 = no such records) or < 0 (EAI_AGAIN style failure). */
static int query_server(uint32_t server, const char *name, uint32_t *out, int capacity) {
    unsigned char packet[512], reply[1500], encoded[256];
    size_t name_size = encode_name(encoded, name);
    if (!name_size) return EAI_NONAME;
    unsigned id = (unsigned)(dosxc_sys_nanoseconds() >> 8) & 0xffff;
    memset(packet, 0, sizeof(packet));
    packet[0] = (unsigned char)(id >> 8);
    packet[1] = (unsigned char)id;
    packet[2] = 1;                     /* recursion desired */
    packet[5] = 1;                     /* one question */
    memcpy(packet + 12, encoded, name_size);
    packet[12 + name_size + 1] = 1;    /* type A */
    packet[12 + name_size + 3] = 1;    /* class IN */
    size_t packet_size = 12 + name_size + 4;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return EAI_SYSTEM;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(53);
    address.sin_addr.s_addr = htonl(server);
    int result = EAI_AGAIN;
    for (int attempt = 0; attempt < 3 && result == EAI_AGAIN; ++attempt) {
        if (sendto(fd, packet, packet_size, 0, (struct sockaddr *)&address, sizeof(address)) < 0) continue;
        unsigned long long deadline = dosxc_sys_nanoseconds() + 2000000000ull;
        for (;;) {
            struct pollfd p = {fd, POLLIN, 0};
            long long left = (long long)(deadline - dosxc_sys_nanoseconds()) / 1000000;
            if (left <= 0 || poll(&p, 1, (int)left) <= 0) break;
            ssize_t n = recv(fd, reply, sizeof(reply), 0);
            if (n < 12 || reply[0] != (id >> 8 & 0xff) || reply[1] != (id & 0xff) || !(reply[2] & 0x80)) continue;
            int rcode = reply[3] & 15;
            if (rcode == 3) { result = 0; goto done; }
            if (rcode) { result = EAI_FAIL; goto done; }
            size_t at = 12;
            int questions = reply[4] << 8 | reply[5], answers = reply[6] << 8 | reply[7], found = 0;
            for (int q = 0; q < questions && at; ++q) {
                at = skip_name(reply, (size_t)n, at);
                if (at) at += 4;
            }
            for (int a = 0; a < answers && at && at < (size_t)n; ++a) {
                at = skip_name(reply, (size_t)n, at);
                if (!at || at + 10 > (size_t)n) break;
                int type = reply[at] << 8 | reply[at + 1];
                size_t length = (size_t)(reply[at + 8] << 8 | reply[at + 9]);
                at += 10;
                if (at + length > (size_t)n) break;
                if (type == 1 && length == 4 && found < capacity) {
                    uint32_t v;
                    memcpy(&v, reply + at, 4);
                    out[found++] = ntohl(v);
                }
                at += length;
            }
            result = found;
            goto done;
        }
    }
done:
    close(fd);
    return result;
}

/* Addresses of NAME in host byte order, or a negative EAI_* value. */
static int resolve(const char *name, uint32_t *out, int capacity) {
    struct in_addr literal;
    if (inet_aton(name, &literal)) {
        out[0] = ntohl(literal.s_addr);
        return 1;
    }
    if (!strcasecmp(name, "localhost")) {
        out[0] = INADDR_LOOPBACK;
        return 1;
    }
    uint32_t servers[4];
    int count = dns_servers(servers, 4);
    if (!count) return EAI_AGAIN;
    int last = EAI_AGAIN;
    for (int i = 0; i < count; ++i) {
        int r = query_server(servers[i], name, out, capacity);
        if (r > 0) return r;
        if (r == 0) return EAI_NONAME;
        last = r;
    }
    return last;
}

/* ---- getaddrinfo ------------------------------------------------------------ */

static long service_port(const char *service, int numeric) {
    if (!service || !*service) return 0;
    char *end;
    long port = strtol(service, &end, 10);
    if (!*end) return port >= 0 && port <= 65535 ? port : -1;
    if (numeric) return -1;
    if (!strcmp(service, "http")) return 80;
    if (!strcmp(service, "https")) return 443;
    if (!strcmp(service, "ftp")) return 21;
    if (!strcmp(service, "domain")) return 53;
    return -1;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **result) {
    int family = hints ? hints->ai_family : AF_UNSPEC;
    int socktype = hints ? hints->ai_socktype : 0, protocol = hints ? hints->ai_protocol : 0;
    int flags = hints ? hints->ai_flags : 0;
    *result = 0;
    if (family != AF_UNSPEC && family != AF_INET) return EAI_FAMILY;
    if (socktype && socktype != SOCK_STREAM && socktype != SOCK_DGRAM) return EAI_SOCKTYPE;
    long port = service_port(service, flags & AI_NUMERICSERV);
    if (port < 0) return EAI_SERVICE;
    if (!node && !service) return EAI_NONAME;
    uint32_t addresses[8];
    int count;
    if (!node) {
        addresses[0] = flags & AI_PASSIVE ? INADDR_ANY : INADDR_LOOPBACK;
        count = 1;
    } else if (flags & AI_NUMERICHOST) {
        struct in_addr a;
        if (!inet_aton(node, &a)) return EAI_NONAME;
        addresses[0] = ntohl(a.s_addr);
        count = 1;
    } else {
        count = resolve(node, addresses, 8);
        if (count < 0) return count;
        if (!count) return EAI_NONAME;
    }
    static const int types[2] = {SOCK_STREAM, SOCK_DGRAM};
    struct addrinfo *head = 0, **tail = &head;
    for (int i = 0; i < count; ++i)
        for (int t = 0; t < 2; ++t) {
            if (socktype && socktype != types[t]) continue;
            struct { struct addrinfo info; struct sockaddr_in address; } *block = calloc(1, sizeof(*block));
            if (!block) { freeaddrinfo(head); return EAI_MEMORY; }
            block->address.sin_family = AF_INET;
            block->address.sin_port = htons((uint16_t)port);
            block->address.sin_addr.s_addr = htonl(addresses[i]);
            block->info.ai_family = AF_INET;
            block->info.ai_socktype = types[t];
            block->info.ai_protocol = protocol ? protocol : types[t] == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
            block->info.ai_addrlen = sizeof(block->address);
            block->info.ai_addr = (struct sockaddr *)&block->address;
            if ((flags & AI_CANONNAME) && !head && node) block->info.ai_canonname = strdup(node);
            *tail = &block->info;
            tail = &block->info.ai_next;
        }
    *result = head;
    return 0;
}

void freeaddrinfo(struct addrinfo *info) {
    while (info) {
        struct addrinfo *next = info->ai_next;
        free(info->ai_canonname);
        free(info);
        info = next;
    }
}

const char *gai_strerror(int error) {
    switch (error) {
    case 0: return "Success";
    case EAI_BADFLAGS: return "Bad value for ai_flags";
    case EAI_NONAME: return "Name or service not known";
    case EAI_AGAIN: return "Temporary failure in name resolution";
    case EAI_FAIL: return "Non-recoverable failure in name resolution";
    case EAI_FAMILY: return "Address family not supported";
    case EAI_SOCKTYPE: return "Socket type not supported";
    case EAI_SERVICE: return "Service not known";
    case EAI_MEMORY: return "Out of memory";
    case EAI_SYSTEM: return strerror(errno);
    }
    return "Unknown error";
}

int getnameinfo(const struct sockaddr *address, socklen_t length, char *host, socklen_t host_size, char *service, socklen_t service_size, int flags) {
    (void)flags;
    if (address->sa_family != AF_INET || length < sizeof(struct sockaddr_in)) return EAI_FAMILY;
    const struct sockaddr_in *in = (const struct sockaddr_in *)address;
    if (host && host_size && !inet_ntop(AF_INET, &in->sin_addr, host, host_size)) return EAI_SYSTEM;
    if (service && service_size) snprintf(service, service_size, "%u", ntohs(in->sin_port));
    return 0;
}

struct hostent *gethostbyname(const char *name) {
    static uint32_t addresses[9];
    static char *list[9], *aliases[1], hostname[256];
    static struct hostent entry;
    uint32_t found[8];
    int count = resolve(name, found, 8);
    if (count <= 0) {
        h_errno = count == EAI_AGAIN ? TRY_AGAIN : HOST_NOT_FOUND;
        return 0;
    }
    for (int i = 0; i < count; ++i) {
        addresses[i] = htonl(found[i]);
        list[i] = (char *)&addresses[i];
    }
    list[count] = 0;
    snprintf(hostname, sizeof(hostname), "%s", name);
    entry.h_name = hostname;
    entry.h_aliases = aliases;
    entry.h_addrtype = AF_INET;
    entry.h_length = 4;
    entry.h_addr_list = list;
    return &entry;
}

struct servent *getservbyname(const char *name, const char *protocol) {
    static struct servent entry;
    static char *none[1];
    long port = service_port(name, 0);
    if (port < 0) return 0;
    entry.s_name = (char *)name;
    entry.s_aliases = none;
    entry.s_port = htons((uint16_t)port);
    entry.s_proto = (char *)protocol;
    return &entry;
}

const char *hstrerror(int error) {
    switch (error) {
    case HOST_NOT_FOUND: return "Unknown host";
    case TRY_AGAIN: return "Host name lookup failure";
    case NO_RECOVERY: return "Unknown server error";
    case NO_DATA: return "No address associated with name";
    }
    return "Resolver error";
}
