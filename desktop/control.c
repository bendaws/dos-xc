/*
 * DOS-XC /desktop/control.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "wm.h"

#define SIDEBAR_W 184
#define ROW_H 24
#define HEADER_H 52
#define PANE_W 560
#define PANE_H 400
#define CELL_W 110
#define CELL_H 78

static void start_app(const char *name, const char *what) {
    char app[260];
    snprintf(app, sizeof(app), "Apps\\%s", name);
    char path[260];
    desktop_path(path, sizeof(path), app);
    char *argv[2] = {path, 0};
    if (dosxc_sys_task(DOSXC_TASK_START, path, (long)argv, 0) < 0) message_box(what, "The program could not be started.", "");
}
static void open_packages(void) {
    if (program_start("pkgmgr", 0)) message_box("Package Manager", "The Package Manager could not be started.", "Install the pkg package again from \\Packages.");
}
static void open_firewall(void) { start_app("FIREWALL.EXE", "Firewall"); }
static void open_events(void) { start_app("EVENTS.EXE", "Event Logs"); }
static void open_devices(void) { start_app("DEVICES.EXE", "Device Manager"); }

/* A page shows a panel inside the window; a tool starts a program instead. */
struct page { const char *label, *about; int icon; void (*open)(void); int tool; };
static const struct page pages[] = {
    {"Home", "All settings", ICON_GEAR, 0, 0},
    {"Display", "Screens, resolution and scaling", ICON_DISPLAY, open_display_settings, 0},
    {"Wallpaper", "The picture behind your windows", ICON_PICTURE, open_wallpaper, 0},
    {"Accent Color", "The color of title bars and menus", ICON_PAINT, open_accent, 0},
    {"Sound", "Volume", ICON_SOUND, open_sound, 0},
    {"Network", "Addresses, DNS servers, IPv4 and IPv6", ICON_NET, open_network_settings, 0},
    {"Date and Time", "Clock, time server and time zone", ICON_GEAR, open_datetime_settings, 0},
    {"Default Programs", "Programs that open web pages and files", ICON_STAR, open_default_programs, 0},
    {"User Account Center", "Your account; administrators manage everyone's", ICON_USER, open_account_center, 0},
    {"Packages", "Install and remove packages", ICON_PACKAGE, open_packages, 1},
    {"Firewall", "Which programs may use the network", ICON_NET, open_firewall, 1},
    {"Event Logs", "What the system has recorded", ICON_INFO, open_events, 1},
    {"Device Manager", "Hardware, and drivers for it", ICON_COMPUTER, open_devices, 1},
};
#define PAGES ((int)(sizeof(pages) / sizeof(*pages)))
#define FIRST_TOOL 9

struct cp { int page, hot, failed; };
static struct window *panel_window;

static struct rect sidebar_row(int i) {
    int y = 8 + i * ROW_H + (i >= FIRST_TOOL ? 26 : 0);
    return (struct rect){4, y, SIDEBAR_W - 8, ROW_H};
}
static struct rect pane_rect(const struct window *w) { return (struct rect){SIDEBAR_W, HEADER_H, w->r.w - SIDEBAR_W, w->r.h - HEADER_H}; }
static struct rect home_cell(int i) {
    int columns = (PANE_W - 16) / CELL_W, at = i - 1;
    return (struct rect){SIDEBAR_W + 8 + at % columns * CELL_W, HEADER_H + 8 + at / columns * CELL_H, CELL_W, CELL_H};
}

static void show_page(struct window *w, int page) {
    struct cp *cp = w->data;
    if (pages[page].tool) { pages[page].open(); return; }
    if (w->hosted) window_close(w->hosted);
    cp->page = page;
    cp->failed = 0;
    if (pages[page].open) {
        window_host_next(w);
        pages[page].open();
        window_host_next(0);
        cp->failed = !w->hosted;
    }
    window_paint(w);
}

static void cp_paint(struct window *w, const struct canvas *c) {
    struct cp *cp = w->data;
    if (cp->page && !w->hosted && !cp->failed) show_page(w, cp->page);
    fill(c, 0, 0, SIDEBAR_W, w->r.h, C_MENU_STRIPE);
    fill(c, SIDEBAR_W - 1, 0, 1, w->r.h, C_SHADOW1);
    for (int i = 0; i < PAGES; ++i) {
        struct rect r = sidebar_row(i);
        if (i == FIRST_TOOL) {
            text(c, r.x + 6, r.y - 18, &font_bold, "Administrative Tools", C_SHADOW1);
            fill(c, r.x + 4, r.y - 4, r.w - 8, 1, C_SHADOW1);
        }
        int on = i == cp->page, hot = i == cp->hot;
        if (on) fill(c, r.x, r.y, r.w, r.h, C_SELECTION);
        else if (hot) fill(c, r.x, r.y, r.w, r.h, C_HOVER);
        draw_icon(c, r.x + 4, r.y + 4, pages[i].icon);
        text(c, r.x + 26, r.y + 7, on ? &font_bold : &font_regular, pages[i].label, on ? 0xffffff : C_TEXT);
    }
    fill(c, SIDEBAR_W, 0, w->r.w - SIDEBAR_W, w->r.h, C_WINDOW);
    draw_icon_big(c, SIDEBAR_W + 12, 10, &icons[pages[cp->page].icon]);
    text(c, SIDEBAR_W + 54, 12, &font_bold, pages[cp->page].label, C_TEXT);
    text(c, SIDEBAR_W + 54, 30, &font_regular, pages[cp->page].about, C_SHADOW1);
    fill(c, SIDEBAR_W + 8, HEADER_H - 4, w->r.w - SIDEBAR_W - 16, 1, C_SHADOW1);
    if (cp->page == 0) {
        for (int i = 1; i < PAGES; ++i) {
            struct rect r = home_cell(i);
            if (i == cp->hot) fill(c, r.x + 2, r.y + 2, r.w - 4, r.h - 4, C_HOVER);
            draw_icon_big(c, r.x + (r.w - 32) / 2, r.y + 8, &icons[pages[i].icon]);
            int tw = text_width(&font_regular, pages[i].label);
            if (tw > r.w - 4) {
                char shorter[24];
                snprintf(shorter, sizeof(shorter), "%.12s...", pages[i].label);
                tw = text_width(&font_regular, shorter);
                text(c, r.x + (r.w - tw) / 2, r.y + 48, &font_regular, shorter, C_TEXT);
            } else text(c, r.x + (r.w - tw) / 2, r.y + 48, &font_regular, pages[i].label, C_TEXT);
        }
        return;
    }
    struct rect pane = pane_rect(w);
    if (!w->hosted) {
        text(c, pane.x + 16, pane.y + 16, &font_regular, "This page is open in its own window.", C_TEXT);
        return;
    }
    struct window *h = w->hosted;
    h->r = (struct rect){w->r.x + pane.x, w->r.y + pane.y, pane.w, pane.h};
    struct canvas inner = *c;
    inner.ox += pane.x;
    inner.oy += pane.y;
    inner.clip = rect_both((struct rect){0, 0, pane.w, pane.h}, (struct rect){c->clip.x - pane.x, c->clip.y - pane.y, c->clip.w, c->clip.h});
    fill(&inner, 0, 0, pane.w, pane.h, C_WINDOW);
    h->ops->paint(h, &inner);
}

static void cp_mouse(struct window *w, int x, int y, int kind, int value) {
    struct cp *cp = w->data;
    static int captured;
    struct rect pane = pane_rect(w);
    if (w->hosted && (captured || (rect_has(pane, x, y) && cp->page))) {
        if (kind == M_DOWN) captured = 1;
        if (kind == M_UP) captured = 0;
        if (w->hosted->ops->mouse) w->hosted->ops->mouse(w->hosted, x - pane.x, y - pane.y, kind, value);
        return;
    }
    int at = -1;
    for (int i = 0; i < PAGES; ++i)
        if (rect_has(sidebar_row(i), x, y) || (cp->page == 0 && i && rect_has(home_cell(i), x, y))) at = i;
    if (at != cp->hot) { cp->hot = at; window_paint(w); }
    if ((kind == M_DOWN || kind == M_DOUBLE) && at >= 0) show_page(w, at);
}

static void cp_key(struct window *w, int key) {
    if (w->hosted && w->hosted->ops->key) w->hosted->ops->key(w->hosted, key);
}

static void cp_closed(struct window *w) {
    free(w->data);
    panel_window = 0;
    dosxc_sys_user(DOSXC_USER_LOCK, 0, 0);
}
static const struct app_ops cp_ops = {cp_paint, cp_mouse, cp_key, cp_closed};

static void open_page(const char *label) {
    if (!panel_window) {
        struct cp *cp = calloc(1, sizeof(*cp));
        if (!cp) return;
        cp->hot = -1;
        panel_window = window_open(SIDEBAR_W + PANE_W, HEADER_H + PANE_H, "Control Panel", ICON_GEAR, &cp_ops, cp);
        if (!panel_window) { free(cp); return; }
        panel_window->track_mouse = 1;
    } else window_raise(panel_window);
    for (int i = 0; i < PAGES; ++i)
        if (!strcmp(pages[i].label, label)) show_page(panel_window, i);
}

void open_control_panel(void) { open_page("Home"); }
void open_network(void) { open_page("Network"); }
void open_user_accounts(void) { open_page("User Account Center"); }

/* ---- Default Programs ---------------------------------------------------- */

static const struct { const char *kind, *label; int optional; } kinds[] = {
    {"browser", "Web browser", 1}, {"editor", "Text editor", 0}, {"media", "Media player", 1},
    {"document", "Documents", 1}, {"spreadsheet", "Spreadsheets", 1}, {"presentation", "Presentations", 1},
};
#define KINDS ((int)(sizeof(kinds) / sizeof(*kinds)))
#define CHOICES 6

struct defaults { struct app_entry choices[KINDS][CHOICES]; int count[KINDS], chosen[KINDS]; };
static struct rect choice_rect(int kind) { return (struct rect){130, 14 + kind * 30, 230, 22}; }
static const struct rect dp_ok = {200, 14 + KINDS * 30 + 10, 76, 24}, dp_cancel = {284, 14 + KINDS * 30 + 10, 76, 24};

/* chosen = -1: none. */
static void dp_paint(struct window *w, const struct canvas *c) {
    struct defaults *d = w->data;
    for (int k = 0; k < KINDS; ++k) {
        struct rect r = choice_rect(k);
        text(c, 14, r.y + 5, &font_regular, kinds[k].label, C_TEXT);
        bevel(c, r.x, r.y, r.w, r.h, 1);
        fill(c, r.x + 2, r.y + 2, r.w - 4, r.h - 4, C_BASE);
        int i = d->chosen[k];
        if (i >= 0) {
            draw_icon(c, r.x + 4, r.y + 3, d->choices[k][i].icon);
            text(c, r.x + 24, r.y + 6, &font_regular, d->choices[k][i].name, C_TEXT);
        } else text(c, r.x + 24, r.y + 6, &font_regular, d->count[k] ? "None" : "(no program installed)", C_DISABLED);
        for (int t = 0; t < 4; ++t) fill(c, r.x + r.w - 14 + t, r.y + 9 + t, 7 - 2 * t, 1, C_TEXT);
    }
    button(c, dp_ok, "OK", 0, 1);
    button(c, dp_cancel, "Cancel", 0, 0);
}

static void dp_accept(struct window *w) {
    struct defaults *d = w->data;
    for (int k = 0; k < KINDS; ++k)
        set_default_program(kinds[k].kind, d->chosen[k] >= 0 ? d->choices[k][d->chosen[k]].id : "none");
    window_close(w);
}

static void dp_mouse(struct window *w, int x, int y, int kind, int value) {
    (void)value;
    struct defaults *d = w->data;
    if (kind != M_DOWN && kind != M_DOUBLE) return;
    if (rect_has(dp_ok, x, y)) { dp_accept(w); return; }
    if (rect_has(dp_cancel, x, y)) { window_close(w); return; }
    for (int k = 0; k < KINDS; ++k)
        if (rect_has(choice_rect(k), x, y) && d->count[k]) {
            int next = d->chosen[k] + 1;
            if (next >= d->count[k]) next = kinds[k].optional ? -1 : 0;
            d->chosen[k] = next;
            window_paint(w);
        }
}

static void dp_key(struct window *w, int key) {
    if (key == '\n') dp_accept(w);
    else if (key == 27) window_close(w);
}

static void dp_closed(struct window *w) { free(w->data); }
static const struct app_ops dp_ops = {dp_paint, dp_mouse, dp_key, dp_closed};

void open_default_programs(void) {
    struct defaults *d = calloc(1, sizeof(*d));
    if (!d) return;
    for (int k = 0; k < KINDS; ++k) {
        d->count[k] = programs_for(kinds[k].kind, d->choices[k], CHOICES);
        char id[32];
        default_program(kinds[k].kind, id, sizeof(id));
        d->chosen[k] = -1;
        for (int i = 0; i < d->count[k]; ++i)
            if (!strcmp(d->choices[k][i].id, id)) d->chosen[k] = i;
    }
    struct window *w = window_open(374, 14 + KINDS * 30 + 46, "Default Programs", ICON_STAR, &dp_ops, d);
    if (!w) free(d);
}

void open_date_time(void) { open_page("Date and Time"); }

void control_panel_tick(void) {
    static unsigned long long next;
    unsigned long long now = dosxc_sys_nanoseconds();
    if (now < next) return;
    next = now + 100000000ULL;
    if (panel_window && panel_window->hosted && panel_window->hosted->ops->tick)
        panel_window->hosted->ops->tick(panel_window->hosted);
}
