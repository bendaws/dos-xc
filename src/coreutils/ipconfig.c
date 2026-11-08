/*
 * DOS-XC coreutils - IPCONFIG: show or change the network settings.
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#include "common.h"
#include <arpa/inet.h>

static void dotted(char *out, unsigned ip) {
    snprintf(out, 16, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255);
}

static int show(void) {
    struct dosxc_netinfo info;
    int adapters = dosxc_sys_net(DOSXC_NET_INFO, 0, 0);
    if (adapters <= 0) {
        puts("No network adapter is active.  Install a driver with PKG, or see DRIVERS /LOG.");
        return 1;
    }
    for (int i = 0; i < adapters; ++i) {
        if (dosxc_sys_net(DOSXC_NET_INFO, i, &info) < 0) continue;
        char ip[16], mask[16], gateway[16], dns[16], dns2[16];
        dotted(ip, info.ip);
        dotted(mask, info.mask);
        dotted(gateway, info.gateway);
        dotted(dns, info.dns[0]);
        dotted(dns2, info.dns[1]);
        printf("Adapter %d: %s\n", i, info.name);
        printf("   MAC address . . : %02X:%02X:%02X:%02X:%02X:%02X\n", info.mac[0], info.mac[1], info.mac[2], info.mac[3], info.mac[4], info.mac[5]);
        if (!info.link) { puts("   Link  . . . . . : down"); continue; }
        printf("   Link  . . . . . : up, %u Mbit/s\n", info.speed);
        printf("   Configured by . : %s\n", info.dhcp ? "DHCP" : "static settings");
        printf("   IP address  . . : %s\n", info.ip ? ip : "(waiting for DHCP)");
        printf("   Subnet mask . . : %s\n", mask);
        printf("   Gateway . . . . : %s\n", gateway);
        printf("   DNS servers . . : %s  %s\n", dns, dns2);
        if (info.dhcp) printf("   Lease remaining : %u s\n", info.lease);
        printf("   Packets . . . . : %llu received, %llu sent, %llu dropped\n", (unsigned long long)info.rx_packets, (unsigned long long)info.tx_packets, (unsigned long long)info.rx_dropped);
    }
    return 0;
}

static int parse(const char *text, unsigned *out) {
    struct in_addr a;
    if (!inet_aton(text, &a)) { printf("%s is not an IP address.\n", text); return -1; }
    *out = ntohl(a.s_addr);
    return 0;
}

static void config_path(char *out, size_t size) {
    struct dosxc_sysinfo info;
    dosxc_sys_sysinfo(&info);
    snprintf(out, size, "%c:\\SYSTEM\\NET.CFG", info.drive);
}

int main(int argc, char **argv) {
    if (argc < 2) return show();
    if (same(argv[1], "/?") || same(argv[1], "/HELP")) {
        puts("IPCONFIG                         Shows the adapters\n"
             "IPCONFIG /RENEW                  Asks the DHCP server again\n"
             "IPCONFIG /DHCP                   Uses DHCP from now on\n"
             "IPCONFIG /STATIC ip mask gateway [dns]   Uses fixed settings from now on");
        return 0;
    }
    if (same(argv[1], "/RENEW")) {
        int r = dosxc_sys_net(DOSXC_NET_RENEW, 0, 0);
        if (r < 0) { printf("Cannot renew: %s.\n", strerror(-r)); return 1; }
        puts("DHCP restarted.");
        return 0;
    }
    char path[32];
    config_path(path, sizeof(path));
    if (same(argv[1], "/DHCP")) {
        struct dosxc_netconfig config = {1, 0, 0, 0, {0, 0}};
        unlink(path);
        int r = dosxc_sys_net(DOSXC_NET_CONFIG, 0, &config);
        if (r < 0) { printf("Cannot change the settings: %s.\n", strerror(-r)); return 1; }
        puts("Using DHCP.");
        return 0;
    }
    if (same(argv[1], "/STATIC") && argc >= 5) {
        struct dosxc_netconfig config = {0, 0, 0, 0, {0, 0}};
        if (parse(argv[2], &config.ip) || parse(argv[3], &config.mask) || parse(argv[4], &config.gateway)) return 1;
        if (argc > 5 && parse(argv[5], &config.dns[0])) return 1;
        FILE *f = fopen(path, "wb");
        if (!f) { printf("Cannot write %s.\n", path); return 1; }
        fprintf(f, "IP=%s\r\nMASK=%s\r\nGATEWAY=%s\r\n", argv[2], argv[3], argv[4]);
        if (argc > 5) fprintf(f, "DNS=%s\r\n", argv[5]);
        fclose(f);
        int r = dosxc_sys_net(DOSXC_NET_CONFIG, 0, &config);
        if (r < 0) { printf("Saved %s, but: %s.\n", path, strerror(-r)); return 1; }
        puts("Static settings applied and saved.");
        return 0;
    }
    puts("Unknown option; see IPCONFIG /?");
    return 1;
}
