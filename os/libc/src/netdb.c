/*
 * DOS-XC /os/libc/src/netdb.c
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

struct name_server { int family; unsigned char address[16]; };

static int is_zero(const unsigned char *a) {
    for (int i = 0; i < 16; ++i) if (a[i]) return 0;
    return 1;
}

static int dns_servers(struct name_server *out, int capacity) {
    int count = 0;
    struct dosxc_netinfo info;
    struct dosxc_netinfo6 info6;
    int adapters = dosxc_sys_net(DOSXC_NET_INFO, 0, 0);
    for (int a = 0; a < adapters && count < capacity; ++a) {
        if (dosxc_sys_net(DOSXC_NET_INFO, a, &info) < 0) continue;
        for (int i = 0; i < 2 && count < capacity; ++i)
            if (info.dns[i]) {
                uint32_t v = htonl(info.dns[i]);
                out[count].family = AF_INET;
                memcpy(out[count++].address, &v, 4);
            }
    }
    for (int a = 0; a < adapters && count < capacity; ++a) {
        if (dosxc_sys_net(DOSXC_NET_INFO6, a, &info6) < 0 || is_zero(info6.global)) continue;
        for (int i = 0; i < 2 && count < capacity; ++i)
            if (!is_zero(info6.dns[i])) {
                out[count].family = AF_INET6;
                memcpy(out[count++].address, info6.dns[i], 16);
            }
    }
    return count;
}

static int ipv6_usable(void) {
    struct dosxc_netinfo6 info6;
    int adapters = dosxc_sys_net(DOSXC_NET_INFO, 0, 0);
    for (int a = 0; a < adapters; ++a)
        if (dosxc_sys_net(DOSXC_NET_INFO6, a, &info6) >= 0 && !is_zero(info6.global)) return 1;
    return 0;
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

/* Query SERVER for records of TYPE (1 = A, 28 = AAAA) of NAME.  Addresses go
   in OUT, 16 bytes apart.  Returns how many (0 = no such records) or < 0
   (EAI_AGAIN style failure). */
static int query_server(const struct name_server *server, const char *name, int wanted, unsigned char *out, int capacity) {
    unsigned char packet[512], reply[1500], encoded[256];
    size_t name_size = encode_name(encoded, name);
    if (!name_size) return EAI_NONAME;
    unsigned id = (unsigned)(dosxc_sys_nanoseconds() >> 8) & 0xffff;
    memset(packet, 0, sizeof(packet));
    packet[0] = (unsigned char)(id >> 8);
    packet[1] = (unsigned char)id;
    packet[2] = 1;
    packet[5] = 1;
    memcpy(packet + 12, encoded, name_size);
    packet[12 + name_size + 1] = (unsigned char)wanted;
    packet[12 + name_size + 3] = 1;
    size_t packet_size = 12 + name_size + 4;
    int fd = socket(server->family, SOCK_DGRAM, 0);
    if (fd < 0) return EAI_SYSTEM;
    struct sockaddr_storage address;
    socklen_t address_size;
    memset(&address, 0, sizeof(address));
    if (server->family == AF_INET6) {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&address;
        a->sin6_family = AF_INET6;
        a->sin6_port = htons(53);
        memcpy(&a->sin6_addr, server->address, 16);
        address_size = sizeof(*a);
    } else {
        struct sockaddr_in *a = (struct sockaddr_in *)&address;
        a->sin_family = AF_INET;
        a->sin_port = htons(53);
        memcpy(&a->sin_addr, server->address, 4);
        address_size = sizeof(*a);
    }
    int result = EAI_AGAIN;
    for (int attempt = 0; attempt < 3 && result == EAI_AGAIN; ++attempt) {
        if (sendto(fd, packet, packet_size, 0, (struct sockaddr *)&address, address_size) < 0) continue;
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
                if (type == wanted && length == (wanted == 1 ? 4u : 16u) && found < capacity)
                    memcpy(out + (size_t)found++ * 16, reply + at, length);
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

/* Ask each server in turn.  Returns as query_server does. */
static int query(const char *name, int type, unsigned char *out, int capacity) {
    struct name_server servers[8];
    int count = dns_servers(servers, 8);
    if (!count) return EAI_AGAIN;
    int last = EAI_AGAIN;
    for (int i = 0; i < count; ++i) {
        int r = query_server(&servers[i], name, type, out, capacity);
        if (r >= 0) return r;
        last = r;
    }
    return last;
}

/* IPv4 addresses of NAME in host byte order, or a negative EAI_* value. */
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
    unsigned char raw[8 * 16];
    if (capacity > 8) capacity = 8;
    int r = query(name, 1, raw, capacity);
    if (r < 0) return r;
    if (!r) return EAI_NONAME;
    for (int i = 0; i < r; ++i) {
        uint32_t v;
        memcpy(&v, raw + i * 16, 4);
        out[i] = ntohl(v);
    }
    return r;
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
    if (family != AF_UNSPEC && family != AF_INET && family != AF_INET6) return EAI_FAMILY;
    if (socktype && socktype != SOCK_STREAM && socktype != SOCK_DGRAM) return EAI_SOCKTYPE;
    long port = service_port(service, flags & AI_NUMERICSERV);
    if (port < 0) return EAI_SERVICE;
    if (!node && !service) return EAI_NONAME;
    struct { int family; unsigned char address[16]; } found[16];
    int count = 0;
#define ADD(f, bytes, n) do { found[count].family = (f); memset(found[count].address, 0, 16); \
        memcpy(found[count++].address, (bytes), (n)); } while (0)
    if (!node) {
        if (family == AF_INET6) {
            ADD(AF_INET6, flags & AI_PASSIVE ? &in6addr_any : &in6addr_loopback, 16);
        } else {
            uint32_t v = htonl(flags & AI_PASSIVE ? INADDR_ANY : INADDR_LOOPBACK);
            ADD(AF_INET, &v, 4);
        }
    } else {
        char literal[64];
        size_t n = strcspn(node, "%");
        unsigned char six[16];
        struct in_addr a;
        if (n >= sizeof(literal)) n = sizeof(literal) - 1;
        memcpy(literal, node, n);
        literal[n] = 0;
        if (inet_pton(AF_INET6, literal, six) == 1) {
            if (family == AF_INET) return EAI_NONAME;
            ADD(AF_INET6, six, 16);
        } else if (inet_aton(node, &a)) {
            if (family == AF_INET6) return EAI_NONAME;
            ADD(AF_INET, &a, 4);
        } else if (flags & AI_NUMERICHOST) {
            return EAI_NONAME;
        } else if (!strcasecmp(node, "localhost")) {
            if (family == AF_INET6) {
                ADD(AF_INET6, &in6addr_loopback, 16);
            } else {
                uint32_t v = htonl(INADDR_LOOPBACK);
                ADD(AF_INET, &v, 4);
            }
        } else {
            unsigned char raw[8 * 16];
            int error = 0, r;
            if (family == AF_INET6 || (family == AF_UNSPEC && ipv6_usable())) {
                r = query(node, 28, raw, 8);
                if (r < 0) error = r;
                for (int i = 0; i < r; ++i) ADD(AF_INET6, raw + i * 16, 16);
            }
            if (family != AF_INET6) {
                r = query(node, 1, raw, 8);
                if (r < 0 && !error) error = r;
                for (int i = 0; i < r; ++i) ADD(AF_INET, raw + i * 16, 4);
            }
            if (!count) return error ? error : EAI_NONAME;
        }
    }
#undef ADD
    static const int types[2] = {SOCK_STREAM, SOCK_DGRAM};
    struct addrinfo *head = 0, **tail = &head;
    for (int i = 0; i < count; ++i)
        for (int t = 0; t < 2; ++t) {
            if (socktype && socktype != types[t]) continue;
            struct { struct addrinfo info; union { struct sockaddr_in v4; struct sockaddr_in6 v6; } address; } *block = calloc(1, sizeof(*block));
            if (!block) { freeaddrinfo(head); return EAI_MEMORY; }
            if (found[i].family == AF_INET6) {
                block->address.v6.sin6_family = AF_INET6;
                block->address.v6.sin6_port = htons((uint16_t)port);
                memcpy(&block->address.v6.sin6_addr, found[i].address, 16);
                block->info.ai_addrlen = sizeof(block->address.v6);
            } else {
                block->address.v4.sin_family = AF_INET;
                block->address.v4.sin_port = htons((uint16_t)port);
                memcpy(&block->address.v4.sin_addr, found[i].address, 4);
                block->info.ai_addrlen = sizeof(block->address.v4);
            }
            block->info.ai_family = found[i].family;
            block->info.ai_socktype = types[t];
            block->info.ai_protocol = protocol ? protocol : types[t] == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
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
    if (address->sa_family == AF_INET6 && length >= sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)address;
        if (host && host_size && !inet_ntop(AF_INET6, &in6->sin6_addr, host, host_size)) return EAI_SYSTEM;
        if (service && service_size) snprintf(service, service_size, "%u", ntohs(in6->sin6_port));
        return 0;
    }
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

/* gethostbyname for either family; AF_INET6 looks up AAAA records. */
struct hostent *gethostbyname2(const char *name, int family) {
    if (family == AF_INET) return gethostbyname(name);
    if (family != AF_INET6) { h_errno = NO_RECOVERY; return 0; }
    static unsigned char addresses[8][16];
    static char *list[9], *aliases[1], hostname[256];
    static struct hostent entry;
    int count;
    if (inet_pton(AF_INET6, name, addresses[0]) == 1) {
        count = 1;
    } else if (!strcasecmp(name, "localhost")) {
        memcpy(addresses[0], &in6addr_loopback, 16);
        count = 1;
    } else {
        count = query(name, 28, &addresses[0][0], 8);
        if (count <= 0) {
            h_errno = count == EAI_AGAIN ? TRY_AGAIN : HOST_NOT_FOUND;
            return 0;
        }
    }
    for (int i = 0; i < count; ++i) list[i] = (char *)addresses[i];
    list[count] = 0;
    snprintf(hostname, sizeof(hostname), "%s", name);
    entry.h_name = hostname;
    entry.h_aliases = aliases;
    entry.h_addrtype = AF_INET6;
    entry.h_length = 16;
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

struct hostent *gethostbyaddr(const void *address, socklen_t length, int type) {
    (void)address; (void)length; (void)type;
    h_errno = HOST_NOT_FOUND;
    return 0;
}

static struct protoent *protocol_entry(const char *name, int number) {
    static char *no_aliases[1];
    static struct protoent entry;
    static const struct { const char *name; int number; } known[] = {
        { "ip", 0 }, { "icmp", 1 }, { "tcp", 6 }, { "udp", 17 }, { "ipv6-icmp", 58 },
    };
    for (unsigned i = 0; i < sizeof known / sizeof known[0]; i++) {
        if (name ? strcmp(name, known[i].name) : number != known[i].number) continue;
        entry.p_name = (char *)known[i].name;
        entry.p_aliases = no_aliases;
        entry.p_proto = known[i].number;
        return &entry;
    }
    return 0;
}
struct protoent *getprotobyname(const char *name) { return protocol_entry(name, 0); }
struct protoent *getprotobynumber(int number) { return protocol_entry(0, number); }
