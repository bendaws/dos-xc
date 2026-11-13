/*
 * DOS-XC /desktop/netpanel.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "wm.h"

enum { F_IP, F_MASK, F_GATEWAY, F_DNS1, F_DNS2, FIELDS };

struct net_panel {
    struct dosxc_netinfo info;
    struct dosxc_netinfo6 info6;
    int adapters, ipv4, ipv6, dhcp, fixed_dns, focus;
    struct field fields[FIELDS];
};
static struct window *net_window;

static const char *const field_labels[FIELDS] = {"Address", "Subnet mask", "Gateway", "DNS server", "Second DNS"};
static struct rect field_rect(int i) { return (struct rect){120 + (i >= F_DNS1 ? 0 : 0), 192 + i * 26, 150, 20}; }
static const struct rect ipv4_box = {12, 140, 14, 14}, dhcp_radio = {32, 162, 12, 12}, fixed_radio = {200, 162, 12, 12};
static const struct rect dns_box = {290, 244, 14, 14}, ipv6_box = {12, 336, 14, 14};
static const struct rect apply_rect = {290, 318, 80, 24}, renew_rect = {376, 318, 80, 24}, refresh_rect = {462, 318, 80, 24};

static void ip_text(dosxc_u32 ip, char *out, size_t size) {
    if (!ip) { snprintf(out, size, "%s", ""); return; }
    snprintf(out, size, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255);
}

static int ip_parse(const char *s, dosxc_u32 *out) {
    unsigned a, b, c, d;
    char extra;
    if (!*s) { *out = 0; return 0; }
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return -1;
    *out = a << 24 | b << 16 | c << 8 | d;
    return 0;
}

static void ip6_text(const unsigned char *a, char *out, size_t size) {
    int zero = 1;
    for (int i = 0; i < 16; ++i) zero &= !a[i];
    if (zero) { snprintf(out, size, "none"); return; }
    size_t n = 0;
    for (int i = 0; i < 16 && n + 6 < size; i += 2) n += (size_t)snprintf(out + n, size - n, "%s%x", i ? ":" : "", a[i] << 8 | a[i + 1]);
}

static void load(struct net_panel *p) {
    p->adapters = dosxc_sys_net(DOSXC_NET_INFO, 0, 0);
    if (p->adapters <= 0) return;
    dosxc_sys_net(DOSXC_NET_INFO, 0, &p->info);
    memset(&p->info6, 0, sizeof(p->info6));
    dosxc_sys_net(DOSXC_NET_INFO6, 0, &p->info6);
    p->ipv4 = !(p->info.flags & DOSXC_NETCFG_IPV4_OFF);
    p->ipv6 = !(p->info.flags & DOSXC_NETCFG_IPV6_OFF);
    p->dhcp = p->info.dhcp || !p->ipv4;
    p->fixed_dns = p->dhcp ? (p->info.flags & DOSXC_NETCFG_DNS_FIXED) != 0 : 1;
    char text[32];
    const dosxc_u32 values[FIELDS] = {p->info.ip, p->info.mask, p->info.gateway, p->info.dns[0], p->info.dns[1]};
    for (int i = 0; i < FIELDS; ++i) {
        ip_text(values[i], text, sizeof(text));
        field_set(&p->fields[i], text);
    }
}

static void radio(const struct canvas *c, struct rect r, int on) {
    fill(c, r.x, r.y, r.w, r.h, C_BASE);
    frame_box(c, r.x, r.y, r.w, r.h, C_SHADOW2);
    if (on) fill(c, r.x + 3, r.y + 3, r.w - 6, r.h - 6, C_TEXT);
}

static void checkbox(const struct canvas *c, struct rect r, int on) {
    bevel(c, r.x, r.y, r.w, r.h, 1);
    fill(c, r.x + 2, r.y + 2, r.w - 4, r.h - 4, C_BASE);
    if (on)
        for (int i = 0; i < 4; ++i) {
            fill(c, r.x + 3 + i, r.y + 6 + i, 1, 2, C_TEXT);
            fill(c, r.x + 7 + i, r.y + 8 - i, 1, 2, C_TEXT);
        }
}

static int field_enabled(const struct net_panel *p, int i) {
    if (!p->ipv4) return 0;
    return i >= F_DNS1 ? p->fixed_dns : !p->dhcp;
}

static void np_paint(struct window *w, const struct canvas *c) {
    struct net_panel *p = w->data;
    if (p->adapters <= 0) {
        text(c, 12, 12, &font_bold, "No network adapter was found.", C_TEXT);
        text(c, 12, 32, &font_regular, "Install the ethernet package to add drivers.", C_TEXT);
        return;
    }
    char line[160], a[24], b[24], d1[24], d2[24];
    snprintf(line, sizeof(line), "%s: %s%s", p->info.name, p->info.link ? "connected" : "no cable",
             p->info.link && p->info.speed ? "" : "");
    text(c, 12, 10, &font_bold, line, C_TEXT);
    if (p->info.link && p->info.speed) { snprintf(line, sizeof(line), "%u Mbit/s", p->info.speed); text(c, 300, 10, &font_regular, line, C_TEXT); }
    ip_text(p->info.ip, a, sizeof(a));
    ip_text(p->info.gateway, b, sizeof(b));
    snprintf(line, sizeof(line), "IPv4: %s%s%s", p->info.flags & DOSXC_NETCFG_IPV4_OFF ? "off" : a[0] ? a : "waiting for an address",
             b[0] ? ", gateway " : "", b);
    text(c, 12, 30, &font_regular, line, C_TEXT);
    ip_text(p->info.dns[0], d1, sizeof(d1));
    ip_text(p->info.dns[1], d2, sizeof(d2));
    snprintf(line, sizeof(line), "DNS: %s%s%s", d1[0] ? d1 : "none", d2[0] ? ", " : "", d2);
    text(c, 12, 46, &font_regular, line, C_TEXT);
    if (p->info.dhcp && p->info.lease) {
        snprintf(line, sizeof(line), "DHCP lease: %u minutes left", p->info.lease / 60);
        text(c, 300, 30, &font_regular, line, C_TEXT);
    }
    char six[64];
    ip6_text(p->info6.global, six, sizeof(six));
    snprintf(line, sizeof(line), "IPv6: %s", p->info.flags & DOSXC_NETCFG_IPV6_OFF ? "off" : six);
    text(c, 12, 62, &font_regular, line, C_TEXT);
    ip6_text(p->info6.link_local, six, sizeof(six));
    snprintf(line, sizeof(line), "Link-local: %s", six);
    text(c, 12, 78, &font_regular, line, C_TEXT);
    snprintf(line, sizeof(line), "Received %llu packets, sent %llu", (unsigned long long)p->info.rx_packets, (unsigned long long)p->info.tx_packets);
    text(c, 12, 94, &font_regular, line, C_SHADOW1);
    fill(c, 12, 124, w->r.w - 24, 1, C_SHADOW1);

    checkbox(c, ipv4_box, p->ipv4);
    text(c, 32, 141, &font_bold, "IPv4", C_TEXT);
    u32 dim = p->ipv4 ? C_TEXT : C_DISABLED;
    radio(c, dhcp_radio, p->dhcp);
    text(c, 50, 162, &font_regular, "Automatic (DHCP)", dim);
    radio(c, fixed_radio, !p->dhcp);
    text(c, 218, 162, &font_regular, "Fixed address", dim);
    for (int i = 0; i < FIELDS; ++i) {
        struct rect r = field_rect(i);
        int on = field_enabled(p, i);
        text(c, 32, r.y + 4, &font_regular, field_labels[i], on ? C_TEXT : C_DISABLED);
        if (on) field_paint(c, &p->fields[i], r, p->focus == i);
        else {
            bevel(c, r.x, r.y, r.w, r.h, 1);
            fill(c, r.x + 2, r.y + 2, r.w - 4, r.h - 4, C_WINDOW);
            text(c, r.x + 4, r.y + 4, &font_regular, p->fields[i].text, C_DISABLED);
        }
    }
    checkbox(c, dns_box, p->fixed_dns);
    text(c, 310, 244, &font_regular, p->dhcp ? "Use these DNS servers" : "DNS servers (needed)", p->ipv4 && p->dhcp ? C_TEXT : C_DISABLED);
    checkbox(c, ipv6_box, p->ipv6);
    text(c, 32, 337, &font_bold, "IPv6", C_TEXT);
    text(c, 74, 337, &font_regular, "(addresses set up automatically)", C_TEXT);
    button(c, apply_rect, "Apply", 0, 1);
    button(c, renew_rect, "Renew", 0, 0);
    button(c, refresh_rect, "Refresh", 0, 0);
}

static int save_config(const struct dosxc_netconfig *config) {
    struct dosxc_sysinfo si;
    dosxc_sys_sysinfo(&si);
    char path[32];
    snprintf(path, sizeof(path), "%c:\\SYSTEM\\NET.CFG", si.drive);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    char a[24];
    if (config->flags & DOSXC_NETCFG_IPV4_OFF) fprintf(f, "IPV4=OFF\r\n");
    else if (config->dhcp) {
        if (config->flags & DOSXC_NETCFG_DNS_FIXED) {
            fprintf(f, "MODE=DHCP\r\n");
            for (int i = 0; i < 2; ++i)
                if (config->dns[i]) { ip_text(config->dns[i], a, sizeof(a)); fprintf(f, "DNS=%s\r\n", a); }
        }
    } else {
        const char *const keys[3] = {"IP", "MASK", "GATEWAY"};
        const dosxc_u32 values[3] = {config->ip, config->mask, config->gateway};
        for (int i = 0; i < 3; ++i)
            if (values[i]) { ip_text(values[i], a, sizeof(a)); fprintf(f, "%s=%s\r\n", keys[i], a); }
        for (int i = 0; i < 2; ++i)
            if (config->dns[i]) { ip_text(config->dns[i], a, sizeof(a)); fprintf(f, "DNS=%s\r\n", a); }
    }
    if (config->flags & DOSXC_NETCFG_IPV6_OFF) fprintf(f, "IPV6=OFF\r\n");
    return fclose(f);
}

static void apply(struct window *w) {
    struct net_panel *p = w->data;
    struct dosxc_netconfig config;
    memset(&config, 0, sizeof(config));
    config.dhcp = p->dhcp;
    if (!p->ipv4) config.flags |= DOSXC_NETCFG_IPV4_OFF;
    if (!p->ipv6) config.flags |= DOSXC_NETCFG_IPV6_OFF;
    if (p->dhcp && p->fixed_dns) config.flags |= DOSXC_NETCFG_DNS_FIXED;
    dosxc_u32 *targets[FIELDS] = {&config.ip, &config.mask, &config.gateway, &config.dns[0], &config.dns[1]};
    for (int i = 0; i < FIELDS; ++i)
        if (field_enabled(p, i) && ip_parse(p->fields[i].text, targets[i])) {
            message_box("Network", "That is not an IPv4 address:", p->fields[i].text);
            return;
        }
    if (p->ipv4 && !p->dhcp && (!config.ip || !config.mask)) { message_box("Network", "A fixed address needs an address and a subnet mask.", ""); return; }
    if (p->dhcp && p->fixed_dns && !config.dns[0]) { message_box("Network", "Enter a DNS server, or untick Use these DNS servers.", ""); return; }
    if (dosxc_sys_net(DOSXC_NET_CONFIG, 0, &config) < 0) {
        message_box("Network", "The settings could not be changed.", "Only administrators can change network settings.");
        return;
    }
    if (save_config(&config)) message_box("Network", "The settings were applied but could not be saved.", "They last until the computer restarts.");
    load(p);
    window_paint(w);
}

static void np_mouse(struct window *w, int x, int y, int kind, int value) {
    (void)value;
    struct net_panel *p = w->data;
    if (kind != M_DOWN || p->adapters <= 0) return;
    if (rect_has((struct rect){ipv4_box.x, ipv4_box.y, 60, 16}, x, y)) p->ipv4 = !p->ipv4;
    else if (p->ipv4 && rect_has((struct rect){dhcp_radio.x, dhcp_radio.y, 140, 14}, x, y)) p->dhcp = 1;
    else if (p->ipv4 && rect_has((struct rect){fixed_radio.x, fixed_radio.y, 120, 14}, x, y)) { p->dhcp = 0; p->fixed_dns = 1; }
    else if (p->ipv4 && p->dhcp && rect_has((struct rect){dns_box.x, dns_box.y, 170, 16}, x, y)) p->fixed_dns = !p->fixed_dns;
    else if (rect_has((struct rect){ipv6_box.x, ipv6_box.y, 60, 16}, x, y)) p->ipv6 = !p->ipv6;
    else if (rect_has(apply_rect, x, y)) { apply(w); return; }
    else if (rect_has(renew_rect, x, y)) {
        if (dosxc_sys_net(DOSXC_NET_RENEW, 0, 0) < 0) message_box("Network", "Renew asks the DHCP server again;", "this adapter has a fixed address.");
    } else if (rect_has(refresh_rect, x, y)) load(p);
    else
        for (int i = 0; i < FIELDS; ++i)
            if (field_enabled(p, i) && rect_has(field_rect(i), x, y)) p->focus = i;
    window_paint(w);
}

static void np_key(struct window *w, int key) {
    struct net_panel *p = w->data;
    if (key == '\t') {
        for (int n = 0; n < FIELDS; ++n) {
            p->focus = (p->focus + 1) % FIELDS;
            if (field_enabled(p, p->focus)) break;
        }
    } else if (key == '\n') apply(w);
    else if (field_enabled(p, p->focus) && ((key >= '0' && key <= '9') || key == '.' || key == '\b' || key >= 0x100)) field_key(&p->fields[p->focus], key);
    window_paint(w);
}

static void np_closed(struct window *w) { free(w->data); net_window = 0; }
static const struct app_ops np_ops = {np_paint, np_mouse, np_key, np_closed};

void open_network_settings(void) {
    if (net_window) { window_raise(net_window); return; }
    struct net_panel *p = calloc(1, sizeof(*p));
    if (!p) return;
    load(p);
    net_window = window_open(560, 370, "Network", ICON_NET, &np_ops, p);
    if (!net_window) free(p);
}
