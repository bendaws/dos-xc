/*
 * DOS-XC coreutils - PING: ICMP echo.
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#include "common.h"
#include <arpa/inet.h>
#include <netdb.h>

int main(int argc, char **argv) {
    int count = 4;
    const char *target = 0;
    for (int i = 1; i < argc; ++i) {
        if (same(argv[i], "-n") || same(argv[i], "/N")) { if (++i < argc) count = atoi(argv[i]); }
        else target = argv[i];
    }
    if (!target) { puts("PING [/N count] host"); return 1; }
    struct hostent *host = gethostbyname(target);
    if (!host) { printf("Cannot find %s.\n", target); return 1; }
    unsigned ip = ntohl(*(unsigned *)host->h_addr_list[0]);
    struct in_addr shown = {htonl(ip)};
    printf("Pinging %s [%s]\n", target, inet_ntoa(shown));
    int received = 0;
    for (int i = 0; i < count; ++i) {
        int r = dosxc_sys_net(DOSXC_NET_PING, (long)ip, (void *)2000);
        if (r >= 0) {
            printf("Reply from %s: time=%d.%03d ms\n", inet_ntoa(shown), r / 1000, r % 1000);
            ++received;
        } else if (r == -ETIMEDOUT) puts("Request timed out.");
        else printf("Failed: %s.\n", strerror(-r));
        if (i + 1 < count) dosxc_sys_sleep(1000000000ull);
    }
    printf("%d sent, %d received.\n", count, received);
    return received ? 0 : 1;
}
