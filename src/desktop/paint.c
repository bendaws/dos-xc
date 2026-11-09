/*
 * DOS-XC /desktop/paint.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * PAINT.EXE: an image editor.  Opens BMP, PNG and JPEG, saves BMP.
 */
#include "wm.h"
#include <strings.h>

#define TOOL_W 56
#define PALETTE_H 44
#define VIEW_X TOOL_W
#define VIEW_Y MENUBAR_H

static const struct menubar bar = {
    {"File", "Edit", "Image"},
    {"New\nOpen...\nSave\nSave As...\n-\nExit\n", "Undo\n", "Clear\nSet as Wallpaper\n"},
};
enum { T_PENCIL, T_BRUSH, T_ERASER, T_LINE, T_RECT, T_FILL_RECT, T_ELLIPSE, T_FILL_ELLIPSE, T_BUCKET, T_PICK, T_COUNT };
static const char *const tool_names[T_COUNT] = {"Pencil", "Brush", "Eraser", "Line", "Box", "Solid", "Oval", "Disk", "Fill", "Pick"};
static const u32 palette[28] = {
    0x000000, 0x808080, 0x800000, 0x808000, 0x008000, 0x008080, 0x000080, 0x800080, 0x808040, 0x004040, 0x0080ff, 0x004080, 0x8000ff, 0x804000,
    0xffffff, 0xc0c0c0, 0xff0000, 0xffff00, 0x00ff00, 0x00ffff, 0x0000ff, 0xff00ff, 0xffff80, 0x00ff80, 0x80ffff, 0x8080ff, 0xff0080, 0xff8040};

static struct surface picture, undo_copy;
static int tool = T_BRUSH, undo_valid, drawing, start_x, start_y, last_x, last_y;
static u32 fg = 0x000000, bg = 0xffffff;
static int pan_x, pan_y;
static char path[300];
static int modified, pending;
static struct window *main_w;
static void update_title(void);

static int view_w(const struct window *w) { return w->content.w - TOOL_W; }
static int view_h(const struct window *w) { return w->content.h - MENUBAR_H - PALETTE_H; }

static void clamp_pan(const struct window *w) {
    int most_x = picture.w - view_w(w), most_y = picture.h - view_h(w);
    if (pan_x > most_x) pan_x = most_x;
    if (pan_y > most_y) pan_y = most_y;
    if (pan_x < 0) pan_x = 0;
    if (pan_y < 0) pan_y = 0;
}

static void plot(int x, int y, u32 c) { if (x >= 0 && y >= 0 && x < picture.w && y < picture.h) picture.px[y * picture.w + x] = c; }

static void plot_round(int x, int y, int r, u32 c) {
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx)
            if (dx * dx + dy * dy <= r * r) plot(x + dx, y + dy, c);
}

static void draw_line(int x0, int y0, int x1, int y1, int r, u32 c) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        plot_round(x0, y0, r, c);
        if (x0 == x1 && y0 == y1) break;
        int e = 2 * err;
        if (e >= dy) { err += dy; x0 += sx; }
        if (e <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_box(int x0, int y0, int x1, int y1, int solid, u32 c) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            if (solid || y == y0 || y == y1 || x == x0 || x == x1) plot(x, y, c);
}

static void draw_ellipse(int x0, int y0, int x1, int y1, int solid, u32 c) {
    double cx = (x0 + x1) / 2.0, cy = (y0 + y1) / 2.0, rx = abs(x1 - x0) / 2.0, ry = abs(y1 - y0) / 2.0;
    if (rx < 0.5) rx = 0.5;
    if (ry < 0.5) ry = 0.5;
    int top = y0 < y1 ? y0 : y1, bottom = y0 < y1 ? y1 : y0, left = x0 < x1 ? x0 : x1, right = x0 < x1 ? x1 : x0;
    for (int y = top; y <= bottom; ++y)
        for (int x = left; x <= right; ++x) {
            double a = (x - cx) / rx, b = (y - cy) / ry, v = a * a + b * b;
            if (solid ? v <= 1.0 : (v <= 1.0 && v >= 1.0 - 2.2 / (rx < ry ? rx : ry))) plot(x, y, c);
        }
}

static void flood(int x, int y, u32 c) {
    if (x < 0 || y < 0 || x >= picture.w || y >= picture.h) return;
    u32 old = picture.px[y * picture.w + x];
    if (old == c) return;
    int *stack = malloc((size_t)picture.w * picture.h * 8);
    if (!stack) return;
    int n = 0;
    stack[n++] = x;
    stack[n++] = y;
    while (n) {
        int py = stack[--n], px = stack[--n];
        int left = px;
        while (left > 0 && picture.px[py * picture.w + left - 1] == old) --left;
        int above = 0, below = 0;
        for (int i = left; i < picture.w && picture.px[py * picture.w + i] == old; ++i) {
            picture.px[py * picture.w + i] = c;
            if (py > 0) {
                int m = picture.px[(py - 1) * picture.w + i] == old;
                if (m && !above) { stack[n++] = i; stack[n++] = py - 1; }
                above = m;
            }
            if (py + 1 < picture.h) {
                int m = picture.px[(py + 1) * picture.w + i] == old;
                if (m && !below) { stack[n++] = i; stack[n++] = py + 1; }
                below = m;
            }
        }
    }
    free(stack);
}

static void save_undo(void) {
    surface_size(&undo_copy, picture.w, picture.h);
    memcpy(undo_copy.px, picture.px, (size_t)picture.w * picture.h * 4);
    undo_valid = 1;
}

static void restore_from_undo(void) { memcpy(picture.px, undo_copy.px, (size_t)picture.w * picture.h * 4); }

static void new_picture(int w, int h) {
    surface_size(&picture, w, h);
    for (int i = 0; i < w * h; ++i) picture.px[i] = 0xffffff;
    undo_valid = 0;
    pan_x = pan_y = 0;
}

static struct rect tool_rect(int t) { return (struct rect){4 + (t % 2) * 25, VIEW_Y + 4 + (t / 2) * 25, 24, 24}; }
static struct rect swatch_rect(const struct window *w, int i) { return (struct rect){60 + (i % 14) * 20, w->content.h - PALETTE_H + 4 + (i / 14) * 18, 18, 16}; }

static void tool_glyph(const struct canvas *c, int t, struct rect r) {
    int x = r.x + 5, y = r.y + 5;
    switch (t) {
    case T_PENCIL: for (int i = 0; i < 11; ++i) fill(c, x + i, y + 11 - i, 2, 2, C_TEXT); break;
    case T_BRUSH: for (int i = 0; i < 7; ++i) fill(c, x + i, y + 7 - i, 2, 2, 0x804000); fill(c, x - 1, y + 8, 6, 6, 0xc04040); break;
    case T_ERASER: fill(c, x + 1, y + 4, 12, 8, 0xe07070); frame_box(c, x + 1, y + 4, 12, 8, C_TEXT); break;
    case T_LINE: for (int i = 0; i < 13; ++i) fill(c, x + i, y + 12 - i, 1, 2, C_TEXT); break;
    case T_RECT: frame_box(c, x, y + 1, 14, 10, C_TEXT); break;
    case T_FILL_RECT: fill(c, x, y + 1, 14, 10, C_TEXT); break;
    case T_ELLIPSE: frame_box(c, x + 3, y, 8, 12, C_TEXT); fill(c, x, y + 3, 14, 6, C_WINDOW); frame_box(c, x, y + 3, 14, 6, C_TEXT); break;
    case T_FILL_ELLIPSE: fill(c, x + 3, y, 8, 12, C_TEXT); fill(c, x, y + 3, 14, 6, C_TEXT); break;
    case T_BUCKET: fill(c, x + 2, y + 3, 10, 9, 0x6060c0); frame_box(c, x + 2, y + 3, 10, 9, C_TEXT); fill(c, x + 11, y + 8, 3, 4, 0x2040e0); break;
    default: fill(c, x + 6, y, 3, 8, C_TEXT); fill(c, x + 4, y + 7, 7, 3, C_TEXT); fill(c, x + 7, y + 10, 1, 4, C_TEXT); break;
    }
}

static void paint_paint(struct window *w, const struct canvas *c) {
    menubar_paint(c, &bar, w->content.w);
    fill(c, 0, VIEW_Y, TOOL_W, w->content.h - VIEW_Y, C_WINDOW);
    fill(c, TOOL_W - 2, VIEW_Y, 2, w->content.h - VIEW_Y, C_SHADOW1);
    for (int t = 0; t < T_COUNT; ++t) {
        struct rect r = tool_rect(t);
        fill(c, r.x, r.y, r.w, r.h, t == tool ? C_HOVER : C_WINDOW);
        bevel(c, r.x, r.y, r.w, r.h, t == tool);
        tool_glyph(c, t, r);
    }
    text(c, 6, VIEW_Y + 4 + (T_COUNT / 2) * 25 + 2, &font_regular, tool_names[tool], C_TEXT);
    /* the picture */
    struct rect view = {VIEW_X, VIEW_Y, view_w(w), view_h(w)};
    fill(c, view.x, view.y, view.w, view.h, 0x808080);
    struct canvas inner = *c;
    inner.clip = rect_both(c->clip, view);
    blit(&inner, VIEW_X - pan_x, VIEW_Y - pan_y, &picture);
    int py = w->content.h - PALETTE_H;
    fill(c, 0, py, w->content.w, PALETTE_H, C_WINDOW);
    fill(c, 0, py, w->content.w, 1, C_SHADOW1);
    bevel(c, 8, py + 6, 44, 34, 1);
    fill(c, 22, py + 14, 22, 18, bg);
    frame_box(c, 22, py + 14, 22, 18, C_TEXT);
    fill(c, 12, py + 8, 22, 18, fg);
    frame_box(c, 12, py + 8, 22, 18, C_TEXT);
    for (int i = 0; i < 28; ++i) {
        struct rect s = swatch_rect(w, i);
        fill(c, s.x, s.y, s.w, s.h, palette[i]);
        bevel(c, s.x, s.y, s.w, s.h, 1);
    }
    char status[64];
    snprintf(status, sizeof(status), "%d x %d   Wheel or arrows scroll.", picture.w, picture.h);
    text(c, 346, py + 14, &font_regular, status, C_TEXT);
}

static void commit_shape(int x, int y) {
    restore_from_undo();
    switch (tool) {
    case T_LINE: draw_line(start_x, start_y, x, y, 0, fg); break;
    case T_RECT: draw_box(start_x, start_y, x, y, 0, fg); break;
    case T_FILL_RECT: draw_box(start_x, start_y, x, y, 1, fg); break;
    case T_ELLIPSE: draw_ellipse(start_x, start_y, x, y, 0, fg); break;
    case T_FILL_ELLIPSE: draw_ellipse(start_x, start_y, x, y, 1, fg); break;
    }
}

static void stroke(int x, int y) {
    switch (tool) {
    case T_PENCIL: draw_line(last_x, last_y, x, y, 0, fg); break;
    case T_BRUSH: draw_line(last_x, last_y, x, y, 2, fg); break;
    case T_ERASER: draw_line(last_x, last_y, x, y, 6, bg); break;
    default: commit_shape(x, y); break;
    }
}

static void paint_mouse(struct window *w, int x, int y, int kind, int value) {
    if (menubar_mouse(w, &bar, x, y, kind)) return;
    if (kind == M_WHEEL) { pan_y -= value * 40; clamp_pan(w); window_paint(w); return; }
    int px = x - VIEW_X + pan_x, py = y - VIEW_Y + pan_y;
    if ((kind == M_DOWN || kind == M_DOUBLE || kind == M_RIGHT)) {
        for (int t = 0; t < T_COUNT; ++t)
            if (rect_has(tool_rect(t), x, y)) { tool = t; window_paint(w); return; }
        for (int i = 0; i < 28; ++i)
            if (rect_has(swatch_rect(w, i), x, y)) {
                if (kind == M_RIGHT) bg = palette[i]; else fg = palette[i];
                window_paint(w);
                return;
            }
        if (kind == M_RIGHT || x < VIEW_X || y < VIEW_Y || y >= w->content.h - PALETTE_H) return;
        if (tool == T_PICK) {
            if (px >= 0 && py >= 0 && px < picture.w && py < picture.h) fg = picture.px[py * picture.w + px];
            window_paint(w);
            return;
        }
        save_undo();
        modified = 1;
        update_title();
        if (tool == T_BUCKET) { flood(px, py, fg); window_paint(w); return; }
        drawing = 1;
        start_x = last_x = px;
        start_y = last_y = py;
        stroke(px, py);
        window_paint(w);
    } else if (kind == M_MOVE && drawing) {
        stroke(px, py);
        last_x = px;
        last_y = py;
        window_paint(w);
    } else if (kind == M_UP && drawing) {
        stroke(px, py);
        drawing = 0;
        window_paint(w);
    }
}

static void paint_key(struct window *w, int key) {
    switch (key) {
    case 0x101: pan_y -= 40; break;
    case 0x102: pan_y += 40; break;
    case 0x104: pan_x -= 40; break;
    case 0x103: pan_x += 40; break;
    case 26:
        if (undo_valid) restore_from_undo();
        break;
    case 19:
        if (path[0]) { if (image_save_bmp(path, &picture)) message_box("Paint", "The picture could not be saved.", path); else { modified = 0; update_title(); } }
        return;
    default: return;
    }
    clamp_pan(w);
    window_paint(w);
}

static void update_title(void) {
    char title[80];
    const char *name = path[0] ? strrchr(path, '\\') : 0;
    name = name ? name + 1 : path[0] ? path : "untitled";
    snprintf(title, sizeof(title), "%s%s - Paint", modified ? "*" : "", name);
    window_title(main_w, title);
}

static int load(const char *file) {
    struct surface fresh = {0};
    if (image_load(file, &fresh)) { message_box("Paint", "The picture could not be opened.", "BMP, PNG and JPEG can be read."); return -1; }
    free(picture.px);
    picture = fresh;
    undo_valid = 0;
    pan_x = pan_y = 0;
    const char *dot = strrchr(file, '.');
    if (dot && !strcasecmp(dot, ".BMP")) snprintf(path, sizeof(path), "%s", file);
    else path[0] = 0;                   /* saved as BMP under a new name */
    modified = 0;
    update_title();
    window_paint(main_w);
    return 0;
}

static void opened(const char *file, void *data) { (void)data; if (file) load(file); }

static void after_save_action(void);

static void write_to(const char *file) {
    char name[320];
    snprintf(name, sizeof(name), "%s", file);
    size_t n = strlen(name);
    if (n < 4 || strcasecmp(name + n - 4, ".BMP")) snprintf(name + n, sizeof(name) - n, ".BMP");
    if (image_save_bmp(name, &picture)) { message_box("Paint", "The picture could not be saved.", name); pending = 0; return; }
    snprintf(path, sizeof(path), "%s", name);
    modified = 0;
    update_title();
    after_save_action();
}

static void saved_as(const char *file, void *data) { (void)data; if (file) write_to(file); else pending = 0; }

static void after_save_action(void) {
    int action = pending;
    pending = 0;
    if (action == 1) window_close(main_w);
    else if (action == 2) file_dialog("Open", 0, 0, opened, 0);
    else if (action == 3) { new_picture(800, 500); path[0] = 0; update_title(); window_paint(main_w); }
}

static void save_or_ask(void) {
    if (path[0]) write_to(path);
    else file_dialog("Save As", 1, 0, saved_as, 0);
}

static void answered(int answer, void *data) {
    (void)data;
    if (answer == 1) save_or_ask();
    else if (answer == 0) { modified = 0; after_save_action(); }
    else pending = 0;
}

static void guard(int action) {
    pending = action;
    if (modified) ask_box("Paint", "Save the changes to this picture?", "Save", "Don't Save", answered, 0);
    else after_save_action();
}

static int paint_closing(struct window *w) {
    (void)w;
    if (!modified) return 1;
    guard(1);
    return 0;
}

static void paint_popup(struct window *w, int tag, int index) {
    if (index < 0) return;
    if (tag == 0) {
        switch (index) {
        case 0: guard(3); break;
        case 1: guard(2); break;
        case 2: save_or_ask(); break;
        case 3: file_dialog("Save As", 1, 0, saved_as, 0); break;
        case 5: if (paint_closing(w)) window_close(w); break;
        }
    } else if (tag == 1 && index == 0) {
        if (undo_valid) restore_from_undo();
        window_paint(w);
    } else if (tag == 2) {
        if (index == 0) {
            save_undo();
            for (int i = 0; i < picture.w * picture.h; ++i) picture.px[i] = bg;
            modified = 1;
            update_title();
            window_paint(w);
        } else {
            const char *home = getenv("HOME");
            struct dosxc_sysinfo info;
            dosxc_sys_sysinfo(&info);
            char folder[300], file[340];
            snprintf(folder, sizeof(folder), "%c:%s\\Wallpapers", info.drive, home && *home == '\\' ? home : "\\Home");
            dosxc_sys_mkdir(folder);
            snprintf(file, sizeof(file), "%s\\From Paint.bmp", folder);
            if (image_save_bmp(file, &picture)) message_box("Paint", "The picture could not be saved.", file);
            else gui_set_wallpaper(file);
        }
    }
}

static void paint_resized(struct window *w) { clamp_pan(w); }

static const struct app_ops paint_ops = {paint_paint, paint_mouse, paint_key, 0, paint_popup, 0, paint_resized, paint_closing};

int main(int argc, char **argv) {
    if (gui_connect()) return 1;
    new_picture(800, 500);
    main_w = window_open(TOOL_W + 800, MENUBAR_H + 500 + PALETTE_H, "untitled - Paint", ICON_PAINT, &paint_ops, 0);
    if (!main_w) return 1;
    main_w->resizable = 1;
    if (argc > 1) load(argv[1]);
    gui_run();
    return 0;
}
