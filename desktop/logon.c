/*
 * DOS-XC /desktop/logon.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "wm.h"

#define MAX_USERS 32
#define TILE_W 128
#define TILE_H 132
#define PICTURE 80
#define BUTTON_W 104

struct tile { struct dosxc_user user; struct surface picture; };
static struct tile tiles[MAX_USERS];
static int tile_count, focus;          /* focus: a tile, then Restart, then Shut Down */
static struct rect area;
static struct window *prompt;

int user_picture(const char *path, int size, struct surface *out) {
    struct surface source = {0};
    if (!path[0] || image_load(path, &source)) return -1;
    if (surface_size(out, size, size)) { free(source.px); return -1; }
    image_cover(&source, out);
    free(source.px);
    return 0;
}

static struct rect tile_rect(int i) {
    int total = tile_count * TILE_W;
    return (struct rect){area.x + (area.w - total) / 2 + i * TILE_W, area.y + area.h / 2 - TILE_H / 2, TILE_W, TILE_H};
}

static struct rect button_rect(int which) {
    return (struct rect){area.x + area.w - (2 - which) * (BUTTON_W + 12), area.y + area.h - 44, BUTTON_W, 26};
}

void logon_init(void) {
    area = screens[main_screen].r;
    tile_count = 0;
    long count = dosxc_sys_user(DOSXC_USER_LIST, 0, &tiles[0].user);
    for (long i = 0; i < count && tile_count < MAX_USERS; ++i) {
        struct tile *t = &tiles[tile_count];
        memset(t, 0, sizeof(*t));
        dosxc_sys_user(DOSXC_USER_LIST, (void *)i, &t->user);
        user_picture(t->user.picture, PICTURE, &t->picture);
        ++tile_count;
    }
    focus = 0;
    desktop_damage(area);
}

void logon_paint(const struct canvas *c, struct rect screen) {
    for (int y = 0; y < screen.h; y += 4) {
        unsigned t = (unsigned)(y * 255 / (screen.h ? screen.h : 1));
        u32 color = (u32)(0x10 + t / 12) << 16 | (u32)(0x70 - t / 6) << 8 | (u32)(0x80 - t / 8);
        fill(c, screen.x, screen.y + y, screen.w, 4, color);
    }
    if (screen.x != area.x || screen.y != area.y) return;
    const char *title = "Welcome to DOS-XC";
    text_big(c, area.x + (area.w - text_width(&font_bold, title) * 2) / 2, area.y + area.h / 2 - TILE_H / 2 - 70, title, 0xffffff, 2);
    for (int i = 0; i < tile_count; ++i) {
        struct rect r = tile_rect(i);
        const struct tile *t = &tiles[i];
        int px = r.x + (r.w - PICTURE) / 2, py = r.y + 8;
        if (i == focus) {
            fill(c, r.x + 6, r.y, r.w - 12, r.h, 0x2f8fa8);
            frame_box(c, r.x + 6, r.y, r.w - 12, r.h, 0xffffff);
        }
        fill(c, px - 3, py - 3, PICTURE + 6, PICTURE + 6, 0xffffff);
        if (t->picture.px) blit(c, px, py, &t->picture);
        else {
            fill(c, px, py, PICTURE, PICTURE, 0xd4d0c8);
            draw_icon_big(c, px + (PICTURE - 32) / 2, py + (PICTURE - 32) / 2, &icons[ICON_USER]);
        }
        char label[40];
        snprintf(label, sizeof(label), "%s", t->user.nickname[0] ? t->user.nickname : t->user.name);
        while (text_width(&font_regular, label) > r.w - 16 && strlen(label) > 3) {
            size_t n = strlen(label);
            label[n - 4] = '.'; label[n - 3] = '.'; label[n - 2] = '.'; label[n - 1] = 0;
        }
        text(c, r.x + (r.w - text_width(&font_regular, label)) / 2, py + PICTURE + 12, &font_regular, label, 0xffffff);
    }
    if (!tile_count) text(c, area.x + 40, area.y + area.h / 2, &font_bold, "There are no user accounts.", 0xffffff);
    static const char *const names[2] = {"Restart", "Shut Down"};
    for (int b = 0; b < 2; ++b) button(c, button_rect(b), names[b], 0, focus == tile_count + b);
}

/* ---- the password prompt, like a "Logon Information" box ---------------- */

struct password { int index; struct field field; char error[64]; };
static const struct rect pw_field = {104, 74, 200, 22};
static const struct rect pw_ok = {64, 112, 76, 24}, pw_cancel = {148, 112, 76, 24}, pw_shut = {232, 112, 92, 24};

static void pw_paint(struct window *w, const struct canvas *c) {
    struct password *p = w->data;
    const struct tile *t = &tiles[p->index];
    if (t->picture.px) {
        struct surface small = {0};
        if (!surface_size(&small, 48, 48)) { image_scale(&t->picture, &small); blit(c, 14, 14, &small); free(small.px); }
    } else draw_icon_big(c, 22, 22, &icons[ICON_USER]);
    char line[96];
    snprintf(line, sizeof(line), "Enter the password for %s.", t->user.nickname[0] ? t->user.nickname : t->user.name);
    text(c, 76, 16, &font_regular, line, C_TEXT);
    text(c, 76, 44, &font_regular, "User name:", C_TEXT);
    text(c, 152, 44, &font_regular, t->user.name, C_TEXT);
    text(c, 16, 79, &font_regular, "Password:", C_TEXT);
    struct field masked = p->field;
    for (size_t i = 0; masked.text[i]; ++i) masked.text[i] = '*';
    field_paint(c, &masked, pw_field, 1);
    if (p->error[0]) text(c, 104, 98, &font_regular, p->error, 0xb00000);
    button(c, pw_ok, "OK", 0, 1);
    button(c, pw_cancel, "Cancel", 0, 0);
    button(c, pw_shut, "Shut Down...", 0, 0);
}

static void shutdown_chosen(int ok, void *data) {
    (void)data;
    if (ok) { leave_desktop(); dosxc_sys_power(DOSXC_POWER_OFF); }
}

static void pw_try(struct window *w) {
    struct password *p = w->data;
    if (!dosxc_sys_user(DOSXC_USER_LOGIN, tiles[p->index].user.name, p->field.text)) {
        memset(p->field.text, 0, sizeof(p->field.text));
        window_close(w);
        desktop_quit(0);
        return;
    }
    memset(p->field.text, 0, sizeof(p->field.text));
    p->field.cursor = 0;
    snprintf(p->error, sizeof(p->error), "That password is not right.");
    window_paint(w);
}

static void pw_mouse(struct window *w, int x, int y, int kind, int value) {
    (void)value;
    if (kind != M_DOWN) return;
    if (rect_has(pw_ok, x, y)) pw_try(w);
    else if (rect_has(pw_cancel, x, y)) window_close(w);
    else if (rect_has(pw_shut, x, y)) confirm_box("Shut Down", "Shut down the computer now?", "Shut Down", ICON_POWER, shutdown_chosen, 0);
}

static void pw_key(struct window *w, int key) {
    struct password *p = w->data;
    if (key == '\n') pw_try(w);
    else if (key == 27) window_close(w);
    else if (field_key(&p->field, key)) window_paint(w);
}

static void pw_closed(struct window *w) {
    struct password *p = w->data;
    memset(p, 0, sizeof(*p));
    free(p);
    prompt = 0;
}
static const struct app_ops pw_ops = {pw_paint, pw_mouse, pw_key, pw_closed};

static void ask_password(int index) {
    if (prompt || index < 0 || index >= tile_count) return;
    struct password *p = calloc(1, sizeof(*p));
    if (!p) return;
    p->index = index;
    prompt = window_open(340, 148, "Logon Information", ICON_USER, &pw_ops, p);
    if (!prompt) free(p);
    else window_modal(prompt);
}

static void restart_chosen(int ok, void *data) {
    (void)data;
    if (ok) { leave_desktop(); dosxc_sys_reboot(); }
}

static void activate_focus(void) {
    if (focus < tile_count) ask_password(focus);
    else if (focus == tile_count) confirm_box("Restart", "Restart the computer now?", "Restart", ICON_RESTART, restart_chosen, 0);
    else confirm_box("Shut Down", "Shut down the computer now?", "Shut Down", ICON_POWER, shutdown_chosen, 0);
}

int logon_click(int x, int y) {
    for (int i = 0; i < tile_count; ++i)
        if (rect_has(tile_rect(i), x, y)) { focus = i; desktop_damage(area); ask_password(i); return 1; }
    for (int b = 0; b < 2; ++b)
        if (rect_has(button_rect(b), x, y)) { focus = tile_count + b; desktop_damage(area); activate_focus(); return 1; }
    return 0;
}

void logon_key(int key) {
    int stops = tile_count + 2;
    if (key == '\t' || key == 0x103 || key == 0x102) focus = (focus + 1) % stops;
    else if (key == 0x104 || key == 0x101) focus = (focus + stops - 1) % stops;
    else if (key == '\n' || key == ' ') { activate_focus(); return; }
    else return;
    desktop_damage(area);
}
