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

static const char *const browsers[] = {"none", "dosfox"};
static const char *const editors[] = {"editor", "vim"};
#define LIST_ROW 22

struct defaults {
    const char *browser_shown[4], *editor_shown[4];
    int browser_count, editor_count, browser, editor;
};
static const struct rect dp_ok = {148, 236, 80, 24}, dp_cancel = {236, 236, 80, 24};
static struct rect browser_row(int i) { return (struct rect){16, 34 + i * LIST_ROW, 300, LIST_ROW}; }
static struct rect editor_row(int i) { return (struct rect){16, 140 + i * LIST_ROW, 300, LIST_ROW}; }

static void choice_paint(const struct canvas *c, struct rect r, const char *name, int on) {
    char label[64] = "None";
    int icon = ICON_INFO;
    if (strcmp(name, "none")) program_info(name, label, sizeof(label), &icon);
    bevel(c, r.x, r.y + 4, 14, 14, 1);
    fill(c, r.x + 2, r.y + 6, 10, 10, C_BASE);
    if (on) fill(c, r.x + 4, r.y + 8, 6, 6, C_TEXT);
    if (strcmp(name, "none")) draw_icon(c, r.x + 22, r.y + 3, icon);
    text(c, r.x + 44, r.y + 6, &font_regular, label, C_TEXT);
}

static void dp_paint(struct window *w, const struct canvas *c) {
    struct defaults *d = w->data;
    text(c, 16, 14, &font_bold, "Web browser", C_TEXT);
    for (int i = 0; i < d->browser_count; ++i) choice_paint(c, browser_row(i), d->browser_shown[i], i == d->browser);
    text(c, 16, 120, &font_bold, "Text editor", C_TEXT);
    for (int i = 0; i < d->editor_count; ++i) choice_paint(c, editor_row(i), d->editor_shown[i], i == d->editor);
    button(c, dp_ok, "OK", 0, 1);
    button(c, dp_cancel, "Cancel", 0, 0);
}

static void dp_accept(struct window *w) {
    struct defaults *d = w->data;
    set_default_program("browser", d->browser_shown[d->browser]);
    set_default_program("editor", d->editor_shown[d->editor]);
    window_close(w);
}

static void dp_mouse(struct window *w, int x, int y, int kind, int value) {
    (void)value;
    struct defaults *d = w->data;
    if (kind != M_DOWN && kind != M_DOUBLE) return;
    if (rect_has(dp_ok, x, y)) { dp_accept(w); return; }
    if (rect_has(dp_cancel, x, y)) { window_close(w); return; }
    for (int i = 0; i < d->browser_count; ++i)
        if (rect_has(browser_row(i), x, y)) { d->browser = i; window_paint(w); }
    for (int i = 0; i < d->editor_count; ++i)
        if (rect_has(editor_row(i), x, y)) { d->editor = i; window_paint(w); }
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
    char browser[32], editor[32];
    default_program("browser", browser, sizeof(browser));
    default_program("editor", editor, sizeof(editor));
    for (size_t i = 0; i < sizeof(browsers) / sizeof(*browsers); ++i)
        if (!strcmp(browsers[i], "none") || program_installed(browsers[i])) {
            if (!strcmp(browsers[i], browser[0] ? browser : "none")) d->browser = d->browser_count;
            d->browser_shown[d->browser_count++] = browsers[i];
        }
    for (size_t i = 0; i < sizeof(editors) / sizeof(*editors); ++i)
        if (program_installed(editors[i])) {
            if (!strcmp(editors[i], editor)) d->editor = d->editor_count;
            d->editor_shown[d->editor_count++] = editors[i];
        }
    struct window *w = window_open(332, 270, "Default Programs", ICON_STAR, &dp_ops, d);
    if (!w) free(d);
}
