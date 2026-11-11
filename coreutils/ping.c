/*
 * DOS-XC /coreutils/ping.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "common.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

int main(int argc, char **argv) {
    int count = 4;
    const char *target = 0;
    for (int i = 1; i < argc; ++i) {
        if (same(argv[i], "-n") || same(argv[i], "/N")) { if (++i < argc) count = atoi(argv[i]); }
        else target = argv[i];
    }
    if (!target) { puts("PING [/N count] host"); return 1; }
    struct addrinfo *found;
    if (getaddrinfo(target, 0, 0, &found)) { printf("Cannot find %s.\n", target); return 1; }
    int six = found->ai_family == AF_INET6;
    unsigned char address6[16];
    unsigned ip = 0;
    char shown[INET6_ADDRSTRLEN];
    if (six) {
        memcpy(address6, &((struct sockaddr_in6 *)found->ai_addr)->sin6_addr, 16);
        inet_ntop(AF_INET6, address6, shown, sizeof(shown));
    } else {
        ip = ntohl(((struct sockaddr_in *)found->ai_addr)->sin_addr.s_addr);
        struct in_addr text = {htonl(ip)};
        inet_ntop(AF_INET, &text, shown, sizeof(shown));
    }
    freeaddrinfo(found);
    printf("Pinging %s [%s]\n", target, shown);
    int received = 0;
    for (int i = 0; i < count; ++i) {
        int r = six ? dosxc_sys_net(DOSXC_NET_PING6, 2000, address6) : dosxc_sys_net(DOSXC_NET_PING, (long)ip, (void *)2000);
        if (r >= 0) {
            printf("Reply from %s: time=%d.%03d ms\n", shown, r / 1000, r % 1000);
            ++received;
        } else if (r == -ETIMEDOUT) puts("Request timed out.");
        else printf("Failed: %s.\n", strerror(-r));
        if (i + 1 < count) dosxc_sys_sleep(1000000000ull);
    }
    printf("%d sent, %d received.\n", count, received);
    return received ? 0 : 1;
}
