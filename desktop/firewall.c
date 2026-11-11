/*
 * DOS-XC /desktop/firewall.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "wm.h"
#include <ctype.h>
#include <strings.h>

#define MAX_RULES 64
#define ROW 20
#define TOP (MENUBAR_H + 64)

struct rule { int allow, inbound, protocol; char ports[16], program[32]; };
static const char *const protocols[3] = {"any", "tcp", "udp"};
static const char *const default_programs[] = {"DOSFOX.EXE", "CURL.EXE", "PYTHON.EXE", "PYTHON3.EXE", "PING.EXE"};

static struct rule rules[MAX_RULES];
static int rule_count, enabled = 1, selected = -1, dirty;
static struct vscroll scroll;
static struct window *main_w;
static char config_path[32];

static const struct menubar bar = {
    {"File", "Rule", "Help"},
    {"Save\nRestore Defaults\n-\nExit\n", "Add\nRemove\n-\nMove Up\nMove Down\n", "About Firewall\n"},
};

static void defaults(void) {
    enabled = 1;
    rule_count = 0;
    for (size_t i = 0; i < sizeof(default_programs) / sizeof(*default_programs); ++i) {
        rules[rule_count] = (struct rule){1, 0, 0, "*", ""};
        snprintf(rules[rule_count++].program, sizeof(rules[0].program), "%s", default_programs[i]);
    }
}

static void load(void) {
    FILE *f = fopen(config_path, "r");
    if (!f) { defaults(); return; }
    rule_count = 0;
    enabled = 1;
    char line[160];
    while (fgets(line, sizeof(line), f) && rule_count < MAX_RULES) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strcmp(line, "enabled=0")) { enabled = 0; continue; }
        if (!line[0] || line[0] == '#' || !strncmp(line, "enabled=", 8)) continue;
        char action[8], direction[8], protocol[8];
        struct rule *r = &rules[rule_count];
        memset(r, 0, sizeof(*r));
        if (sscanf(line, "%7s %7s %7s %15s %31s", action, direction, protocol, r->ports, r->program) < 3) continue;
        r->allow = !strcasecmp(action, "allow");
        r->inbound = !strcasecmp(direction, "in");
        r->protocol = !strcasecmp(protocol, "tcp") ? 1 : !strcasecmp(protocol, "udp") ? 2 : 0;
        if (!r->ports[0]) strcpy(r->ports, "*");
        if (!r->program[0]) strcpy(r->program, "*");
        ++rule_count;
    }
    fclose(f);
}

static void save(void) {
    FILE *f = fopen(config_path, "w");
    if (!f) { message_box("Firewall", "The rules could not be saved.", "Only administrators can change the firewall."); return; }
    fprintf(f, "# DOS-XC firewall: the first matching rule decides; anything unmatched is blocked.\r\n");
    fprintf(f, "# allow|block in|out any|tcp|udp *|PORT|LOW-HIGH *|PROGRAM.EXE\r\nenabled=%d\r\n", enabled);
    for (int i = 0; i < rule_count; ++i) {
        const struct rule *r = &rules[i];
        fprintf(f, "%s %s %s %s %s\r\n", r->allow ? "allow" : "block", r->inbound ? "in" : "out", protocols[r->protocol], r->ports, r->program);
    }
    fclose(f);
    long on = dosxc_sys_net(DOSXC_NET_FIREWALL, 0, 0);
    if (on < 0) message_box("Firewall", "The rules were saved but not applied.", "Only administrators can change the firewall.");
    dirty = 0;
}

static struct rect list_rect(const struct window *w) { return (struct rect){8, TOP, w->content.w - 16, w->content.h - TOP - 44}; }
static struct rect button_rect(const struct window *w, int i) { return (struct rect){8 + i * 92, w->content.h - 34, 86, 24}; }
static const char *const buttons[] = {"Add", "Remove", "Move Up", "Move Down", "Save"};
static const int columns[5] = {0, 70, 130, 190, 290};

static void paint(struct window *w, const struct canvas *c) {
    fill(c, 0, 0, w->content.w, w->content.h, C_WINDOW);
    menubar_paint(c, &bar, w->content.w);
    draw_icon_big(c, 12, MENUBAR_H + 10, &icons[ICON_NET]);
    text(c, 54, MENUBAR_H + 10, &font_bold, enabled ? "The firewall is on" : "The firewall is off", enabled ? 0x007000 : 0xa00000);
    text(c, 54, MENUBAR_H + 28, &font_regular, "Programs not allowed below cannot use the network.", C_TEXT);
    button(c, (struct rect){w->content.w - 108, MENUBAR_H + 12, 98, 24}, enabled ? "Turn Off" : "Turn On", 0, 0);
    struct rect r = list_rect(w);
    bevel(c, r.x, r.y, r.w, r.h, 1);
    fill(c, r.x + 2, r.y + 2, r.w - 4, r.h - 4, C_BASE);
    static const char *const headers[] = {"Action", "Direction", "Protocol", "Ports", "Program"};
    for (int i = 0; i < 5; ++i) {
        int x = r.x + 2 + columns[i], cw = (i < 4 ? columns[i + 1] : r.w - 18) - columns[i];
        bevel(c, x, r.y + 2, cw, ROW, 0);
        text(c, x + 5, r.y + 7, &font_regular, headers[i], C_TEXT);
    }
    scroll.total = rule_count;
    scroll.visible = (r.h - ROW - 4) / ROW;
    vscroll_clamp(&scroll);
    for (int i = 0; i < scroll.visible && scroll.top + i < rule_count; ++i) {
        const struct rule *rule = &rules[scroll.top + i];
        int y = r.y + 2 + ROW + i * ROW, on = scroll.top + i == selected;
        u32 color = on ? 0xffffff : C_TEXT;
        if (on) fill(c, r.x + 2, y, r.w - 20, ROW, C_SELECTION);
        fill(c, r.x + 8, y + 6, 8, 8, rule->allow ? 0x00a000 : 0xc00000);
        text(c, r.x + 20, y + 5, &font_regular, rule->allow ? "Allow" : "Block", color);
        text(c, r.x + 2 + columns[1] + 5, y + 5, &font_regular, rule->inbound ? "In" : "Out", color);
        text(c, r.x + 2 + columns[2] + 5, y + 5, &font_regular, rule->protocol ? (rule->protocol == 1 ? "TCP" : "UDP") : "Any", color);
        text(c, r.x + 2 + columns[3] + 5, y + 5, &font_regular, strcmp(rule->ports, "*") ? rule->ports : "All", color);
        text(c, r.x + 2 + columns[4] + 5, y + 5, &font_regular, strcmp(rule->program, "*") ? rule->program : "All programs", color);
    }
    vscroll_paint(c, &scroll, (struct rect){r.x + r.w - 16, r.y + 2, 14, r.h - 4});
    for (int i = 0; i < 5; ++i) button(c, button_rect(w, i), buttons[i], 0, i == 4 && dirty);
}

static int valid_ports(const char *s) {
    if (!strcmp(s, "*")) return 1;
    char *end;
    long low = strtol(s, &end, 10), high = low;
    if (end == s) return 0;
    if (*end == '-') high = strtol(end + 1, &end, 10);
    return !*end && low >= 0 && high <= 65535 && low <= high;
}

static void ports_entered(const char *value, void *data) {
    int i = (int)(long)data;
    if (!value || i >= rule_count) return;
    if (!valid_ports(value)) { message_box("Firewall", "Ports are *, one number, or a range such as 8000-8080.", ""); return; }
    snprintf(rules[i].ports, sizeof(rules[i].ports), "%s", value);
    dirty = 1;
    window_paint(main_w);
}

static void program_entered(const char *value, void *data) {
    int i = (int)(long)data;
    if (!value || i >= rule_count) return;
    char name[32];
    size_t n = 0;
    for (const char *p = value; *p && n + 1 < sizeof(name); ++p)
        if (!isspace((unsigned char)*p)) name[n++] = (char)toupper((unsigned char)*p);
    name[n] = 0;
    if (!n) strcpy(name, "*");
    const char *slash = strrchr(name, '\\');
    snprintf(rules[i].program, sizeof(rules[i].program), "%s", slash ? slash + 1 : name);
    dirty = 1;
    window_paint(main_w);
}

static void act(struct window *w, int which) {
    if (which == 0 && rule_count < MAX_RULES) {
        rules[rule_count] = (struct rule){1, 1, 1, "8080", "*"};
        selected = rule_count++;
        dirty = 1;
        input_box("Add Rule", "Ports to allow in (*, 80 or 8000-8080):", "8080", ports_entered, (void *)(long)selected);
    } else if (which == 1 && selected >= 0 && selected < rule_count) {
        memmove(&rules[selected], &rules[selected + 1], sizeof(*rules) * (size_t)(rule_count - selected - 1));
        if (--rule_count <= selected) selected = rule_count - 1;
        dirty = 1;
    } else if ((which == 2 || which == 3) && selected >= 0) {
        int other = selected + (which == 2 ? -1 : 1);
        if (other >= 0 && other < rule_count) {
            struct rule t = rules[selected];
            rules[selected] = rules[other];
            rules[other] = t;
            selected = other;
            dirty = 1;
        }
    } else if (which == 4) save();
    window_paint(w);
}

static void popup(struct window *w, int tag, int index) {
    if (tag == 0) {
        if (index == 0) save();
        else if (index == 1) { defaults(); dirty = 1; }
        else if (index == 3) window_close(w);
    } else if (tag == 1) {
        if (index == 0) act(w, 0);
        else if (index == 1) act(w, 1);
        else if (index == 3) act(w, 2);
        else if (index == 4) act(w, 3);
    } else if (tag == 2 && index == 0)
        message_box("About Firewall", "Rules are checked from the top; the first that matches decides.",
                    "Unmatched traffic is blocked; blocked attempts are in Event Logs.");
    window_paint(w);
}

static void mouse(struct window *w, int x, int y, int kind, int value) {
    if (menubar_mouse(w, &bar, x, y, kind)) return;
    struct rect r = list_rect(w);
    if (vscroll_mouse(&scroll, (struct rect){r.x + r.w - 16, r.y + 2, 14, r.h - 4}, x, y, kind, value)) { window_paint(w); return; }
    if (kind == M_WHEEL) { scroll.top -= value * 2; vscroll_clamp(&scroll); window_paint(w); return; }
    if (kind != M_DOWN && kind != M_DOUBLE) return;
    if (rect_has((struct rect){w->content.w - 108, MENUBAR_H + 12, 98, 24}, x, y) && kind == M_DOWN) { enabled = !enabled; dirty = 1; }
    for (int i = 0; i < 5; ++i)
        if (rect_has(button_rect(w, i), x, y) && kind == M_DOWN) { act(w, i); return; }
    if (rect_has(r, x, y) && y >= r.y + 2 + ROW && x < r.x + r.w - 18) {
        int at = scroll.top + (y - r.y - 2 - ROW) / ROW;
        if (at >= rule_count) { selected = -1; window_paint(w); return; }
        int column = 0;
        for (int i = 0; i < 5; ++i)
            if (x - r.x - 2 >= columns[i]) column = i;
        if (at == selected && kind == M_DOWN) {
            struct rule *rule = &rules[at];
            if (column == 0) rule->allow = !rule->allow, dirty = 1;
            else if (column == 1) rule->inbound = !rule->inbound, dirty = 1;
            else if (column == 2) rule->protocol = (rule->protocol + 1) % 3, dirty = 1;
        }
        if (kind == M_DOUBLE && column == 3) input_box("Ports", "Ports (*, 80 or 8000-8080):", rules[at].ports, ports_entered, (void *)(long)at);
        if (kind == M_DOUBLE && column == 4) input_box("Program", "Program file name (* for all programs):", rules[at].program, program_entered, (void *)(long)at);
        selected = at;
    }
    window_paint(w);
}

static void key(struct window *w, int k) {
    if (k == 0x101 && selected > 0) --selected;
    else if (k == 0x102 && selected + 1 < rule_count) ++selected;
    else if (k == 0x108) { act(w, 1); return; }
    else if (k == 19) save();
    else return;
    window_paint(w);
}

static void ask_save(int answer, void *data) {
    (void)data;
    if (answer == 1) save();
    if (answer >= 0) { dirty = 0; window_close(main_w); }
}

static int closing(struct window *w) {
    (void)w;
    if (!dirty) return 1;
    ask_box("Firewall", "Save the changed rules?", "Save", "Don't Save", ask_save, 0);
    return 0;
}

static const struct app_ops ops = {paint, mouse, key, 0, popup, 0, 0, closing};

int main(void) {
    if (gui_connect()) { puts("Firewall needs the DOS-XC desktop."); return 1; }
    struct dosxc_sysinfo info;
    dosxc_sys_sysinfo(&info);
    snprintf(config_path, sizeof(config_path), "%c:\\SYSTEM\\FIREWALL.CFG", info.drive);
    load();
    main_w = window_open(560, 400, "Firewall", ICON_NET, &ops, 0);
    if (!main_w) return 1;
    main_w->resizable = 1;
    gui_run();
    return 0;
}
