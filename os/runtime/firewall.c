/*
 * DOS-XC /os/runtime/firewall.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "runtime.h"
#include "net.h"

#define MAX_RULES 64

/* One line of \SYSTEM\FIREWALL.CFG: allow|block in|out any|tcp|udp *|PORT|LOW-HIGH *|PROGRAM.EXE */
struct rule { u8 allow, inbound, protocol; u16 low, high; char program[32]; };
enum { ANY, TCP, UDP };

static struct rule rules[MAX_RULES];
static int rule_count, enabled = 1, loaded;
static char text[8192];

static const char *const default_programs[] = {"DOSFOX.EXE", "CURL.EXE", "PYTHON.EXE", "PYTHON3.EXE", "PING.EXE"};

static void set_defaults(void) {
    enabled = 1;
    rule_count = 0;
    for (uptr i = 0; i < sizeof(default_programs) / sizeof(*default_programs); ++i) {
        struct rule *r = &rules[rule_count++];
        memset(r, 0, sizeof(*r));
        r->allow = 1;
        r->high = 65535;
        rt_strcpy(r->program, default_programs[i]);
    }
}

static char *word(char **p, char *out, uptr size) {
    while (**p == ' ' || **p == '\t') ++*p;
    uptr n = 0;
    while (**p && **p != ' ' && **p != '\t' && **p != '\r' && **p != '\n') {
        if (n + 1 < size) out[n++] = (char)rt_upper(**p);
        ++*p;
    }
    out[n] = 0;
    return out;
}

static u32 number(const char **p) {
    u32 v = 0;
    while (**p >= '0' && **p <= '9') v = v * 10 + (u32)(*(*p)++ - '0');
    return v;
}

static int parse_rule(char *line, struct rule *r) {
    char action[8], direction[8], protocol[8], ports[16];
    memset(r, 0, sizeof(*r));
    word(&line, action, sizeof(action));
    word(&line, direction, sizeof(direction));
    word(&line, protocol, sizeof(protocol));
    word(&line, ports, sizeof(ports));
    word(&line, r->program, sizeof(r->program));
    if (!rt_strcmp(action, "ALLOW")) r->allow = 1;
    else if (rt_strcmp(action, "BLOCK")) return -1;
    if (!rt_strcmp(direction, "IN")) r->inbound = 1;
    else if (rt_strcmp(direction, "OUT")) return -1;
    r->protocol = !rt_strcmp(protocol, "TCP") ? TCP : !rt_strcmp(protocol, "UDP") ? UDP : ANY;
    if (!ports[0] || !rt_strcmp(ports, "*")) { r->low = 0; r->high = 65535; }
    else {
        const char *p = ports;
        r->low = r->high = (u16)number(&p);
        if (*p == '-') { ++p; r->high = (u16)number(&p); }
        if (r->high < r->low) return -1;
    }
    if (!r->program[0]) rt_strcpy(r->program, "*");
    return 0;
}

void firewall_reload(void) {
    loaded = 1;
    char path[32] = "C:\\SYSTEM\\FIREWALL.CFG";
    path[0] = (char)rt_upper(ctx->drive);
    struct fat32_info info;
    if (fs_stat(path, &info) || info.size >= sizeof(text) || fs_read(path, text, info.size)) { set_defaults(); return; }
    text[info.size] = 0;
    enabled = 1;
    rule_count = 0;
    for (char *line = text; *line;) {
        char *end = line;
        while (*end && *end != '\n') ++end;
        char saved = *end;
        *end = 0;
        while (*line == ' ' || *line == '\t') ++line;
        if (!rt_strcmp(line, "enabled=0") || !rt_strcmp(line, "enabled=0\r")) enabled = 0;
        else if (*line && *line != '#' && *line != '\r' && rt_strcmp(line, "enabled=1") && rt_strcmp(line, "enabled=1\r") && rule_count < MAX_RULES &&
                 !parse_rule(line, &rules[rule_count]))
            ++rule_count;
        *end = saved;
        line = saved ? end + 1 : end;
    }
}

static int loopback(const net_addr *a) {
    if (net_addr_is_v4(a)) return (net_addr_get_v4(a) >> 24) == 127;
    for (int i = 0; i < 15; ++i)
        if (a->b[i]) return 0;
    return a->b[15] == 1;
}

/* 0 when the running program may send to (or, INBOUND, receive on) PORT. */
int firewall_check(int inbound, int stream, u16 port, const net_addr *peer) {
    if (!loaded) firewall_reload();
    if (!enabled || !current || (peer && loopback(peer))) return 0;
    const char *program = event_program(current);
    for (int i = 0; i < rule_count; ++i) {
        const struct rule *r = &rules[i];
        if (r->inbound != inbound || port < r->low || port > r->high) continue;
        if (r->protocol != ANY && (r->protocol == TCP) != (stream != 0)) continue;
        if (rt_strcmp(r->program, "*") && !rt_equal_nocase(r->program, program)) continue;
        if (r->allow) return 0;
        break;
    }
    event_log(DOSXC_EVENT_WARNING, "Firewall", "Blocked %s %s %s on port %u", program, inbound ? "receiving" : "sending",
              stream ? "TCP" : "UDP", (unsigned)port);
    return -DOSXC_EACCES;
}

int firewall_enabled(void) {
    if (!loaded) firewall_reload();
    return enabled;
}
