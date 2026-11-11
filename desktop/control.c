/*
 * DOS-XC /desktop/control.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "wm.h"

#define CELL_W 96
#define CELL_H 72
#define COLUMNS 4

static void open_packages(void) {
    char app[260];
    desktop_path(app, sizeof(app), "Apps\\PKGMGR.EXE");
    char *argv[2] = {app, 0};
    if (dosxc_sys_task(DOSXC_TASK_START, app, (long)argv, 0) < 0) message_box("Packages", "The package manager could not be started.", "");
}

static const struct { const char *label; int icon; void (*open)(void); } panels[] = {
    {"Display", ICON_DISPLAY, open_display_settings},
    {"Wallpaper", ICON_PICTURE, open_wallpaper},
    {"Accent Color", ICON_PAINT, open_accent},
    {"Sound", ICON_SOUND, open_sound},
    {"Network", ICON_NET, open_network},
    {"Default Programs", ICON_STAR, open_default_programs},
    {"User Accounts", ICON_USER, open_user_accounts},
    {"Packages", ICON_PACKAGE, open_packages},
};
#define PANELS ((int)(sizeof(panels) / sizeof(*panels)))

static struct window *panel_window;

static struct rect cell(int i) { return (struct rect){8 + i % COLUMNS * CELL_W, 8 + i / COLUMNS * CELL_H, CELL_W, CELL_H}; }

static void cp_paint(struct window *w, const struct canvas *c) {
    int *hot = w->data;
    fill(c, 0, 0, w->r.w, w->r.h, C_BASE);
    for (int i = 0; i < PANELS; ++i) {
        struct rect r = cell(i);
        if (i == *hot) fill(c, r.x + 2, r.y + 2, r.w - 4, r.h - 4, C_HOVER);
        draw_icon_big(c, r.x + (r.w - 32) / 2, r.y + 8, &icons[panels[i].icon]);
        int tw = text_width(&font_regular, panels[i].label);
        text(c, r.x + (r.w - tw) / 2, r.y + 48, &font_regular, panels[i].label, C_TEXT);
    }
}

static void cp_mouse(struct window *w, int x, int y, int kind, int value) {
    (void)value;
    int *hot = w->data, at = -1;
    for (int i = 0; i < PANELS; ++i)
        if (rect_has(cell(i), x, y)) at = i;
    if (at != *hot) { *hot = at; window_paint(w); }
    if ((kind == M_DOWN || kind == M_DOUBLE) && at >= 0) panels[at].open();
}

static void cp_closed(struct window *w) { free(w->data); panel_window = 0; }
static const struct app_ops cp_ops = {cp_paint, cp_mouse, 0, cp_closed};

void open_control_panel(void) {
    if (panel_window) { window_raise(panel_window); return; }
    int *hot = malloc(sizeof(int));
    if (!hot) return;
    *hot = -1;
    int rows = (PANELS + COLUMNS - 1) / COLUMNS;
    panel_window = window_open(16 + COLUMNS * CELL_W, 16 + rows * CELL_H, "Control Panel", ICON_GEAR, &cp_ops, hot);
    if (!panel_window) { free(hot); return; }
    panel_window->track_mouse = 1;
}

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
