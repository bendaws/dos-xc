/*
 * DOS-XC /desktop/widgets.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * Scroll bars, text fields and menu bars, shared by the desktop and its
 * client programs.
 */
#include "wm.h"

#define MIN_THUMB 14

void desktop_path(char *out, size_t size, const char *relative) {
    struct dosxc_sysinfo info;
    dosxc_sys_sysinfo(&info);
    snprintf(out, size, "%c:\\Programs\\DOS-XC Desktop\\%s", info.drive, relative);
}

void vscroll_clamp(struct vscroll *s) {
    int most = s->total - s->visible;
    if (s->top > most) s->top = most;
    if (s->top < 0) s->top = 0;
}

static struct rect thumb_of(const struct vscroll *s, struct rect r) {
    if (s->total <= s->visible || r.h <= 0) return (struct rect){r.x, r.y, r.w, 0};
    int h = r.h * s->visible / s->total;
    if (h < MIN_THUMB) h = MIN_THUMB;
    int y = r.y + (r.h - h) * s->top / (s->total - s->visible);
    return (struct rect){r.x, y, r.w, h};
}

void vscroll_paint(const struct canvas *c, const struct vscroll *s, struct rect r) {
    fill(c, r.x, r.y, r.w, r.h, C_MENU_STRIPE);
    struct rect t = thumb_of(s, r);
    if (!t.h) return;
    fill(c, t.x, t.y, t.w, t.h, C_WINDOW);
    bevel(c, t.x, t.y, t.w, t.h, 0);
}

int vscroll_mouse(struct vscroll *s, struct rect r, int x, int y, int kind, int value) {
    (void)value;
    struct rect t = thumb_of(s, r);
    if (kind == M_MOVE && s->dragging) {
        if (r.h > t.h && s->total > s->visible) s->top = s->grab_top + (y - s->grab_y) * (s->total - s->visible) / (r.h - t.h);
        vscroll_clamp(s);
        return 1;
    }
    if (kind == M_UP && s->dragging) { s->dragging = 0; return 1; }
    if ((kind != M_DOWN && kind != M_DOUBLE) || !rect_has(r, x, y)) return 0;
    if (rect_has(t, x, y)) { s->dragging = 1; s->grab_y = y; s->grab_top = s->top; return 1; }
    s->top += y < t.y ? -s->visible : s->visible;
    vscroll_clamp(s);
    return 1;
}

void field_set(struct field *f, const char *text) {
    snprintf(f->text, sizeof(f->text), "%s", text);
    f->cursor = (int)strlen(f->text);
}

void field_paint(const struct canvas *c, const struct field *f, struct rect r, int focused) {
    bevel(c, r.x, r.y, r.w, r.h, 1);
    fill(c, r.x + 2, r.y + 2, r.w - 4, r.h - 4, C_BASE);
    struct canvas inner = *c;
    inner.clip = rect_both(c->clip, (struct rect){r.x + 2, r.y + 2, r.w - 4, r.h - 4});
    int ty = r.y + (r.h - font_regular.height) / 2;
    char head[260];
    snprintf(head, sizeof(head), "%.*s", f->cursor, f->text);
    int caret = text_width(&font_regular, head), shift = 0;
    if (caret > r.w - 12) shift = caret - (r.w - 12);
    text(&inner, r.x + 5 - shift, ty, &font_regular, f->text, C_TEXT);
    if (focused) fill(&inner, r.x + 5 - shift + caret, ty - 1, 1, font_regular.height + 2, C_TEXT);
}

/* 1 if the key changed the field. */
int field_key(struct field *f, int key) {
    int n = (int)strlen(f->text);
    switch (key) {
    case '\b':
        if (!f->cursor) return 0;
        memmove(f->text + f->cursor - 1, f->text + f->cursor, (size_t)(n - f->cursor + 1));
        --f->cursor;
        return 1;
    case 0x108:                        /* Delete */
        if (f->cursor == n) return 0;
        memmove(f->text + f->cursor, f->text + f->cursor + 1, (size_t)(n - f->cursor));
        return 1;
    case 0x103: if (f->cursor < n) ++f->cursor; return 1;
    case 0x104: if (f->cursor) --f->cursor; return 1;
    case 0x105: f->cursor = 0; return 1;
    case 0x106: f->cursor = n; return 1;
    }
    if (key < 32 || key > 255 || key == 127 || n + 1 >= (int)sizeof(f->text)) return 0;
    memmove(f->text + f->cursor + 1, f->text + f->cursor, (size_t)(n - f->cursor + 1));
    f->text[f->cursor++] = (char)key;
    return 1;
}

static int title_at(const struct menubar *m, int x, int *left) {
    int at = 4;
    for (int i = 0; i < 8 && m->titles[i]; ++i) {
        int w = text_width(&font_regular, m->titles[i]) + 12;
        if (x >= at && x < at + w) { *left = at; return i; }
        at += w;
    }
    return -1;
}

void menubar_paint(const struct canvas *c, const struct menubar *m, int width) {
    fill(c, 0, 0, width, MENUBAR_H, C_WINDOW);
    fill(c, 0, MENUBAR_H - 2, width, 1, C_SHADOW1);
    fill(c, 0, MENUBAR_H - 1, width, 1, C_HIGHLIGHT);
    int at = 4;
    for (int i = 0; i < 8 && m->titles[i]; ++i) {
        text(c, at + 6, (MENUBAR_H - font_regular.height) / 2 - 1, &font_regular, m->titles[i], C_TEXT);
        at += text_width(&font_regular, m->titles[i]) + 12;
    }
}

/* Open the menu under a click on the bar; 1 if the bar took the click. */
int menubar_mouse(struct window *w, const struct menubar *m, int x, int y, int kind) {
    if (y >= MENUBAR_H || (kind != M_DOWN && kind != M_DOUBLE)) return y < MENUBAR_H && kind != M_MOVE && kind != M_UP;
    int left = 0, i = title_at(m, x, &left);
    if (i >= 0) window_popup(w, left, MENUBAR_H, m->items[i], i);
    return 1;
}
