/*
 * DOS-XC /openwork/suite.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "suite.h"
#include <strings.h>

#ifndef OPENWORK_VERSION
#define OPENWORK_VERSION "1.0"
#endif

#define RECENT_MAX 10
#define NAV_W 150

const struct suite_app *app;
char suite_path[300];
static struct surface strip16, strip32, app16, app32;

enum { H_FILE, H_TAB, H_HELP, H_ITEM, H_GALLERY, H_MORE, H_BACK, H_NAV, H_NEW, H_BROWSE_OPEN, H_BROWSE_SAVE, H_RECENT, H_SETTINGS };
enum { PAGE_INFO, PAGE_NEW, PAGE_OPEN, PAGE_SAVE_AS };
enum { ACT_SAVE = -1, ACT_CLOSE = -2, ACT_ABOUT = -3 };

u32 mix(u32 a, u32 b, int percent) {
    u32 out = 0;
    for (int shift = 0; shift < 24; shift += 8) {
        int x = (int)(a >> shift & 0xff), y = (int)(b >> shift & 0xff);
        out |= (u32)(x + (y - x) * percent / 100) << shift;
    }
    return out;
}

u32 accent_text(void) {
    u32 a = app->accent;
    int light = (int)((a >> 16 & 0xff) * 299 + (a >> 8 & 0xff) * 587 + (a & 0xff) * 114) / 1000;
    return light > 150 ? mix(a, 0, 42) : a;
}

static void load_alpha(const char *folder, const char *name, struct surface *out) {
    char path[300];
    snprintf(path, sizeof(path), "%s%s", folder, name);
    if (image_load_alpha(path, out)) { memset(out, 0, sizeof(*out)); return; }
    for (int i = 0; i < out->w * out->h; ++i)
        if (!(out->px[i] >> 24)) out->px[i] = 0;
}

void suite_start(const struct suite_app *a, const char *argv0) {
    app = a;
    char folder[260] = "", name[64];
    snprintf(folder, sizeof(folder), "%s", argv0);
    char *slash = strrchr(folder, '\\');
    if (slash) slash[1] = 0;
    else folder[0] = 0;
    load_alpha(folder, "RIBBON16.PNG", &strip16);
    load_alpha(folder, "RIBBON32.PNG", &strip32);
    snprintf(name, sizeof(name), "%s.PNG", a->icon);
    load_alpha(folder, name, &app16);
    char icon[300];
    snprintf(icon, sizeof(icon), "%s%s", folder, name);
    gui_set_icon_file(icon);
    snprintf(name, sizeof(name), "%s32.PNG", a->icon);
    load_alpha(folder, name, &app32);
}

static void blend(const struct canvas *c, int x, int y, const struct surface *s, int sx, int w, int h, u32 tint) {
    if (!s->px) return;
    struct rect r = rect_both((struct rect){x, y, w, h}, c->clip);
    for (int j = 0; j < r.h; ++j) {
        const u32 *from = s->px + (r.y - y + j) * s->w + sx + (r.x - x);
        u32 *to = c->s->px + (r.y + j + c->oy) * c->s->w + r.x + c->ox;
        for (int i = 0; i < r.w; ++i) {
            u32 v = from[i], a = v >> 24;
            if (!a) continue;
            u32 color = tint ? tint : v & 0xffffff;
            to[i] = a == 255 ? color : mix(to[i] & 0xffffff, color, (int)(a * 100 / 255));
        }
    }
}

void ow_icon(const struct canvas *c, int x, int y, int icon, int size, u32 tint) {
    if (icon < 0 || icon >= IC_COUNT) return;
    blend(c, x, y, size >= 32 ? &strip32 : &strip16, icon * (size >= 32 ? 32 : 16), size >= 32 ? 32 : 16, size >= 32 ? 32 : 16, tint);
}

void ow_app_icon(const struct canvas *c, int x, int y, int big) {
    const struct surface *s = big ? &app32 : &app16;
    blend(c, x, y, s, 0, s->w, s->h, 0);
}

void ow_text_scaled(const struct canvas *c, int x, int y, const struct font *f, const char *s, u32 color, int scale) {
    if (scale <= 1) { text(c, x, y, f, s, color); return; }
    for (; *s; ++s) {
        unsigned ch = (unsigned char)*s;
        int width = f->widths[ch];
        for (int row = 0; row < f->height; ++row) {
            unsigned bits = f->rows[ch * f->height + row];
            for (int col = 0; col < width; ++col)
                if (bits >> col & 1) fill(c, x + col * scale, y + row * scale, scale, scale, color);
        }
        x += (width + f->spacing) * scale;
    }
}

void ow_text_fit(const struct canvas *c, int x, int y, int width, const struct font *f, const char *s, u32 color) {
    if (text_width(f, s) <= width) { text(c, x, y, f, s, color); return; }
    char cut[300];
    int n = (int)strlen(s);
    if (n > (int)sizeof(cut) - 4) n = (int)sizeof(cut) - 4;
    for (; n > 0; --n) {
        snprintf(cut, sizeof(cut), "%.*s...", n, s);
        if (text_width(f, cut) <= width) break;
    }
    if (n > 0) text(c, x, y, f, cut, color);
}

void ow_button(const struct canvas *c, struct rect r, const char *label, int primary, int hover) {
    u32 face = hover ? mix(app->accent, 0xffffff, 85) : primary ? mix(app->accent, 0xffffff, 90) : 0xfdfdfd;
    u32 edge = hover || primary ? mix(app->accent, 0xffffff, 30) : 0xababab;
    fill(c, r.x, r.y, r.w, r.h, face);
    frame_box(c, r.x, r.y, r.w, r.h, edge);
    text(c, r.x + (r.w - text_width(&font_regular, label)) / 2, r.y + (r.h - font_regular.height) / 2, &font_regular, label, OW_TEXT);
}

void ow_field(const struct canvas *c, const struct field *f, struct rect r, int focused, int secret) {
    fill(c, r.x, r.y, r.w, r.h, 0xffffff);
    frame_box(c, r.x, r.y, r.w, r.h, focused ? app->accent : 0xababab);
    struct field shown = *f;
    if (secret) for (size_t i = 0; shown.text[i]; ++i) shown.text[i] = '*';
    struct canvas inner = *c;
    inner.clip = rect_both(c->clip, (struct rect){r.x + 1, r.y + 1, r.w - 2, r.h - 2});
    char head[260];
    snprintf(head, sizeof(head), "%.*s", shown.cursor, shown.text);
    int caret = text_width(&font_regular, head), shift = caret > r.w - 12 ? caret - (r.w - 12) : 0;
    int ty = r.y + (r.h - font_regular.height) / 2;
    text(&inner, r.x + 5 - shift, ty, &font_regular, shown.text, 0x262626);
    if (focused) fill(&inner, r.x + 5 - shift + caret, ty - 1, 1, font_regular.height + 2, 0x262626);
}

void ow_status(const struct canvas *c, int y, int width, const char *left, const char *right) {
    fill(c, 0, y, width, STATUS_H, app->accent);
    text(c, 10, y + (STATUS_H - font_regular.height) / 2, &font_regular, left, app->on_accent);
    if (right) text(c, width - 12 - text_width(&font_regular, right), y + (STATUS_H - font_regular.height) / 2, &font_regular, right, app->on_accent);
}

static int thumb_of(const struct vscroll *s, struct rect r, int *size) {
    int total = s->total > s->visible ? s->total : s->visible;
    *size = total ? r.h * s->visible / total : r.h;
    if (*size < 24) *size = 24;
    if (*size > r.h) *size = r.h;
    int range = total - s->visible;
    return range > 0 ? (r.h - *size) * s->top / range : 0;
}

void ow_vscroll_paint(const struct canvas *c, const struct vscroll *s, struct rect r) {
    fill(c, r.x, r.y, r.w, r.h, 0xf0f0f0);
    if (s->total <= s->visible) return;
    int size, at = thumb_of(s, r, &size);
    fill(c, r.x + 3, r.y + at + 1, r.w - 6, size - 2, s->dragging ? 0x9a9a9a : 0xc2c3c9);
}

int ow_vscroll_mouse(struct vscroll *s, struct rect r, int x, int y, int kind) {
    if (kind == M_UP && s->dragging) { s->dragging = 0; return 1; }
    if (kind == M_MOVE && s->dragging) {
        int size;
        thumb_of(s, r, &size);
        int range = s->total - s->visible, room = r.h - size;
        if (room > 0) s->top = s->grab_top + (y - s->grab_y) * range / room;
        vscroll_clamp(s);
        return 1;
    }
    if (kind != M_DOWN || !rect_has(r, x, y)) return 0;
    if (s->total <= s->visible) return 1;
    int size, at = thumb_of(s, r, &size);
    if (y < r.y + at) s->top -= s->visible;
    else if (y >= r.y + at + size) s->top += s->visible;
    else { s->dragging = 1; s->grab_y = y; s->grab_top = s->top; }
    vscroll_clamp(s);
    return 1;
}

/* ---- recent files: %HOME%\OPENWORK.CFG holds "ICON|path" lines ---- */

static char recent[RECENT_MAX][260];
static int recent_count;

static void recent_load(void) {
    char path[260];
    home_path(path, sizeof(path), "OPENWORK.CFG");
    recent_count = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[320];
    size_t n = strlen(app->icon);
    while (fgets(line, sizeof(line), f) && recent_count < RECENT_MAX) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, app->icon, n) && line[n] == '|') snprintf(recent[recent_count++], sizeof(recent[0]), "%s", line + n + 1);
    }
    fclose(f);
}

static void recent_add(const char *file) {
    char path[260];
    home_path(path, sizeof(path), "OPENWORK.CFG");
    struct text keep = {0};
    size_t n = strlen(app->icon);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[320];
        while (fgets(line, sizeof(line), f))
            if (strncmp(line, app->icon, n) || line[n] != '|') text_put(&keep, line);
        fclose(f);
    }
    recent_load();
    f = fopen(path, "w");
    if (!f) { free(keep.data); return; }
    fprintf(f, "%s|%s\n", app->icon, file);
    for (int i = 0, written = 1; i < recent_count && written < RECENT_MAX; ++i)
        if (strcasecmp(recent[i], file)) { fprintf(f, "%s|%s\n", app->icon, recent[i]); ++written; }
    if (keep.data) fwrite(keep.data, 1, keep.size, f);
    fclose(f);
    free(keep.data);
    recent_load();
}

void suite_document(struct window *w, const char *path) {
    snprintf(suite_path, sizeof(suite_path), "%s", path ? path : "");
    char title[96];
    const char *name = strrchr(suite_path, '\\');
    snprintf(title, sizeof(title), "%s - %s", suite_path[0] ? (name ? name + 1 : suite_path) : app->untitled, app->name);
    if (w) window_title(w, title);
    if (suite_path[0]) recent_add(suite_path);
}

/* ---- the About dialog ---- */

static const struct rect about_ok = {336, 276, 84, 26};

static void about_paint(struct window *w, const struct canvas *c) {
    int W = w->r.w;
    fill(c, 0, 0, W, w->r.h, 0xffffff);
    static const u32 stripe[] = {0x2b579a, 0x217346, 0xc42b1c, 0xf0b400};
    for (int i = 0; i < 4; ++i) fill(c, i * W / 4, 0, W / 4 + 1, 4, stripe[i]);
    ow_text_scaled(c, 24, 22, &font_bold, "OpenWork", OW_TEXT, 3);
    text(c, 26, 58, &font_regular, "The office suite for DOS-XC", OW_FAINT);
    fill(c, 0, 84, W, 104, mix(app->accent, 0xffffff, 90));
    fill(c, 0, 84, 6, 104, app->accent);
    ow_app_icon(c, 26, 100, 1);
    ow_text_scaled(c, 72, 100, &font_bold, app->name, mix(accent_text(), 0, 15), 2);
    text(c, 72, 128, &font_regular, app->blurb, OW_TEXT);
    text(c, 72, 146, &font_regular, "Version " OPENWORK_VERSION, OW_FAINT);
    text(c, 72, 162, &font_regular, "Part of OpenWork", OW_FAINT);
    text(c, 24, 204, &font_regular, "OpenWork: OpenPapers, OpenNumbers, OpenShow and OpenMail.", OW_TEXT);
    text(c, 24, 222, &font_regular, "Copyright (c) 2026 Ben Daws.", OW_FAINT);
    ow_button(c, about_ok, "OK", 1, 0);
}
static void about_mouse(struct window *w, int x, int y, int kind, int value) {
    if (kind == M_DOWN && rect_has(about_ok, x, y)) window_close(w);
}
static void about_key(struct window *w, int key) { if (key == '\n' || key == 27 || key == ' ') window_close(w); }
static const struct app_ops about_ops = {about_paint, about_mouse, about_key};

void suite_about(void) {
    char title[64];
    snprintf(title, sizeof(title), "About %s", app->name);
    struct window *w = window_open(440, 316, title, ICON_INFO, &about_ops, 0);
    if (w) window_modal(w);
}

/* ---- the ribbon ---- */

static int add_hit(struct ribbon *rb, struct rect r, int kind, int a, const struct rb_item *item) {
    if (rb->hit_count == RB_HITS) return 0;
    rb->hits[rb->hit_count++] = (struct rb_hit){r, kind, a, item};
    return rb->hover == rb->hit_count - 1;
}

static void arrow_down(const struct canvas *c, int x, int y, u32 color) {
    for (int i = 0; i < 3; ++i) fill(c, x + i, y + i, 5 - 2 * i, 1, color);
}

static int label_width(const char *label) {
    int best = 0;
    char line[64];
    for (const char *p = label; *p;) {
        size_t n = strcspn(p, "\n");
        snprintf(line, sizeof(line), "%.*s", (int)n, p);
        int w = text_width(&font_regular, line);
        if (w > best) best = w;
        p += n + (p[n] == '\n');
    }
    return best;
}

static int item_width(const struct rb_item *it, int entries) {
    switch (it->kind) {
    case RB_LARGE: { int w = label_width(it->label) + 12; return w < 44 ? 44 : w; }
    case RB_COMBO: return it->width;
    case RB_GALLERY: return entries * it->width + 18;
    default: return 24 + (it->flags & RB_LABEL ? text_width(&font_regular, it->label) + 6 : 0) + (it->flags & RB_MENU ? 10 : 0);
    }
}

static void paint_item(struct ribbon *rb, const struct canvas *c, const struct rb_item *it, struct rect r, int hot) {
    int st = rb->state ? rb->state(it->command) : 0, off = st & RB_OFF, on = st & RB_ON;
    u32 color = off ? 0xb4b4b4 : OW_TEXT, tint = off ? 0xc8c8c8 : 0;
    if (it->kind == RB_COMBO) {
        fill(c, r.x, r.y + 1, r.w, r.h - 2, 0xffffff);
        frame_box(c, r.x, r.y + 1, r.w, r.h - 2, hot && !off ? mix(app->accent, 0xffffff, 40) : 0xc6c6c6);
        const char *v = rb->value ? rb->value(it->command) : "";
        struct canvas inner = *c;
        inner.clip = rect_both(c->clip, (struct rect){r.x + 2, r.y, r.w - 16, r.h});
        text(&inner, r.x + 5, r.y + (r.h - font_regular.height) / 2, &font_regular, v, color);
        arrow_down(c, r.x + r.w - 11, r.y + r.h / 2 - 1, color);
        return;
    }
    if (!off && (hot || on)) {
        fill(c, r.x, r.y, r.w, r.h, on ? mix(app->accent, 0xffffff, hot ? 65 : 75) : mix(app->accent, 0xffffff, 86));
        if (on) frame_box(c, r.x, r.y, r.w, r.h, mix(app->accent, 0xffffff, 45));
    }
    if (it->kind == RB_LARGE) {
        ow_icon(c, r.x + (r.w - 32) / 2, r.y + 5, it->icon, 32, tint);
        int y = r.y + 42;
        char line[64];
        for (const char *p = it->label; *p; y += 12) {
            size_t n = strcspn(p, "\n");
            snprintf(line, sizeof(line), "%.*s", (int)n, p);
            text(c, r.x + (r.w - text_width(&font_regular, line)) / 2, y, &font_regular, line, color);
            p += n + (p[n] == '\n');
        }
        if (it->flags & RB_MENU) arrow_down(c, r.x + r.w / 2 - 2, y + 2, color);
    } else {
        ow_icon(c, r.x + 4, r.y + 3, it->icon, 16, tint);
        if (it->flags & RB_LABEL) text(c, r.x + 24, r.y + (r.h - font_regular.height) / 2, &font_regular, it->label, color);
        if (it->flags & RB_MENU) arrow_down(c, r.x + r.w - 9, r.y + r.h / 2 - 1, color);
    }
}

/* Lays out (and with C, paints) the selected tab's groups; returns the width used. */
static int place(struct ribbon *rb, const struct canvas *c, int entries_cut) {
    const struct rb_tab *tab = &rb->tabs[rb->tab];
    int x = 4, top = TAB_H + 3, bottom = TAB_H + RIBBON_BODY_H;
    for (int g = 0; g < tab->count; ++g) {
        const struct rb_group *group = &tab->groups[g];
        int gx = x;
        x += 6;
        for (int i = 0; i < group->count;) {
            const struct rb_item *it = &group->items[i];
            if (it->kind == RB_LARGE || it->kind == RB_GALLERY) {
                int entries = it->count;
                if (it->kind == RB_GALLERY) { entries -= entries_cut; if (entries < 2) entries = 2; if (entries > it->count) entries = it->count; }
                int w = item_width(it, entries);
                if (c) {
                    if (it->kind == RB_LARGE) {
                        struct rect r = {x, top, w, bottom - top - 22};
                        paint_item(rb, c, it, r, add_hit(rb, r, H_ITEM, 0, it));
                    } else {
                        struct rect frame = {x, top + 2, w, bottom - top - 26};
                        fill(c, frame.x, frame.y, frame.w, frame.h, 0xffffff);
                        frame_box(c, frame.x, frame.y, frame.w, frame.h, 0xc6c6c6);
                        int chosen = rb->gallery_selected ? rb->gallery_selected(it->command) : -1;
                        for (int e = 0; e < entries; ++e) {
                            struct rect r = {frame.x + 2 + e * it->width, frame.y + 2, it->width - 2, frame.h - 4};
                            int hot = add_hit(rb, r, H_GALLERY, e, it);
                            if (hot || e == chosen) fill(c, r.x, r.y, r.w, r.h, mix(app->accent, 0xffffff, e == chosen ? 75 : 88));
                            if (e == chosen) frame_box(c, r.x, r.y, r.w, r.h, mix(app->accent, 0xffffff, 40));
                            struct canvas inner = *c;
                            inner.clip = rect_both(c->clip, r);
                            if (rb->gallery_paint) rb->gallery_paint(&inner, r, it->command, e, e == chosen);
                        }
                        struct rect more = {frame.x + frame.w - 15, frame.y, 15, frame.h};
                        int hot = add_hit(rb, more, H_MORE, 0, it);
                        fill(c, more.x, more.y + 1, more.w - 1, more.h - 2, hot ? mix(app->accent, 0xffffff, 86) : 0xf6f6f6);
                        fill(c, more.x, more.y, 1, more.h, 0xc6c6c6);
                        arrow_down(c, more.x + 5, more.y + more.h - 10, OW_TEXT);
                    }
                }
                x += w + 2;
                ++i;
                continue;
            }
            int rows[3] = {x, x, x};
            for (; i < group->count && (group->items[i].kind == RB_SMALL || group->items[i].kind == RB_COMBO); ++i) {
                const struct rb_item *s = &group->items[i];
                int row = s->row < 0 ? 0 : s->row > 2 ? 2 : s->row, w = item_width(s, 0);
                struct rect r = {rows[row], top + row * 23, w, 22};
                if (c) paint_item(rb, c, s, r, add_hit(rb, r, H_ITEM, 0, s));
                rows[row] += w + (s->kind == RB_COMBO ? 4 : 1);
            }
            int right = rows[0] > rows[1] ? rows[0] : rows[1];
            if (rows[2] > right) right = rows[2];
            x = right + 3;
        }
        x += 6;
        int lw = text_width(&font_regular, group->label);
        if (x - gx < lw + 16) x = gx + lw + 16;
        if (c) {
            text(c, gx + (x - gx - lw) / 2, bottom - 16, &font_regular, group->label, OW_FAINT);
            fill(c, x, top + 2, 1, bottom - top - 8, OW_LINE);
        }
        x += 1;
    }
    return x;
}

static const char *const page_names[] = {"Info", "New", "Open", "Save As"};

struct nav { const char *label; int page; };
static int nav_items(struct nav *out) {
    int n = 0;
    out[n++] = (struct nav){"Info", PAGE_INFO};
    if (app->pages & BS_NEW) out[n++] = (struct nav){"New", PAGE_NEW};
    if (app->pages & BS_OPEN) out[n++] = (struct nav){"Open", PAGE_OPEN};
    if (app->pages & BS_SAVE) out[n++] = (struct nav){"Save", ACT_SAVE};
    if (app->pages & BS_SAVE_AS) out[n++] = (struct nav){"Save As", PAGE_SAVE_AS};
    out[n++] = (struct nav){app->pages & BS_OPEN ? "Close" : "Exit", ACT_CLOSE};
    out[n++] = (struct nav){0, 0};
    out[n++] = (struct nav){"About", ACT_ABOUT};
    return n;
}

static void tile(struct ribbon *rb, const struct canvas *c, struct rect r, int kind, int icon, const char *line1, const char *line2) {
    int hot = add_hit(rb, r, kind, 0, 0);
    fill(c, r.x, r.y, r.w, r.h, hot ? mix(app->accent, 0xffffff, 86) : 0xffffff);
    frame_box(c, r.x, r.y, r.w, r.h, hot ? mix(app->accent, 0xffffff, 40) : 0xd4d4d4);
    ow_icon(c, r.x + 12, r.y + (r.h - 32) / 2, icon, 32, 0);
    text(c, r.x + 54, r.y + r.h / 2 - (line2 ? 12 : 5), &font_bold, line1, OW_TEXT);
    if (line2) text(c, r.x + 54, r.y + r.h / 2 + 3, &font_regular, line2, OW_FAINT);
}

static void blank_preview(const struct canvas *c, struct rect r) {
    fill(c, r.x, r.y, r.w, r.h, 0xffffff);
    frame_box(c, r.x, r.y, r.w, r.h, 0xc6c6c6);
    if (!strcmp(app->kind, "workbook")) {
        for (int x = r.x + 18; x < r.x + r.w; x += 28) fill(c, x, r.y + 1, 1, r.h - 2, 0xe0e0e0);
        for (int y = r.y + 14; y < r.y + r.h; y += 14) fill(c, r.x + 1, y, r.w - 2, 1, 0xe0e0e0);
    } else if (!strcmp(app->kind, "presentation")) {
        int h = r.w * 9 / 16, y = r.y + (r.h - h) / 2;
        fill(c, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 0xf3f3f3);
        fill(c, r.x + 6, y, r.w - 12, h, 0xffffff);
        frame_box(c, r.x + 6, y, r.w - 12, h, 0xd0d0d0);
        fill(c, r.x + 20, y + h / 2 - 8, r.w - 40, 6, 0xd8d8d8);
        fill(c, r.x + 34, y + h / 2 + 4, r.w - 68, 3, 0xe4e4e4);
    }
}

static void backstage_paint(struct ribbon *rb, struct window *w, const struct canvas *c) {
    int W = w->r.w, H = w->r.h;
    fill(c, 0, 0, W, H, 0xffffff);
    fill(c, 0, 0, NAV_W, H, app->accent);
    struct rect back = {14, 14, 36, 36};
    if (add_hit(rb, back, H_BACK, 0, 0)) fill(c, back.x, back.y, back.w, back.h, mix(app->accent, 0xffffff, 18));
    ow_icon(c, 16, 16, IC_BACK, 32, app->on_accent);
    struct nav nav[10];
    int n = nav_items(nav), y = 72;
    for (int i = 0; i < n; ++i) {
        if (!nav[i].label) { fill(c, 20, y + 8, NAV_W - 40, 1, mix(app->accent, app->on_accent, 35)); y += 18; continue; }
        struct rect r = {0, y, NAV_W, 34};
        int hot = add_hit(rb, r, H_NAV, nav[i].page, 0);
        if (nav[i].page >= 0 && nav[i].page == rb->page) fill(c, r.x, r.y, r.w, r.h, mix(app->accent, 0, 22));
        else if (hot) fill(c, r.x, r.y, r.w, r.h, mix(app->accent, 0xffffff, 16));
        text(c, 28, y + 12, &font_bold, nav[i].label, app->on_accent);
        y += 34;
    }
    int x0 = NAV_W + 36, y0 = 30, right = W - 24;
    ow_text_scaled(c, x0, y0, &font_regular, page_names[rb->page], OW_TEXT, 3);
    y0 += 54;
    struct canvas page = *c;
    page.clip = rect_both(c->clip, (struct rect){NAV_W, 0, W - NAV_W, H});
    if (rb->page == PAGE_INFO) {
        int x = x0;
        if (app->settings) {
            tile(rb, &page, (struct rect){x0, y0, 220, 64}, H_SETTINGS, IC_SETTINGS, "Account Settings", "Servers, sign-in and sending");
            y0 += 90;
        } else {
            const char *name = strrchr(suite_path, '\\');
            ow_text_scaled(&page, x0, y0, &font_bold, suite_path[0] ? (name ? name + 1 : suite_path) : app->untitled, OW_TEXT, 2);
            ow_text_fit(&page, x0, y0 + 26, right - x0, &font_regular, suite_path[0] ? suite_path : "Not saved yet", OW_FAINT);
            y0 += 60;
        }
        fill(&page, x0, y0, right - x0 > 420 ? 420 : right - x0, 1, OW_LINE);
        text(&page, x, y0 + 14, &font_bold, app->settings ? "Account" : "Properties", OW_TEXT);
        char lines[1024] = "";
        if (app->info) app->info(lines, sizeof(lines));
        int ly = y0 + 36;
        for (char *p = lines; *p; ly += 18) {
            char *end = strchr(p, '\n');
            if (end) *end = 0;
            char *tab = strchr(p, '\t');
            if (tab) *tab = 0;
            text(&page, x, ly, &font_regular, p, OW_FAINT);
            if (tab) ow_text_fit(&page, x + 120, ly, right - x - 120, &font_regular, tab + 1, OW_TEXT);
            if (!end) break;
            p = end + 1;
        }
    } else if (rb->page == PAGE_NEW) {
        struct rect r = {x0, y0, 150, 190};
        int hot = add_hit(rb, r, H_NEW, 0, 0);
        if (hot) fill(&page, r.x - 4, r.y - 4, r.w + 8, r.h + 30, mix(app->accent, 0xffffff, 88));
        blank_preview(&page, r);
        char label[48];
        snprintf(label, sizeof(label), "Blank %s", app->kind);
        text(&page, r.x, r.y + r.h + 8, &font_regular, label, OW_TEXT);
    } else if (rb->page == PAGE_OPEN || rb->page == PAGE_SAVE_AS) {
        int open = rb->page == PAGE_OPEN;
        tile(rb, &page, (struct rect){x0, y0, 200, 64}, open ? H_BROWSE_OPEN : H_BROWSE_SAVE, IC_COMPUTER, "Computer", "Browse");
        int x1 = x0 + 236;
        if (x1 + 120 > right) { x1 = x0; y0 += 90; }
        char heading[48];
        snprintf(heading, sizeof(heading), open ? "Recent %s" : "Formats", app->kinds);
        text(&page, x1, y0 + 2, &font_bold, heading, OW_TEXT);
        fill(&page, x1, y0 + 20, right - x1, 1, OW_LINE);
        int ly = y0 + 28;
        if (open) {
            if (!recent_count) text(&page, x1, ly + 6, &font_regular, "Files you open or save appear here.", OW_FAINT);
            for (int i = 0; i < recent_count && ly + 40 < H; ++i, ly += 40) {
                struct rect r = {x1, ly, right - x1, 38};
                if (add_hit(rb, r, H_RECENT, i, 0)) fill(&page, r.x, r.y, r.w, r.h, mix(app->accent, 0xffffff, 88));
                ow_app_icon(&page, r.x + 6, r.y + 11, 0);
                const char *slash = strrchr(recent[i], '\\');
                ow_text_fit(&page, r.x + 30, r.y + 7, r.w - 36, &font_bold, slash ? slash + 1 : recent[i], OW_TEXT);
                char folder[260];
                snprintf(folder, sizeof(folder), "%.*s", slash ? (int)(slash - recent[i]) : 0, recent[i]);
                ow_text_fit(&page, r.x + 30, r.y + 22, r.w - 36, &font_regular, folder, OW_FAINT);
            }
        } else {
            char formats[512];
            snprintf(formats, sizeof(formats), "%s", app->formats);
            for (char *p = strtok(formats, "\n"); p; p = strtok(0, "\n"), ly += 18) text(&page, x1, ly + 4, &font_regular, p, OW_TEXT);
        }
    }
}

int ribbon_paint(struct ribbon *rb, struct window *w, const struct canvas *c) {
    rb->hit_count = 0;
    if (rb->backstage) { backstage_paint(rb, w, c); return 1; }
    int W = w->r.w;
    fill(c, 0, 0, W, RIBBON_H, 0xffffff);
    struct rect file = {0, 0, 56, TAB_H};
    fill(c, file.x, file.y, file.w, file.h, add_hit(rb, file, H_FILE, 0, 0) ? mix(app->accent, 0xffffff, 15) : app->accent);
    text(c, (file.w - text_width(&font_regular, "FILE")) / 2, 7, &font_regular, "FILE", app->on_accent);
    fill(c, 0, TAB_H - 1, W, 1, OW_BORDER);
    int x = 60;
    for (int i = 0; i < rb->tab_count; ++i) {
        int lw = text_width(&font_regular, rb->tabs[i].label);
        struct rect r = {x, 2, lw + 22, TAB_H - 2};
        int hot = add_hit(rb, r, H_TAB, i, 0);
        if (i == rb->tab) {
            fill(c, r.x, r.y, r.w, r.h + 1, 0xffffff);
            fill(c, r.x, r.y, r.w, 1, OW_BORDER);
            fill(c, r.x, r.y, 1, r.h, OW_BORDER);
            fill(c, r.x + r.w - 1, r.y, 1, r.h, OW_BORDER);
        }
        text(c, r.x + 11, 7, &font_regular, rb->tabs[i].label, i == rb->tab || hot ? accent_text() : OW_TEXT);
        x += r.w + 2;
    }
    struct rect help = {W - 28, 2, 22, 20};
    if (add_hit(rb, help, H_HELP, 0, 0)) fill(c, help.x, help.y, help.w, help.h, mix(app->accent, 0xffffff, 86));
    ow_icon(c, help.x + 3, help.y + 2, IC_HELP, 16, 0);
    fill(c, 0, RIBBON_H - 1, W, 1, OW_BORDER);
    int saved = rb->hit_count, need = place(rb, 0, 0), cut = 0;
    if (need > W) {
        const struct rb_tab *tab = &rb->tabs[rb->tab];
        for (int g = 0; g < tab->count; ++g)
            for (int i = 0; i < tab->groups[g].count; ++i)
                if (tab->groups[g].items[i].kind == RB_GALLERY) cut = (need - W + tab->groups[g].items[i].width - 1) / tab->groups[g].items[i].width;
    }
    rb->hit_count = saved;
    struct canvas body = *c;
    body.clip = rect_both(c->clip, (struct rect){0, TAB_H, W, RIBBON_BODY_H - 1});
    place(rb, &body, cut);
    return 0;
}

static void opened(const char *path, void *data) { if (path && app->open_file) app->open_file(path); }

static void close_window(struct window *w) {
    if (!w->ops->closing || w->ops->closing(w)) window_close(w);
}

static void click(struct ribbon *rb, struct window *w, const struct rb_hit *h) {
    const struct rb_item *it = h->item;
    switch (h->kind) {
    case H_FILE: rb->backstage = 1; rb->page = PAGE_INFO; recent_load(); break;
    case H_TAB: rb->tab = h->a; break;
    case H_HELP: suite_about(); break;
    case H_ITEM:
        if (rb->state && rb->state(it->command) & RB_OFF) break;
        if (it->kind == RB_COMBO || it->flags & RB_MENU) { if (rb->menu) window_popup(w, h->r.x, h->r.y + h->r.h, rb->menu(it->command), RB_TAG + it->command); }
        else rb->command(it->command, -1);
        break;
    case H_GALLERY: rb->command(it->command, h->a); break;
    case H_MORE: if (rb->menu) window_popup(w, h->r.x, h->r.y + h->r.h, rb->menu(it->command), RB_TAG + it->command); break;
    case H_BACK: rb->backstage = 0; break;
    case H_NAV:
        if (h->a >= 0) { rb->page = h->a; break; }
        if (h->a == ACT_SAVE) {
            if (suite_path[0]) { rb->backstage = 0; app->save(); }
            else rb->page = PAGE_SAVE_AS;
        } else if (h->a == ACT_CLOSE) { rb->backstage = 0; close_window(w); }
        else if (h->a == ACT_ABOUT) suite_about();
        break;
    case H_NEW: rb->backstage = 0; app->new_file(); break;
    case H_BROWSE_OPEN: rb->backstage = 0; file_dialog("Open", 0, 0, opened, 0); break;
    case H_BROWSE_SAVE: rb->backstage = 0; app->save_as(); break;
    case H_RECENT: rb->backstage = 0; if (h->a < recent_count) { char path[260]; snprintf(path, sizeof(path), "%s", recent[h->a]); app->open_file(path); } break;
    case H_SETTINGS: rb->backstage = 0; app->settings(); break;
    }
}

int ribbon_mouse(struct ribbon *rb, struct window *w, int x, int y, int kind) {
    int found = -1;
    for (int i = 0; i < rb->hit_count; ++i)
        if (rect_has(rb->hits[i].r, x, y)) { found = i; break; }
    if (kind == M_MOVE && found != rb->hover) { rb->hover = found; window_paint(w); }
    if (kind == M_DOWN && found >= 0) {
        struct rb_hit h = rb->hits[found];
        rb->hover = -1;
        click(rb, w, &h);
        window_paint(w);
        return 1;
    }
    return rb->backstage || y < RIBBON_H;
}

int ribbon_key(struct ribbon *rb, struct window *w, int key) {
    if (!rb->backstage) return 0;
    if (key == 27) { rb->backstage = 0; window_paint(w); }
    return 1;
}

int ribbon_popup(struct ribbon *rb, struct window *w, int tag, int index) {
    if (tag < RB_TAG || tag >= RB_TAG + 1000) return 0;
    if (index >= 0) rb->command(tag - RB_TAG, index);
    window_paint(w);
    return 1;
}

void ribbon_file(struct ribbon *rb, struct window *w, int page) {
    rb->backstage = 1;
    rb->page = page;
    recent_load();
    window_paint(w);
}
