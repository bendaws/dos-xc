/*
 * DOS-XC /desktop/apps.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "wm.h"
#include <ctype.h>
#include <strings.h>

extern const unsigned *about_logo(void);
void save_layout(void);
int sound_level(void);

static int hit(struct rect r, int x, int y) { return rect_has(r, x, y); }

static const struct rect about_ok = {200, 118, 80, 24};

static void about_paint(struct window *w, const struct canvas *c) {
    (void)w;
    struct dosxc_sysinfo info;
    dosxc_sys_sysinfo(&info);
    draw_pixels(c, 20, 18, 32, 32, about_logo());
    text(c, 72, 18, &font_bold, info.version, C_TEXT);
    text(c, 72, 34, &font_regular, "DOS eXtended Core", C_TEXT);
    char line[80];
    snprintf(line, sizeof(line), "%u-bit edition, %s boot", info.bits, info.firmware == 'U' ? "UEFI" : "BIOS");
    text(c, 72, 58, &font_regular, line, C_TEXT);
    snprintf(line, sizeof(line), "%lu MiB memory, %lu MiB free", (unsigned long)(info.memory_total >> 20), (unsigned long)(info.memory_free >> 20));
    text(c, 72, 74, &font_regular, line, C_TEXT);
    snprintf(line, sizeof(line), "%d display%s", screen_count, screen_count == 1 ? "" : "s");
    text(c, 72, 90, &font_regular, line, C_TEXT);
    button(c, about_ok, "OK", 0, 0);
}
static void about_mouse(struct window *w, int x, int y, int kind, int value) {
    (void)value;
    if (kind == M_DOWN && hit(about_ok, x, y)) window_close(w);
}
static void about_key(struct window *w, int key) { if (key == '\n' || key == 27) window_close(w); }
static const struct app_ops about_ops = {about_paint, about_mouse, about_key, 0};

void open_about(void) { window_open(296, 152, "About DOS-XC", ICON_INFO, &about_ops, 0); }

#define MAP_X 12
#define MAP_Y 12
#define MAP_W 436
#define MAP_H 200
struct pending { int x, y, w, h, mode; };
static struct settings {
    struct window *w;
    struct pending p[MAX_SCREENS];
    int selected, main;
    int dragging, grab_x, grab_y, start_x, start_y, drag_den;   /* the map's scale can change mid-drag */
    int scale;
} *st;

static const struct rect set_prev = {120, 228, 24, 22}, set_next = {280, 228, 24, 22};
static const struct rect set_main = {12, 292, 110, 24}, set_identify = {130, 292, 90, 24};
static const struct rect set_apply = {368, 292, 80, 24};
static const int scale_values[3] = {100, 125, 150};
static struct rect scale_button(int i) { return (struct rect){120 + i * 60, 260, 56, 22}; }

static void map_transform(int *scale_den, int *ox, int *oy) {
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    for (int i = 0; i < screen_count; ++i) {
        struct pending *p = &st->p[i];
        if (p->x < x0) x0 = p->x;
        if (p->y < y0) y0 = p->y;
        if (p->x + p->w > x1) x1 = p->x + p->w;
        if (p->y + p->h > y1) y1 = p->y + p->h;
    }
    int w = x1 - x0, h = y1 - y0;
    int den = 1;
    while (w / den > MAP_W - 20 || h / den > MAP_H - 20) ++den;
    *scale_den = den;
    *ox = MAP_X + (MAP_W - w / den) / 2 - x0 / den;
    *oy = MAP_Y + (MAP_H - h / den) / 2 - y0 / den;
}

static void settings_paint(struct window *w, const struct canvas *c) {
    (void)w;
    bevel(c, MAP_X - 2, MAP_Y - 2, MAP_W + 4, MAP_H + 4, 1);
    fill(c, MAP_X, MAP_Y, MAP_W, MAP_H, C_DESKTOP);
    int den, ox, oy;
    map_transform(&den, &ox, &oy);
    for (int i = 0; i < screen_count; ++i) {
        struct pending *p = &st->p[i];
        int x = ox + p->x / den, y = oy + p->y / den, mw = p->w / den, mh = p->h / den;
        u32 color = i == st->selected ? C_SELECTION : 0x808080;
        fill(c, x, y, mw, mh, color);
        frame_box(c, x, y, mw, mh, 0xffffff);
        char label[16];
        snprintf(label, sizeof(label), "%d", i + 1);
        text_big(c, x + mw / 2 - 8, y + mh / 2 - 12, label, 0xffffff, 2);
        if (i == st->main) text(c, x + 6, y + 5, &font_bold, "Main", 0xffffff);
    }
    struct pending *sel = &st->p[st->selected];
    char line[64];
    snprintf(line, sizeof(line), "Display %d:", st->selected + 1);
    text(c, 12, 233, &font_bold, line, C_TEXT);
    button(c, set_prev, "<", 0, 0);
    snprintf(line, sizeof(line), "%d x %d", sel->w, sel->h);
    text(c, 144 + (136 - text_width(&font_regular, line)) / 2, 233, &font_regular, line, C_TEXT);
    button(c, set_next, ">", 0, 0);
    text(c, 12, 265, &font_bold, "Scale:", C_TEXT);
    for (int i = 0; i < 3; ++i) {
        char label[8];
        snprintf(label, sizeof(label), "%d%%", scale_values[i]);
        button(c, scale_button(i), label, st->scale == scale_values[i], st->scale == scale_values[i]);
    }
    button(c, set_main, "Make main", 0, 0);
    button(c, set_identify, "Identify", 0, 0);
    button(c, set_apply, "Apply", 0, 1);
}

static void settings_snap(void) {
    struct pending *s = &st->p[st->selected];
    int best = 1 << 30, bx = s->x, by = s->y;
    for (int i = 0; i < screen_count; ++i) {
        if (i == st->selected) continue;
        struct pending *o = &st->p[i];
        int cx[4] = {o->x - s->w, o->x + o->w, s->x, s->x};
        int cy[4] = {s->y, s->y, o->y - s->h, o->y + o->h};
        for (int k = 0; k < 4; ++k) {
            int x = cx[k], y = cy[k];
            if (k < 2) {
                if (y < o->y - s->h + 1) y = o->y - s->h + 1;
                if (y > o->y + o->h - 1) y = o->y + o->h - 1;
                if (abs(y - o->y) < 40) y = o->y;
            } else {
                if (x < o->x - s->w + 1) x = o->x - s->w + 1;
                if (x > o->x + o->w - 1) x = o->x + o->w - 1;
                if (abs(x - o->x) < 40) x = o->x;
            }
            int distance = abs(x - s->x) + abs(y - s->y);
            int clear = 1;
            for (int j = 0; j < screen_count; ++j) {
                if (j == st->selected) continue;
                struct rect a = {x, y, s->w, s->h}, b = {st->p[j].x, st->p[j].y, st->p[j].w, st->p[j].h};
                if (rect_meets(a, b)) clear = 0;
            }
            if (clear && distance < best) { best = distance; bx = x; by = y; }
        }
    }
    s->x = bx;
    s->y = by;
    int x0 = 1 << 30, y0 = 1 << 30;
    for (int i = 0; i < screen_count; ++i) {
        if (st->p[i].x < x0) x0 = st->p[i].x;
        if (st->p[i].y < y0) y0 = st->p[i].y;
    }
    for (int i = 0; i < screen_count; ++i) { st->p[i].x -= x0; st->p[i].y -= y0; }
}

static void settings_step(int delta) {
    struct pending *p = &st->p[st->selected];
    const struct screen *s = &screens[st->selected];
    int m = p->mode + delta;
    if (m < 0 || m >= s->mode_count) return;
    p->mode = m;
    p->w = (int)s->modes[m].width;
    p->h = (int)s->modes[m].height;
    settings_snap();
    window_paint(st->w);
}

static void settings_apply(void) {
    for (int i = 0; i < screen_count; ++i)
        if (st->p[i].w * 100 / st->scale < 640 || st->p[i].h * 100 / st->scale < 480) {
            message_box("Display Settings", "That scale is too large for this resolution.", "The desktop needs at least 640 x 480 after scaling");
            return;
        }
    for (int i = 0; i < screen_count; ++i) {
        screens[i].r.x = st->p[i].x * 100 / st->scale;
        screens[i].r.y = st->p[i].y * 100 / st->scale;
        screens[i].mode = st->p[i].mode;
    }
    main_screen = st->main;
    set_ui_scale(st->scale);
    layout_changed();
    save_layout();
    window_paint(st->w);
}

static void settings_mouse(struct window *w, int x, int y, int kind, int value) {
    (void)w; (void)value;
    if (kind == M_DOWN || kind == M_DOUBLE) {
        if (hit(set_prev, x, y)) { settings_step(-1); return; }
        if (hit(set_next, x, y)) { settings_step(1); return; }
        if (hit(set_main, x, y)) { st->main = st->selected; window_paint(w); return; }
        if (hit(set_identify, x, y)) { identify_screens(); return; }
        if (hit(set_apply, x, y)) { settings_apply(); return; }
        for (int i = 0; i < 3; ++i)
            if (hit(scale_button(i), x, y)) { st->scale = scale_values[i]; window_paint(w); return; }
        int den, ox, oy;
        map_transform(&den, &ox, &oy);
        for (int i = screen_count - 1; i >= 0; --i) {
            struct pending *p = &st->p[i];
            struct rect r = {ox + p->x / den, oy + p->y / den, p->w / den, p->h / den};
            if (hit(r, x, y)) {
                st->selected = i;
                st->dragging = 1;
                st->grab_x = x;
                st->grab_y = y;
                st->start_x = p->x;
                st->start_y = p->y;
                st->drag_den = den;
                window_paint(w);
                return;
            }
        }
    } else if (kind == M_MOVE && st->dragging) {
        struct pending *p = &st->p[st->selected];
        p->x = st->start_x + (x - st->grab_x) * st->drag_den;
        p->y = st->start_y + (y - st->grab_y) * st->drag_den;
        window_paint(w);
    } else if (kind == M_UP && st->dragging) {
        st->dragging = 0;
        settings_snap();
        window_paint(w);
    }
}

static void settings_closed(struct window *w) { (void)w; free(st); st = 0; }
static const struct app_ops settings_ops = {settings_paint, settings_mouse, 0, settings_closed};

void open_display_settings(void) {
    if (st) return;
    st = calloc(1, sizeof(*st));
    if (!st) return;
    for (int i = 0; i < screen_count; ++i)
        st->p[i] = (struct pending){screens[i].r.x * ui_scale / 100, screens[i].r.y * ui_scale / 100, screens[i].phys_w, screens[i].phys_h, screens[i].mode};
    st->main = main_screen;
    st->scale = ui_scale;
    st->selected = main_screen;
    st->w = window_open(460, 328, "Display Settings", ICON_DISPLAY, &settings_ops, 0);
    if (!st->w) { free(st); st = 0; }
}

#define WP_LIST 16
#define WP_MAX 128
struct wallpapers {
    struct window *w;
    char names[WP_MAX][80], paths[WP_MAX][300];
    int count, selected;
    struct vscroll scroll;
    struct surface preview;
    int preview_ok;
};
static struct wallpapers *wp;
static const struct rect wp_list = {12, 12, 200, 268}, wp_add = {12, 292, 96, 24}, wp_apply = {440, 292, 80, 24};
#define WP_ROW 18
#define PREVIEW_X 228
#define PREVIEW_Y 12
#define PREVIEW_W 292
#define PREVIEW_H 268

static void wp_add_folder(const char *folder) {
    struct dosxc_dirent *entries = malloc(sizeof(*entries) * WP_MAX);
    if (!entries) return;
    int n = (int)dosxc_sys_listdir(folder, entries, WP_MAX);
    for (int i = 0; i < n && i < WP_MAX && wp->count < WP_MAX; ++i) {
        if (!image_is_picture(entries[i].name) || (entries[i].attributes & DOSXC_ATTR_DIRECTORY)) continue;
        snprintf(wp->paths[wp->count], sizeof(wp->paths[0]), "%s\\%s", folder, entries[i].name);
        snprintf(wp->names[wp->count], sizeof(wp->names[0]), "%s", entries[i].name);
        char *dot = strrchr(wp->names[wp->count], '.');
        if (dot) *dot = 0;
        ++wp->count;
    }
    free(entries);
}

static void user_folder(char *out, size_t size) {
    const char *home = getenv("HOME");
    snprintf(out, size, "%s\\Wallpapers", home && *home ? home : "\\Users\\Public");
}

static void wp_preview(void) {
    wp->preview_ok = 0;
    if (wp->selected <= 0 || wp->selected >= wp->count) return;
    struct surface full = {0};
    if (image_load(wp->paths[wp->selected], &full)) return;
    surface_size(&wp->preview, PREVIEW_W, PREVIEW_H);
    image_cover(&full, &wp->preview);
    free(full.px);
    wp->preview_ok = 1;
}

static void wp_scan(void) {
    wp->count = 1;
    snprintf(wp->names[0], sizeof(wp->names[0]), "(None)");
    wp->paths[0][0] = 0;
    char folder[128];
    desktop_path(folder, sizeof(folder), "Wallpapers");
    wp_add_folder(folder);
    char mine[300];
    user_folder(mine, sizeof(mine));
    wp_add_folder(mine);
    wp->scroll.total = wp->count;
    wp->scroll.visible = wp_list.h / WP_ROW;
    wp->selected = 0;
    for (int i = 0; i < wp->count; ++i)
        if (!strcasecmp(wp->paths[i], current_wallpaper())) wp->selected = i;
}

static void wp_paint(struct window *w, const struct canvas *c) {
    (void)w;
    bevel(c, wp_list.x - 2, wp_list.y - 2, wp_list.w + 4, wp_list.h + 4, 1);
    fill(c, wp_list.x, wp_list.y, wp_list.w, wp_list.h, C_BASE);
    for (int i = 0; i < wp->scroll.visible && wp->scroll.top + i < wp->count; ++i) {
        int y = wp_list.y + i * WP_ROW, at = wp->scroll.top + i, on = at == wp->selected;
        if (on) fill(c, wp_list.x, y, wp_list.w - 14, WP_ROW, C_SELECTION);
        text(c, wp_list.x + 6, y + 4, &font_regular, wp->names[at], on ? 0xffffff : C_TEXT);
    }
    vscroll_paint(c, &wp->scroll, (struct rect){wp_list.x + wp_list.w - 14, wp_list.y, 14, wp_list.h});
    bevel(c, PREVIEW_X - 2, PREVIEW_Y - 2, PREVIEW_W + 4, PREVIEW_H + 4, 1);
    if (wp->preview_ok) blit(c, PREVIEW_X, PREVIEW_Y, &wp->preview);
    else fill(c, PREVIEW_X, PREVIEW_Y, PREVIEW_W, PREVIEW_H, C_DESKTOP);
    button(c, wp_add, "Add...", 0, 0);
    button(c, wp_apply, "Apply", 0, 1);
}

static void wp_added(const char *path, void *data) {
    (void)data;
    if (!path) return;
    char folder[300], target[420];
    user_folder(folder, sizeof(folder));
    dosxc_sys_mkdir(folder);
    const char *name = path;
    for (const char *p = path; *p; ++p)
        if (*p == '\\' || *p == '/') name = p + 1;
    snprintf(target, sizeof(target), "%s\\%s", folder, name);
    FILE *in = fopen(path, "rb"), *out = in ? fopen(target, "wb") : 0;
    int ok = in && out;
    static char buffer[32768];
    size_t n;
    while (ok && (n = fread(buffer, 1, sizeof(buffer), in)) > 0)
        if (fwrite(buffer, 1, n, out) != n) ok = 0;
    if (in) fclose(in);
    if (out && fclose(out)) ok = 0;
    if (!ok) { message_box("Wallpaper", "The picture could not be copied", "to your Wallpapers folder."); return; }
    if (wp) {
        wp_scan();
        for (int i = 0; i < wp->count; ++i)
            if (!strcasecmp(wp->paths[i], target)) wp->selected = i;
        wp_preview();
        window_paint(wp->w);
    }
}

static void wp_mouse(struct window *w, int x, int y, int kind, int value) {
    struct rect bar = {wp_list.x + wp_list.w - 14, wp_list.y, 14, wp_list.h};
    if (vscroll_mouse(&wp->scroll, bar, x, y, kind, value)) { window_paint(w); return; }
    if (kind == M_WHEEL) { wp->scroll.top -= value * 2; vscroll_clamp(&wp->scroll); window_paint(w); return; }
    if (kind != M_DOWN && kind != M_DOUBLE) return;
    if (hit(wp_add, x, y)) { file_dialog("Add Wallpaper", 0, 0, wp_added, 0); return; }
    if (hit(wp_apply, x, y)) { set_wallpaper(wp->paths[wp->selected]); return; }
    if (hit(wp_list, x, y)) {
        int at = wp->scroll.top + (y - wp_list.y) / WP_ROW;
        if (at < wp->count && at != wp->selected) {
            wp->selected = at;
            wp_preview();
            window_paint(w);
        }
        if (kind == M_DOUBLE && at < wp->count) set_wallpaper(wp->paths[at]);
    }
}

static void wp_closed(struct window *w) { (void)w; free(wp->preview.px); free(wp); wp = 0; }
static const struct app_ops wp_ops = {wp_paint, wp_mouse, 0, wp_closed};

void open_wallpaper(void) {
    if (wp) return;
    wp = calloc(1, sizeof(*wp));
    if (!wp) return;
    wp->w = window_open(532, 328, "Wallpaper", ICON_PICTURE, &wp_ops, 0);
    if (!wp->w) { free(wp); wp = 0; return; }
    wp_scan();
    wp_preview();
}

static struct window *sound_w;
static int sound_dragging, sound_saved = 100;
static const struct rect sound_slider = {16, 44, 240, 18}, sound_mute = {16, 72, 110, 24}, sound_close = {176, 72, 80, 24};

static void sound_paint(struct window *w, const struct canvas *c) {
    (void)w;
    int level = sound_level();
    char line[32];
    snprintf(line, sizeof(line), level ? "Volume: %d%%" : "Volume: muted", level);
    text(c, 16, 12, &font_bold, line, C_TEXT);
    text(c, 16, 27, &font_regular, "Above 0 the PC speaker plays at full volume.", C_DISABLED);
    bevel(c, sound_slider.x, sound_slider.y + 6, sound_slider.w, 6, 1);
    struct rect knob = {sound_slider.x + (sound_slider.w - 12) * level / 100, sound_slider.y, 12, sound_slider.h};
    button(c, knob, "", 0, 0);
    button(c, sound_mute, level ? "Mute" : "Unmute", 0, 0);
    button(c, sound_close, "Close", 0, 0);
}

static void sound_set_from(int x) {
    int level = (x - sound_slider.x - 6) * 100 / (sound_slider.w - 12);
    set_sound_volume(level < 0 ? 0 : level > 100 ? 100 : level);
    window_paint(sound_w);
}

static void sound_mouse(struct window *w, int x, int y, int kind, int value) {
    (void)value;
    if (kind == M_DOWN || kind == M_DOUBLE) {
        if (hit(sound_slider, x, y)) { sound_dragging = 1; sound_set_from(x); return; }
        if (hit(sound_close, x, y)) { window_close(w); return; }
        if (hit(sound_mute, x, y)) {
            if (sound_level()) { sound_saved = sound_level(); set_sound_volume(0); }
            else set_sound_volume(sound_saved ? sound_saved : 100);
            window_paint(w);
        }
    } else if (kind == M_MOVE && sound_dragging) sound_set_from(x);
    else if (kind == M_UP) sound_dragging = 0;
}

static void sound_closed(struct window *w) { (void)w; sound_w = 0; sound_dragging = 0; }
static const struct app_ops sound_ops = {sound_paint, sound_mouse, 0, sound_closed};

void open_sound(void) {
    if (sound_w) return;
    sound_w = window_open(272, 108, "Sound", ICON_SOUND, &sound_ops, 0);
}

static struct window *accent_w;
static int accent_dragging = -1;
static const u32 accent_presets[12] = {ACCENT_DEFAULT, 0xb02020, 0xc8641e, 0xa88414, 0x2e7d32, 0x00796b,
                                       0x0e7c9c, 0x1f5fbf, 0x1a3a7a, 0x6a2ea0, 0xb83280, 0x505860};
static const struct rect accent_close = {216, 240, 80, 24};
#define SWATCH 36
#define SWATCH_GAP 12

static struct rect accent_swatch(int i) { return (struct rect){16 + i % 6 * (SWATCH + SWATCH_GAP - 1), 34 + i / 6 * (SWATCH + 6), SWATCH, SWATCH}; }
static struct rect accent_slider(int channel) { return (struct rect){36, 128 + channel * 24, 200, 18}; }

static void accent_paint(struct window *w, const struct canvas *c) {
    (void)w;
    u32 color = current_accent();
    text(c, 16, 12, &font_bold, "Choose a color:", C_TEXT);
    for (int i = 0; i < 12; ++i) {
        struct rect r = accent_swatch(i);
        bevel(c, r.x, r.y, r.w, r.h, 1);
        fill(c, r.x + 2, r.y + 2, r.w - 4, r.h - 4, accent_presets[i]);
        if (accent_presets[i] == color) {
            frame_box(c, r.x - 2, r.y - 2, r.w + 4, r.h + 4, C_TEXT);
            frame_box(c, r.x + 3, r.y + 3, r.w - 6, r.h - 6, 0xffffff);
        }
    }
    static const char *const names[3] = {"R", "G", "B"};
    for (int i = 0; i < 3; ++i) {
        struct rect r = accent_slider(i);
        int value = (int)(color >> (16 - i * 8) & 255);
        char number[8];
        snprintf(number, sizeof(number), "%d", value);
        text(c, 16, r.y + 4, &font_bold, names[i], C_TEXT);
        bevel(c, r.x, r.y + 6, r.w, 6, 1);
        struct rect knob = {r.x + (r.w - 12) * value / 255, r.y, 12, r.h};
        button(c, knob, "", 0, 0);
        text(c, r.x + r.w + 10, r.y + 4, &font_regular, number, C_TEXT);
    }
    gradient(c, 16, 204, 280, 18, C_ACTIVE1, C_ACTIVE2);
    text(c, 22, 208, &font_bold, "Preview", 0xffffff);
    button(c, accent_close, "Close", 0, 0);
}

static void accent_from_slider(int channel, int x) {
    struct rect r = accent_slider(channel);
    int value = (x - r.x - 6) * 255 / (r.w - 12);
    value = value < 0 ? 0 : value > 255 ? 255 : value;
    int shift = 16 - channel * 8;
    set_accent((current_accent() & ~(0xffu << shift)) | (u32)value << shift, 0);
    window_paint(accent_w);
}

static void accent_mouse(struct window *w, int x, int y, int kind, int value) {
    (void)value;
    if (kind == M_DOWN || kind == M_DOUBLE) {
        if (hit(accent_close, x, y)) { window_close(w); return; }
        for (int i = 0; i < 12; ++i)
            if (hit(accent_swatch(i), x, y)) { set_accent(accent_presets[i], 1); window_paint(w); return; }
        for (int i = 0; i < 3; ++i)
            if (hit(accent_slider(i), x, y)) { accent_dragging = i; accent_from_slider(i, x); return; }
    } else if (kind == M_MOVE && accent_dragging >= 0) accent_from_slider(accent_dragging, x);
    else if (kind == M_UP && accent_dragging >= 0) {
        accent_dragging = -1;
        set_accent(current_accent(), 1);
    }
}

static void accent_closed(struct window *w) {
    (void)w;
    accent_w = 0;
    if (accent_dragging >= 0) set_accent(current_accent(), 1);
    accent_dragging = -1;
}
static const struct app_ops accent_ops = {accent_paint, accent_mouse, 0, accent_closed};

void open_accent(void) {
    if (accent_w) return;
    accent_w = window_open(312, 274, "Accent Color", ICON_PAINT, &accent_ops, 0);
}
