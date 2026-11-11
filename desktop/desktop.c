/*
 * DOS-XC /desktop/desktop.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "wm.h"
#include <strings.h>

#define TITLE_H 19
#define FRAME_TOP (TITLE_H + 5)
#define FRAME_SIDE 4
#define FRAME_BOTTOM 4
#define TASKBAR_H 27
#define MAX_WINDOWS 32
#define MAX_DAMAGE 48
#define NS_PER_MS 1000000ull

struct screen screens[MAX_SCREENS];
int screen_count, main_screen;
int ui_scale = 100;

static struct window *windows[MAX_WINDOWS];       /* bottom to top */
static int window_count;
static struct window *task_order[MAX_WINDOWS];    /* in the order they opened */
static int task_count;
static struct window *active;

static struct rect damage[MAX_DAMAGE];
static int damage_count;

static int cursor_x, cursor_y;
static unsigned buttons;
static int mouse_seen;
static long long identify_until;
static struct gui_board *board;
static struct surface wall_source;
static char wall_path[300];
static int wall_loaded;
static int running = 1, exit_status;
static int clock_minute = -1;
static u32 accent = ACCENT_DEFAULT;
static int sound_volume = 100, net_state, net_shown = -1, volume_shown = -1;
static long long tray_next;
static char cfg_browser[16], cfg_editor[16];
static int accum_x, accum_y;
static void desktop_config_save(void);

enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE, DRAG_CAPTURE, DRAG_BUTTON };
enum { EDGE_L = 1, EDGE_T = 2, EDGE_R = 4, EDGE_B = 8 };
enum { HIT_NONE, HIT_CONTENT, HIT_TITLE, HIT_EDGE, HIT_CLOSE, HIT_MAX, HIT_MIN };
static struct {
    int mode, edges, grab_x, grab_y, button, hit;
    struct window *w;
    struct rect start;
} drag;

/* ---- menus -------------------------------------------------------------- */

struct menu_item { const char *label; int icon; void (*act)(void); const struct menu_item *sub; int sub_count; };
#define MENU_ROW 22
#define MENU_SEP 8
#define MENU_W 190
struct open_menu { int open, hot, count; struct rect r; const struct menu_item *items; int stripe; };
static struct open_menu menu, submenu, nested;   /* nested: a folder inside a submenu */

static void damage_rect(struct rect r) {
    if (r.w <= 0 || r.h <= 0) return;
    for (int i = 0; i < damage_count; ++i)
        if (rect_meets(damage[i], r)) {
            r = rect_either(damage[i], r);
            damage[i] = damage[--damage_count];
            i = -1;
        }
    if (damage_count == MAX_DAMAGE) {
        for (int i = 1; i < damage_count; ++i) damage[0] = rect_either(damage[0], damage[i]);
        damage_count = 1;
        damage[0] = rect_either(damage[0], r);
        return;
    }
    damage[damage_count++] = r;
}

static void damage_all(void) {
    for (int i = 0; i < screen_count; ++i) damage_rect(screens[i].r);
}

static struct rect main_work(void) {
    struct rect r = screens[main_screen].r;
    r.h -= TASKBAR_H;
    return r;
}

static int screen_at(int x, int y) {
    for (int i = 0; i < screen_count; ++i)
        if (rect_has(screens[i].r, x, y)) return i;
    return -1;
}

static struct rect work_area(int s) {
    return s == main_screen ? main_work() : screens[s].r;
}

static void window_resized(struct window *w);

static struct rect frame_of(const struct window *w) {
    if (w->popup) return w->r;
    return (struct rect){w->r.x - FRAME_SIDE, w->r.y - FRAME_TOP, w->r.w + 2 * FRAME_SIDE, w->r.h + FRAME_TOP + FRAME_BOTTOM};
}

static void place_on_screens(void) {
    for (int i = 0; i < window_count; ++i) {
        struct window *w = windows[i];
        if (w->maximized) {
            int s = screen_at(w->r.x, w->r.y);
            if (s < 0) s = main_screen;
            struct rect a = work_area(s);
            w->r = (struct rect){a.x + FRAME_SIDE, a.y + FRAME_TOP, a.w - 2 * FRAME_SIDE, a.h - FRAME_TOP - FRAME_BOTTOM};
            window_resized(w);
            continue;
        }
        struct rect f = frame_of(w);
        if (screen_at(f.x + f.w / 2, f.y + TITLE_H / 2) >= 0) continue;
        struct rect a = main_work();
        w->r.x = a.x + (a.w - w->r.w) / 2;
        w->r.y = a.y + (a.h - w->r.h) / 2;
        if (w->r.y < a.y + FRAME_TOP) w->r.y = a.y + FRAME_TOP;
        if (w->r.x < a.x + FRAME_SIDE) w->r.x = a.x + FRAME_SIDE;
    }
}

static void keep_cursor_on_screens(void) {
    if (screen_at(cursor_x, cursor_y) >= 0) return;
    struct rect r = screens[main_screen].r;
    cursor_x = r.x + r.w / 2;
    cursor_y = r.y + r.h / 2;
}

/* A device pixel covers less than one desktop pixel at 125% and 150%, so
   each comes from one source pixel, or two across a seam. */
static void scale_map(struct scale_step **map, int device, int logical) {
    free(*map);
    *map = malloc((size_t)device * sizeof(**map));
    if (!*map) return;
    for (int p = 0; p < device; ++p) {
        int start = p * 100, end = (p + 1) * 100, left = start / ui_scale;
        int boundary = (left + 1) * ui_scale, weight = 0;
        if (end > boundary && left + 1 < logical) weight = (end - boundary) * 256 / 100;
        (*map)[p] = (struct scale_step){left < logical ? left : logical - 1, weight};
    }
}

static void update_screen_sizes(void) {
    for (int i = 0; i < screen_count; ++i) {
        struct screen *s = &screens[i];
        s->r.w = s->phys_w * 100 / ui_scale;
        s->r.h = s->phys_h * 100 / ui_scale;
        surface_size(&s->back, s->r.w, s->r.h);
        scale_map(&s->xmap, s->phys_w, s->r.w);
        scale_map(&s->ymap, s->phys_h, s->r.h);
        if (wall_loaded) {
            surface_size(&s->wall, s->r.w, s->r.h);
            image_cover(&wall_source, &s->wall);
        }
    }
}

static void apply_modes(void) {
    for (int i = 0; i < screen_count; ++i) {
        struct screen *s = &screens[i];
        struct dosxc_display_mode wanted = s->modes[s->mode];
        struct dosxc_display d;
        dosxc_sys_display(DOSXC_DISPLAY_SET, i, &wanted, 0);
        dosxc_sys_display(DOSXC_DISPLAY_INFO, i, &d, 0);
        s->framebuffer = (unsigned char *)(uintptr_t)d.framebuffer;
        s->pitch = (int)d.pitch;
        s->phys_w = (int)wanted.width;
        s->phys_h = (int)wanted.height;
    }
    update_screen_sizes();
}

/* Display Settings has already placed the monitors for the new scale. */
void set_ui_scale(int percent) {
    if (percent == ui_scale) return;
    ui_scale = percent;
    desktop_config_save();
}

void layout_changed(void) {
    apply_modes();
    place_on_screens();
    keep_cursor_on_screens();
    icons_layout();
    damage_all();
}

void identify_screens(void) {
    identify_until = (long long)dosxc_sys_nanoseconds() + 2000 * (long long)NS_PER_MS;
    damage_all();
}

/* The saved arrangement: "%HOME%\DISPLAYS.CFG", one display per line. */
static void config_path(char *out, size_t size) {
    const char *home = getenv("HOME");
    snprintf(out, size, "%s\\DISPLAYS.CFG", home && *home ? home : "\\Users\\Public");
}

void save_layout(void) {
    char path[300];
    config_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%d %d\n", screen_count, main_screen);
    for (int i = 0; i < screen_count; ++i)
        fprintf(f, "%d %d %d %d\n", screens[i].r.x, screens[i].r.y, screens[i].phys_w, screens[i].phys_h);
    fclose(f);
}

static int mode_index(const struct screen *s, int w, int h) {
    for (int i = 0; i < s->mode_count; ++i)
        if ((int)s->modes[i].width == w && (int)s->modes[i].height == h) return i;
    return -1;
}

static int load_layout(void) {
    char path[300];
    config_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int count = 0, wanted_main = 0, ok = fscanf(f, "%d %d", &count, &wanted_main) == 2 && count == screen_count;
    for (int i = 0; ok && i < screen_count; ++i) {
        int x, y, w, h;
        ok = fscanf(f, "%d %d %d %d", &x, &y, &w, &h) == 4;
        int m = ok ? mode_index(&screens[i], w, h) : -1;
        if (m < 0) { ok = 0; break; }
        screens[i].r.x = x;
        screens[i].r.y = y;
        screens[i].mode = m;
    }
    fclose(f);
    if (!ok) return 0;
    main_screen = wanted_main >= 0 && wanted_main < screen_count ? wanted_main : 0;
    return 1;
}

static void default_layout(void) {
    int x = 0;
    for (int i = 0; i < screen_count; ++i) {
        struct screen *s = &screens[i];
        int m = mode_index(s, s->preferred_width, s->preferred_height);
        if (m < 0) m = mode_index(s, 1024, 768);
        if (m < 0) m = s->mode_count - 1;
        s->mode = m;
        s->r = (struct rect){x, 0, (int)s->modes[m].width * 100 / ui_scale, (int)s->modes[m].height * 100 / ui_scale};
        x += s->r.w;
    }
    main_screen = 0;
}

static int screens_init(void) {
    int count = dosxc_sys_display(DOSXC_DISPLAY_INFO, 0, 0, 0);
    if (count <= 0) return -1;
    if (count > MAX_SCREENS) count = MAX_SCREENS;
    screen_count = count;
    for (int i = 0; i < count; ++i) {
        struct screen *s = &screens[i];
        struct dosxc_display d;
        dosxc_sys_display(DOSXC_DISPLAY_INFO, i, &d, 0);
        memcpy(s->name, d.name, sizeof(s->name));
        s->preferred_width = (int)d.preferred_width;
        s->preferred_height = (int)d.preferred_height;
        s->mode_count = dosxc_sys_display(DOSXC_DISPLAY_MODES, i, s->modes, MAX_MODES);
        if (s->mode_count > MAX_MODES) s->mode_count = MAX_MODES;
        if (s->mode_count <= 0) return -1;
    }
    if (!load_layout()) default_layout();
    return 0;
}

/* ---- windows ------------------------------------------------------------ */

static void damage_window(const struct window *w) { damage_rect(frame_of(w)); }

static void activate(struct window *w) {
    if (active == w) {
        if (!w) return;
    } else {
        if (active) damage_window(active);
        active = w;
        if (w) damage_window(w);
        damage_rect(main_work());
        struct rect bar = screens[main_screen].r;
        bar.y += bar.h - TASKBAR_H;
        bar.h = TASKBAR_H;
        damage_rect(bar);
    }
    if (!w) return;
    for (int i = 0; i < window_count; ++i)
        if (windows[i] == w) {
            memmove(windows + i, windows + i + 1, (size_t)(window_count - i - 1) * sizeof(*windows));
            windows[window_count - 1] = w;
            damage_window(w);
            break;
        }
}

static void activate_top(void) {
    struct window *found = 0;
    for (int i = window_count - 1; i >= 0 && !found; --i)
        if (!windows[i]->minimized) found = windows[i];
    activate(found);
}

static struct window *window_create(int w, int h, const char *title, int icon, const struct app_ops *ops, void *data, int remote) {
    if (window_count == MAX_WINDOWS) return 0;
    struct window *win = calloc(1, sizeof(*win));
    if (!win) return 0;
    struct rect a = main_work();
    static int cascade;
    int step = 24 * cascade;
    cascade = (cascade + 1) % 6;
    win->r = (struct rect){a.x + (a.w - w) / 2 + step - 40, a.y + (a.h - h) / 2 + step - 40, w, h};
    if (win->r.y < a.y + FRAME_TOP) win->r.y = a.y + FRAME_TOP;
    if (win->r.x < a.x + FRAME_SIDE) win->r.x = a.x + FRAME_SIDE;
    snprintf(win->title, sizeof(win->title), "%s", title);
    win->icon = icon;
    win->ops = ops;
    win->data = data;
    win->needs_paint = !remote;
    if (!remote && surface_size(&win->content, w, h)) { free(win); return 0; }
    windows[window_count++] = win;
    task_order[task_count++] = win;
    activate(win);
    return win;
}

struct window *window_open(int w, int h, const char *title, int icon, const struct app_ops *ops, void *data) {
    return window_create(w, h, title, icon, ops, data, 0);
}

void window_modal(struct window *w) { (void)w; }
void window_popup(struct window *w, int x, int y, const char *items, int tag) { (void)w; (void)x; (void)y; (void)items; (void)tag; }

void window_close(struct window *w) {
    for (int i = 0; i < window_count; ++i)
        if (windows[i] == w) {
            memmove(windows + i, windows + i + 1, (size_t)(window_count - i - 1) * sizeof(*windows));
            --window_count;
            break;
        }
    for (int i = 0; i < task_count; ++i)
        if (task_order[i] == w) {
            memmove(task_order + i, task_order + i + 1, (size_t)(task_count - i - 1) * sizeof(*task_order));
            --task_count;
            break;
        }
    damage_window(w);
    if (drag.w == w) drag.mode = DRAG_NONE;
    if (w->ops->closed) w->ops->closed(w);
    if (active == w) { active = 0; activate_top(); }
    struct rect bar = screens[main_screen].r;
    bar.y += bar.h - TASKBAR_H;
    bar.h = TASKBAR_H;
    damage_rect(bar);
    if (!w->remote) free(w->content.px);
    free(w);
}

void window_title(struct window *w, const char *title) {
    snprintf(w->title, sizeof(w->title), "%s", title);
    damage_window(w);
    struct rect bar = screens[main_screen].r;
    bar.y += bar.h - TASKBAR_H;
    bar.h = TASKBAR_H;
    damage_rect(bar);
}

/* A client's window: tell it, and draw what it publishes. */
static void post_event(struct window *w, int type, int x, int y, int kind, int value) {
    struct gui_client *c = &board->clients[w->client];
    u32 next = (c->ev_write + 1) % GUI_RING;
    if (next == c->ev_read) return;
    c->events[c->ev_write] = (struct gui_event){type, x, y, kind, value, 0, (uintptr_t)w->remote};
    c->ev_write = next;
}

static void window_resized(struct window *w) {
    if (w->remote) { post_event(w, EV_RESIZE, w->r.w, w->r.h, 0, 0); return; }
    surface_size(&w->content, w->r.w, w->r.h);
    window_paint(w);
}

static void window_maximize(struct window *w) {
    struct rect old = frame_of(w);
    if (w->maximized) {
        w->r = w->restore;
        w->maximized = 0;
    } else {
        int s = screen_at(w->r.x + w->r.w / 2, w->r.y + w->r.h / 2);
        if (s < 0) s = main_screen;
        struct rect a = work_area(s);
        w->restore = w->r;
        w->r = (struct rect){a.x + FRAME_SIDE, a.y + FRAME_TOP, a.w - 2 * FRAME_SIDE, a.h - FRAME_TOP - FRAME_BOTTOM};
        w->maximized = 1;
    }
    window_resized(w);
    damage_rect(old);
    damage_window(w);
}

static void window_minimize(struct window *w) {
    w->minimized = 1;
    damage_window(w);
    if (active == w) { active = 0; activate_top(); }
    struct rect bar = screens[main_screen].r;
    bar.y += bar.h - TASKBAR_H;
    bar.h = TASKBAR_H;
    damage_rect(bar);
}

static struct rect title_rect(struct rect f) { return (struct rect){f.x + 4, f.y + 4, f.w - 8, TITLE_H}; }

static struct rect title_button(struct rect f, int which) {
    struct rect t = title_rect(f);
    return (struct rect){t.x + t.w - 16 * which - 1, t.y + 2, 15, 15};
}

static int window_hit(const struct window *w, int x, int y, int *edges) {
    struct rect f = frame_of(w);
    if (!rect_has(f, x, y)) return HIT_NONE;
    if (rect_has(w->r, x, y)) return HIT_CONTENT;
    if (rect_has(title_button(f, 1), x, y)) return HIT_CLOSE;
    if (w->resizable && rect_has(title_button(f, 2), x, y)) return HIT_MAX;
    if (rect_has(title_button(f, w->resizable ? 3 : 2), x, y)) return HIT_MIN;
    if (rect_has(title_rect(f), x, y)) return HIT_TITLE;
    *edges = 0;
    if (w->resizable && !w->maximized) {
        if (x < f.x + FRAME_SIDE) *edges |= EDGE_L;
        if (x >= f.x + f.w - FRAME_SIDE) *edges |= EDGE_R;
        if (y < f.y + FRAME_SIDE) *edges |= EDGE_T;
        if (y >= f.y + f.h - FRAME_SIDE) *edges |= EDGE_B;
    }
    return *edges ? HIT_EDGE : HIT_TITLE;
}

/* ---- painting ----------------------------------------------------------- */

static void paint_title_glyph(const struct canvas *c, struct rect b, int kind) {
    int x = b.x + 4, y = b.y + 4;
    switch (kind) {
    case 0:
        for (int i = 0; i < 7; ++i) {
            fill(c, x + i, y + i, 2, 1, C_TEXT);
            fill(c, x + 6 - i, y + i, 2, 1, C_TEXT);
        }
        break;
    case 1:
        frame_box(c, x, y, 8, 7, C_TEXT);
        fill(c, x, y + 1, 8, 1, C_TEXT);
        break;
    default:
        fill(c, x, y + 5, 6, 2, C_TEXT);
        break;
    }
}

static void paint_frame(const struct canvas *c, struct window *w) {
    if (w->popup) return;
    struct rect f = frame_of(w);
    int focused = w == active;
    int moving = focused && drag.mode == DRAG_MOVE && drag.w == w;
    u32 left = moving ? C_MOVING1 : focused ? C_ACTIVE1 : C_INACTIVE1;
    u32 right = moving ? C_MOVING2 : focused ? C_ACTIVE2 : C_INACTIVE2;
    u32 stripes = moving ? C_MOVING1 : focused ? C_ACTIVE1 : C_INACTIVE1;
    u32 shadow = moving ? C_MOVING_SHADOW : focused ? C_ACTIVE_SHADOW : C_INACTIVE_SHADOW;
    u32 label = focused ? 0xffffff : C_INACTIVE_TEXT;

    fill(c, f.x, f.y, f.w, f.h, C_WINDOW);
    fill(c, f.x, f.y, f.w, 1, C_WINDOW);
    fill(c, f.x, f.y, 1, f.h, C_WINDOW);
    fill(c, f.x + 1, f.y + 1, f.w - 2, 1, C_HIGHLIGHT);
    fill(c, f.x + 1, f.y + 1, 1, f.h - 2, C_HIGHLIGHT);
    fill(c, f.x, f.y + f.h - 1, f.w, 1, C_SHADOW2);
    fill(c, f.x + f.w - 1, f.y, 1, f.h, C_SHADOW2);
    fill(c, f.x + 1, f.y + f.h - 2, f.w - 2, 1, C_SHADOW1);
    fill(c, f.x + f.w - 2, f.y + 1, 1, f.h - 2, C_SHADOW1);

    struct rect t = title_rect(f);
    gradient(c, t.x, t.y, t.w, t.h, left, right);
    draw_icon(c, t.x + 2, t.y + 1, w->icon);
    int tx = t.x + 22, ty = t.y + (t.h - font_bold.height) / 2;
    text(c, tx + 1, ty + 1, &font_bold, w->title, shadow);
    text(c, tx, ty, &font_bold, w->title, label);

    int buttons_left = title_button(f, w->resizable ? 3 : 2).x;
    int stripes_from = tx + text_width(&font_bold, w->title) + 6;
    if (buttons_left - 4 > stripes_from)
        for (int y = t.y + 3; y < t.y + t.h - 3; y += 2)
            fill(c, stripes_from, y, buttons_left - 4 - stripes_from, 1, stripes);

    struct rect close = title_button(f, 1);
    button(c, close, "", 0, 0);
    paint_title_glyph(c, close, 0);
    if (w->resizable) {
        struct rect max = title_button(f, 2);
        button(c, max, "", 0, 0);
        paint_title_glyph(c, max, 1);
    }
    struct rect min = title_button(f, w->resizable ? 3 : 2);
    button(c, min, "", 0, 0);
    paint_title_glyph(c, min, 2);
}

static struct rect taskbar_rect(void) {
    struct rect r = screens[main_screen].r;
    r.y += r.h - TASKBAR_H;
    r.h = TASKBAR_H;
    return r;
}

#define START_W 78
#define TASK_W 150
#define NET_W 22
#define VOLUME_W 54

static struct rect start_button_rect(void) {
    struct rect b = taskbar_rect();
    return (struct rect){b.x + 3, b.y + 3, START_W, b.h - 5};
}

static struct rect clock_rect(void) {
    struct rect b = taskbar_rect();
    int w = text_width(&font_regular, "00/00/0000  00:00") + 12;
    return (struct rect){b.x + b.w - w - 3, b.y + 3, w, b.h - 5};
}

static struct rect net_rect(void) {
    struct rect c = clock_rect();
    return (struct rect){c.x - NET_W - 2, c.y, NET_W, c.h};
}

static struct rect volume_rect(void) {
    struct rect n = net_rect();
    return (struct rect){n.x - VOLUME_W - 2, n.y, VOLUME_W, n.h};
}

static struct rect task_button_rect(int n) {
    struct rect b = taskbar_rect();
    int room = volume_rect().x - 3 - (b.x + START_W + 8);
    int width = task_count && room / task_count < TASK_W ? room / task_count : TASK_W;
    return (struct rect){b.x + START_W + 8 + n * width, b.y + 3, width - 2, b.h - 5};
}

/* The date and time for the taskbar. */
static void clock_text(char *out, size_t size) {
    long long now = dosxc_sys_time();
    long long days = now / 86400 + 719468;
    long long era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int year = (int)((long long)yoe + era * 400), day = (int)(doy - (153 * mp + 2) / 5 + 1), month = (int)(mp < 10 ? mp + 3 : mp - 9);
    if (month <= 2) ++year;
    int minutes = (int)((now % 86400 + 86400) % 86400) / 60;
    snprintf(out, size, "%02d/%02d/%04d  %d:%02d", month, day, year, minutes / 60, minutes % 60);
}

static void paint_clipped_label(const struct canvas *c, struct rect r, const struct font *f, const char *s, u32 color) {
    struct canvas inner = *c;
    inner.clip = rect_both(c->clip, r);
    text(&inner, r.x, r.y + (r.h - f->height) / 2, f, s, color);
}

static void paint_taskbar(const struct canvas *c) {
    struct rect b = taskbar_rect();
    if (!rect_meets(b, c->clip)) return;
    fill(c, b.x, b.y, b.w, b.h, C_WINDOW);
    fill(c, b.x, b.y, b.w, 1, C_HIGHLIGHT);

    struct rect start = start_button_rect();
    int pressed = menu.open && menu.items != 0 && menu.stripe;
    button(c, start, "", pressed, 0);
    draw_icon(c, start.x + 4 + pressed, start.y + 3 + pressed, ICON_LOGO);
    text(c, start.x + 24 + pressed, start.y + (start.h - font_bold.height) / 2 + pressed, &font_bold, "DOS-XC", C_TEXT);

    for (int i = 0; i < task_count; ++i) {
        struct window *w = task_order[i];
        struct rect t = task_button_rect(i);
        int down = w == active && !w->minimized;
        fill(c, t.x, t.y, t.w, t.h, down ? C_HOVER : C_WINDOW);
        bevel(c, t.x, t.y, t.w, t.h, down);
        draw_icon(c, t.x + 4 + down, t.y + 3 + down, w->icon);
        struct rect label = {t.x + 24 + down, t.y + down, t.w - 28, t.h};
        paint_clipped_label(c, label, down ? &font_bold : &font_regular, w->title, C_TEXT);
    }

    struct rect volume = volume_rect();
    bevel(c, volume.x, volume.y, volume.w, volume.h, 1);
    fill(c, volume.x + 2, volume.y + 2, volume.w - 4, volume.h - 4, C_WINDOW);
    draw_icon(c, volume.x + 3, volume.y + (volume.h - ICON_SIZE) / 2, ICON_SOUND);
    char level[8];
    snprintf(level, sizeof(level), sound_volume ? "%d%%" : "Mute", sound_volume);
    text(c, volume.x + 21, volume.y + (volume.h - font_regular.height) / 2, &font_regular, level, C_TEXT);

    struct rect net = net_rect();
    bevel(c, net.x, net.y, net.w, net.h, 1);
    fill(c, net.x + 2, net.y + 2, net.w - 4, net.h - 4, C_WINDOW);
    draw_icon(c, net.x + (net.w - ICON_SIZE) / 2, net.y + (net.h - ICON_SIZE) / 2, net_state ? ICON_NET : ICON_NET_OFF);

    struct rect clk = clock_rect();
    bevel(c, clk.x, clk.y, clk.w, clk.h, 1);
    fill(c, clk.x + 2, clk.y + 2, clk.w - 4, clk.h - 4, C_WINDOW);
    char line[32];
    clock_text(line, sizeof(line));
    text(c, clk.x + (clk.w - text_width(&font_regular, line)) / 2, clk.y + (clk.h - font_regular.height) / 2, &font_regular, line, C_TEXT);
}

#define MENU_HEADER 52
#define ICON_HEADER (-2)
static int item_height(const struct menu_item *m) { return m->icon == ICON_HEADER ? MENU_HEADER : m->label ? MENU_ROW : MENU_SEP; }

static struct dosxc_user me;
static int accounts_on;
static struct surface me_picture;

static void paint_header(const struct canvas *c, struct rect r, int y, const char *label) {
    gradient(c, r.x + 2, y, r.w - 4, MENU_HEADER, C_ACTIVE1, C_ACTIVE2);
    if (me_picture.px) {
        fill(c, r.x + 7, y + 5, me_picture.w + 2, me_picture.h + 2, 0xffffff);
        blit(c, r.x + 8, y + 6, &me_picture);
    } else {
        draw_icon_big(c, r.x + 8, y + 10, &icons[ICON_COMPUTER]);
    }
    text(c, r.x + 56, y + (MENU_HEADER - font_bold.height) / 2, &font_bold, label, 0xffffff);
}

static void paint_one_menu(const struct canvas *c, const struct open_menu *o) {
    if (!o->open || !rect_meets(o->r, c->clip)) return;
    struct rect r = o->r;
    fill(c, r.x, r.y, r.w, r.h, C_BASE);
    fill(c, r.x + 2, r.y + 2, 24, r.h - 4, C_MENU_STRIPE);
    bevel(c, r.x, r.y, r.w, r.h, 0);
    int y = r.y + 2;
    for (int i = 0; i < o->count; ++i) {
        const struct menu_item *m = &o->items[i];
        int h = item_height(m);
        if (m->icon == ICON_HEADER) {
            paint_header(c, r, y, m->label);
        } else if (!m->label) {
            fill(c, r.x + 28, y + h / 2 - 1, r.w - 32, 1, C_SHADOW1);
            fill(c, r.x + 28, y + h / 2, r.w - 32, 1, C_HIGHLIGHT);
        } else {
            if (i == o->hot) fill(c, r.x + 2, y, r.w - 4, h, C_MENU_SELECTION);
            if (m->icon >= 0) draw_icon(c, r.x + 6, y + 3, m->icon);
            text(c, r.x + 32, y + (h - font_regular.height) / 2, &font_regular, m->label, C_TEXT);
            if (m->sub)
                for (int k = 0; k < 4; ++k) fill(c, r.x + r.w - 12 + k, y + 7 + k, 1, 7 - 2 * k, C_TEXT);
        }
        y += h;
    }
}

static void paint_menu(const struct canvas *c) {
    paint_one_menu(c, &menu);
    paint_one_menu(c, &submenu);
    paint_one_menu(c, &nested);
}

static const u32 C_ARROW_DARK = 0xff000000, C_ARROW_LIGHT = 0xffffffff;
static const char cursor_shape[] =
    "K..........."
    "KK.........."
    "KWK........."
    "KWWK........"
    "KWWWK......."
    "KWWWWK......"
    "KWWWWWK....."
    "KWWWWWWK...."
    "KWWWWWWWK..."
    "KWWWWWWWWK.."
    "KWWWWWKKKKK."
    "KWWWKWWK...."
    "KWWKKWWK...."
    "KWK..KWWK..."
    "KK...KWWK..."
    "K.....KWWK.."
    "......KWWK.."
    ".......KK...";
#define CURSOR_W 12
#define CURSOR_H 18

static void paint_cursor(const struct canvas *c) {
    struct rect cr = {cursor_x, cursor_y, CURSOR_W, CURSOR_H};
    if (!rect_meets(cr, c->clip)) return;
    u32 px[CURSOR_W * CURSOR_H];
    for (int i = 0; i < CURSOR_W * CURSOR_H; ++i)
        px[i] = cursor_shape[i] == 'K' ? C_ARROW_DARK : cursor_shape[i] == 'W' ? C_ARROW_LIGHT : 0;
    draw_pixels(c, cursor_x, cursor_y, CURSOR_W, CURSOR_H, px);
}

static void paint_identify(const struct canvas *c, int s) {
    if ((long long)dosxc_sys_nanoseconds() >= identify_until) return;
    struct rect r = screens[s].r;
    int bw = 180, bh = 150;
    int bx = r.x + (r.w - bw) / 2, by = r.y + (r.h - bh) / 2;
    fill(c, bx, by, bw, bh, C_SELECTION);
    bevel(c, bx, by, bw, bh, 0);
    char label[8];
    snprintf(label, sizeof(label), "%d", s + 1);
    text_big(c, bx + bw / 2 - 30, by + 20, label, 0xffffff, 10);
}

static void paint_scene(const struct canvas *c, int s) {
    if (logon_mode) logon_paint(c, screens[s].r);
    else if (screens[s].wall.px) blit(c, screens[s].r.x, screens[s].r.y, &screens[s].wall);
    else fill(c, c->clip.x, c->clip.y, c->clip.w, c->clip.h, C_DESKTOP);
    if (s == main_screen && !logon_mode) icons_paint(c);
    for (int i = 0; i < window_count; ++i) {
        struct window *w = windows[i];
        if (w->minimized || !rect_meets(frame_of(w), c->clip)) continue;
        paint_frame(c, w);
        struct canvas inner = *c;
        inner.clip = rect_both(c->clip, w->r);
        if (w->content.px) blit(&inner, w->r.x, w->r.y, &w->content);
    }
    if (s == main_screen && !logon_mode) paint_taskbar(c);
    paint_menu(c);
    paint_identify(c, s);
    paint_cursor(c);
}

static inline u32 blend(u32 a, u32 b, unsigned w) {
    unsigned r = 256 - w;
    return (((a & 0xff00ff) * r + (b & 0xff00ff) * w) >> 8 & 0xff00ff) | (((a & 0xff00) * r + (b & 0xff00) * w) >> 8 & 0xff00);
}

/* Copy desktop pixels to the framebuffer.  A pixel that falls on a seam
   between two desktop pixels blends them; the rest are copied as they are,
   which keeps the fonts sharp. */
static void present(const struct screen *s, struct rect r) {
    struct rect area = rect_both(r, s->r);
    if (ui_scale == 100) {
        for (int y = 0; y < area.h; ++y) {
            int by = area.y - s->r.y + y, bx = area.x - s->r.x;
            memcpy(s->framebuffer + by * s->pitch + bx * 4, s->back.px + by * s->back.w + bx, (size_t)area.w * 4);
        }
        return;
    }
    if (!area.w || !area.h || !s->xmap || !s->ymap) return;
    int lx0 = area.x - s->r.x - 1, ly0 = area.y - s->r.y - 1, lx1 = area.x - s->r.x + area.w + 1, ly1 = area.y - s->r.y + area.h + 1;
    int px0 = lx0 < 0 ? 0 : lx0 * ui_scale / 100, py0 = ly0 < 0 ? 0 : ly0 * ui_scale / 100;
    int px1 = (lx1 * ui_scale + 99) / 100, py1 = (ly1 * ui_scale + 99) / 100;
    if (px1 > s->phys_w) px1 = s->phys_w;
    if (py1 > s->phys_h) py1 = s->phys_h;
    for (int py = py0; py < py1; ++py) {
        const struct scale_step *ys = &s->ymap[py];
        const u32 *row0 = s->back.px + ys->src * s->back.w;
        const u32 *row1 = ys->weight ? row0 + s->back.w : row0;
        u32 *out = (u32 *)(s->framebuffer + py * s->pitch) + px0;
        for (int px = px0; px < px1; ++px) {
            const struct scale_step *xs = &s->xmap[px];
            u32 top = xs->weight ? blend(row0[xs->src], row0[xs->src + 1], (unsigned)xs->weight) : row0[xs->src];
            if (!ys->weight) { *out++ = top; continue; }
            u32 bottom = xs->weight ? blend(row1[xs->src], row1[xs->src + 1], (unsigned)xs->weight) : row1[xs->src];
            *out++ = blend(top, bottom, (unsigned)ys->weight);
        }
    }
}

static void paint_windows(void) {
    for (int i = 0; i < window_count; ++i) {
        struct window *w = windows[i];
        if (!w->needs_paint || w->remote) continue;
        w->needs_paint = 0;
        struct canvas c = {&w->content, 0, 0, {0, 0, w->content.w, w->content.h}};
        fill(&c, 0, 0, w->content.w, w->content.h, C_WINDOW);
        w->ops->paint(w, &c);
        damage_window(w);
    }
}

static void composite(void) {
    paint_windows();
    for (int d = 0; d < damage_count; ++d)
        for (int s = 0; s < screen_count; ++s) {
            struct rect area = rect_both(damage[d], screens[s].r);
            if (!area.w || !area.h) continue;
            struct canvas c = {&screens[s].back, -screens[s].r.x, -screens[s].r.y, area};
            paint_scene(&c, s);
            present(&screens[s], area);
        }
    damage_count = 0;
}

static int nested_index = -1;

static void nested_close(void) {
    nested_index = -1;
    if (!nested.open) return;
    nested.open = 0;
    damage_rect(nested.r);
}

static void submenu_close(void) {
    nested_close();
    if (!submenu.open) return;
    submenu.open = 0;
    damage_rect(submenu.r);
}

static void menu_close(void) {
    submenu_close();
    if (!menu.open) return;
    menu.open = 0;
    damage_rect(menu.r);
    damage_rect(taskbar_rect());
}

static int menu_height(const struct menu_item *items, int count) {
    int height = 4;
    for (int i = 0; i < count; ++i) height += item_height(&items[i]);
    return height;
}

static void menu_place(struct open_menu *o, int x, int y, const struct menu_item *items, int count, int stripe) {
    int height = menu_height(items, count), s = screen_at(x, y);
    struct rect a = s < 0 ? screens[main_screen].r : screens[s].r;
    if (y + height > a.y + a.h) y = a.y + a.h - height;
    if (x + MENU_W > a.x + a.w) x = a.x + a.w - MENU_W;
    o->items = items;
    o->count = count;
    o->stripe = stripe;
    o->r = (struct rect){x, y, MENU_W, height};
    o->hot = -1;
    o->open = 1;
    damage_rect(o->r);
}

static void menu_open_at(int x, int y, const struct menu_item *items, int count, int start_menu) {
    menu_close();
    menu_place(&menu, x, y, items, count, start_menu);
    damage_rect(taskbar_rect());
}

static int menu_item_at(const struct open_menu *o, int x, int y) {
    if (!o->open || !rect_has(o->r, x, y)) return -1;
    int at = o->r.y + 2;
    for (int i = 0; i < o->count; ++i) {
        int h = item_height(&o->items[i]);
        if (y >= at && y < at + h) return o->items[i].label && o->items[i].icon != ICON_HEADER ? i : -1;
        at += h;
    }
    return -1;
}

static int submenu_index, submenu_pending = -2;
static long long submenu_since;

static void submenu_open(int index) {
    submenu_index = index;
    int y = menu.r.y + 2;
    for (int i = 0; i < index; ++i) y += item_height(&menu.items[i]);
    const struct menu_item *m = &menu.items[index];
    submenu_close();
    menu_place(&submenu, menu.r.x + menu.r.w - 3, y - 2, m->sub, m->sub_count, 0);
    if (submenu.r.x < menu.r.x + menu.r.w - 3) {
        submenu.r.x = menu.r.x - MENU_W + 3;
        damage_rect(submenu.r);
    }
}

static void nested_open(int index) {
    nested_close();
    nested_index = index;
    int y = submenu.r.y + 2;
    for (int i = 0; i < index; ++i) y += item_height(&submenu.items[i]);
    const struct menu_item *m = &submenu.items[index];
    menu_place(&nested, submenu.r.x + submenu.r.w - 3, y - 2, m->sub, m->sub_count, 0);
    if (nested.r.x < submenu.r.x + submenu.r.w - 3) {
        nested.r.x = submenu.r.x - MENU_W + 3;
        damage_rect(nested.r);
    }
}

static void act_shutdown(void);
static void act_restart(void);
static void act_exit(void) { running = 0; }
void desktop_quit(int status) { exit_status = status; running = 0; }
int logon_mode;

/* A program in the package's Apps folder, as its own task. */
static void launch(const char *name) {
    char path[128], relative[64], argv0[32];
    snprintf(relative, sizeof(relative), "Apps\\%s", name);
    desktop_path(path, sizeof(path), relative);
    snprintf(argv0, sizeof(argv0), "%s", name);
    char *argv[2] = {argv0, 0};
    long pid = dosxc_sys_task(DOSXC_TASK_START, path, (long)argv, 0);
    if (pid < 0) message_box("Cannot start", name, "The program could not be started.");
}
#define LAUNCHER(fn, file) static void fn(void) { launch(file); }
LAUNCHER(act_explorer, "EXPLORER.EXE")
LAUNCHER(act_editor, "EDITOR.EXE")
LAUNCHER(act_calc, "CALC.EXE")
LAUNCHER(act_paint, "PAINT.EXE")
LAUNCHER(act_tasks, "TASKMGR.EXE")
LAUNCHER(act_events, "EVENTS.EXE")
LAUNCHER(act_firewall, "FIREWALL.EXE")
LAUNCHER(act_fonts, "FONTS.EXE")
LAUNCHER(act_solitaire, "SOLITAIRE.EXE")
LAUNCHER(act_mines, "MINES.EXE")
LAUNCHER(act_chess, "CHESS.EXE")
LAUNCHER(act_prompt, "TERMINAL.EXE")
LAUNCHER(act_packages, "PKGMGR.EXE")
LAUNCHER(act_setup, "SETUPGUI.EXE")

static void act_browser(void) {
    char name[32];
    default_program("browser", name, sizeof(name));
    if (!name[0] || program_start(name, 0)) message_box("Web Browser", "The web browser could not be started.", "Choose one in Control Panel, Default Programs.");
}

static void act_text_editor(void) {
    char name[32];
    default_program("editor", name, sizeof(name));
    if (program_start(name, 0)) message_box("Text Editor", "The text editor could not be started.", "Choose one in Control Panel, Default Programs.");
}

static void confirmed_restart(int ok, void *data) { (void)data; if (ok) act_restart(); }
static void confirmed_shutdown(int ok, void *data) { (void)data; if (ok) act_shutdown(); }
static void ask_restart(void) { confirm_box("Restart", "Restart the computer now?", "Restart", ICON_RESTART, confirmed_restart, 0); }
static void ask_shutdown(void) { confirm_box("Shut Down", "Shut down the computer now?", "Shut Down", ICON_POWER, confirmed_shutdown, 0); }

#define COUNT_OF(a) ((int)(sizeof(a) / sizeof(*(a))))
#define ADMIN_FOLDER "System/Administrators"
/* Programs that need an administrator: System, Administrators (packages add theirs with category=System/Administrators). */
static const struct menu_item admin_items[] = {
    {"Package Manager", ICON_PACKAGE, act_packages},
    {"Event Logs", ICON_INFO, act_events},
    {"Firewall", ICON_NET, act_firewall},
};
static const struct menu_item system_items[] = {
    {"File Explorer", ICON_FOLDER, act_explorer},
    {"Task Manager", ICON_TASKS, act_tasks},
    {"DOS Prompt", ICON_PROMPT, act_prompt},
    {"Fonts", ICON_FILE, act_fonts},
    {"Install DOS-XC", ICON_SETUP, act_setup},             /* live system only: keep last */
};
static const struct menu_item utility_items[] = {
    {"File Editor", ICON_EDITOR, act_editor},
    {"Calculator", ICON_CALC, act_calc},
    {"Paint", ICON_PAINT, act_paint},
};
static const struct menu_item game_items[] = {
    {"Solitaire", ICON_CARDS, act_solitaire},
    {"Minesweeper", ICON_MINES, act_mines},
    {"Chess", ICON_CHESS, act_chess},
};
static int system_count = COUNT_OF(system_items);
static struct menu_item start_items[32];
static char browser_label[64], editor_label[64];

static void act_logout(void) { running = 0; exit_status = 3; }

#define MAX_CATEGORIES 8
#define MAX_CATEGORY_ITEMS 24
static struct menu_item category_items[MAX_CATEGORIES][MAX_CATEGORY_ITEMS];
static char category_names[MAX_CATEGORIES][32];
static int category_counts[MAX_CATEGORIES], category_total;
static struct app_entry menu_apps[32];
static int menu_app_count;

static void launch_menu_app(int i) {
    if (i < menu_app_count && app_start(&menu_apps[i], 0))
        message_box(menu_apps[i].name, "The program could not be started.", "Reinstall its package.");
}
#define APP_THUNK(n) static void act_app##n(void) { launch_menu_app(n); }
APP_THUNK(0) APP_THUNK(1) APP_THUNK(2) APP_THUNK(3) APP_THUNK(4) APP_THUNK(5) APP_THUNK(6) APP_THUNK(7)
APP_THUNK(8) APP_THUNK(9) APP_THUNK(10) APP_THUNK(11) APP_THUNK(12) APP_THUNK(13) APP_THUNK(14) APP_THUNK(15)
APP_THUNK(16) APP_THUNK(17) APP_THUNK(18) APP_THUNK(19) APP_THUNK(20) APP_THUNK(21) APP_THUNK(22) APP_THUNK(23)
APP_THUNK(24) APP_THUNK(25) APP_THUNK(26) APP_THUNK(27) APP_THUNK(28) APP_THUNK(29) APP_THUNK(30) APP_THUNK(31)
static void (*const app_thunks[32])(void) = {
    act_app0, act_app1, act_app2, act_app3, act_app4, act_app5, act_app6, act_app7, act_app8, act_app9, act_app10,
    act_app11, act_app12, act_app13, act_app14, act_app15, act_app16, act_app17, act_app18, act_app19, act_app20,
    act_app21, act_app22, act_app23, act_app24, act_app25, act_app26, act_app27, act_app28, act_app29, act_app30, act_app31};

static int category(const char *name) {
    for (int i = 0; i < category_total; ++i)
        if (!strcasecmp(category_names[i], name)) return i;
    if (category_total == MAX_CATEGORIES) return -1;
    snprintf(category_names[category_total], sizeof(category_names[0]), "%s", name);
    category_counts[category_total] = 0;
    return category_total++;
}

static void category_add(int c, struct menu_item item) {
    if (c >= 0 && category_counts[c] < MAX_CATEGORY_ITEMS) category_items[c][category_counts[c]++] = item;
}

static int category_icon(const char *name) {
    if (!strcasecmp(name, "System")) return ICON_COMPUTER;
    if (!strcasecmp(name, "Utilities")) return ICON_TOOLS;
    if (!strcasecmp(name, "Games")) return ICON_GAMES;
    if (!strcasecmp(name, "Internet")) return ICON_NET;
    if (!strcasecmp(name, "Multimedia")) return ICON_SOUND;
    if (!strcasecmp(name, "Office")) return ICON_EDITOR;
    return ICON_FOLDER;
}

static int start_menu_build(void) {
    int n = 0;
    if (accounts_on && me.name[0]) start_items[n++] = (struct menu_item){me.nickname, ICON_HEADER, 0};
    char name[32];
    int icon;
    default_program("browser", name, sizeof(name));
    if (name[0] && !program_info(name, browser_label, sizeof(browser_label), &icon))
        start_items[n++] = (struct menu_item){browser_label, icon, act_browser};
    default_program("editor", name, sizeof(name));
    if (!program_info(name, editor_label, sizeof(editor_label), &icon))
        start_items[n++] = (struct menu_item){editor_label, icon, act_text_editor};
    if (n && start_items[n - 1].icon != ICON_HEADER) start_items[n++] = (struct menu_item){0, 0, 0};
    category_total = 0;
    int system = category("System"), utilities = category("Utilities"), games = category("Games");
    static struct menu_item admin_menu[MAX_CATEGORY_ITEMS];
    int admin_count = 0;
    for (int i = 0; i < COUNT_OF(admin_items); ++i) admin_menu[admin_count++] = admin_items[i];
    struct menu_item admin_folder = {"Administrators", ICON_GEAR, 0, admin_menu, 0};
    int install_last = system_count == COUNT_OF(system_items);
    for (int i = 0; i < system_count - install_last; ++i) category_add(system, system_items[i]);
    category_add(system, admin_folder);
    int admin_slot = category_counts[system] - 1;
    if (install_last) category_add(system, system_items[system_count - 1]);
    for (int i = 0; i < COUNT_OF(utility_items); ++i) category_add(utilities, utility_items[i]);
    for (int i = 0; i < COUNT_OF(game_items); ++i) category_add(games, game_items[i]);
    menu_app_count = 0;
    static struct app_entry all[32];
    int count = apps_list(all, 32);
    for (int i = 0; i < count && menu_app_count < 32; ++i) {
        if (all[i].builtin) continue;
        menu_apps[menu_app_count] = all[i];
        struct menu_item item = {menu_apps[menu_app_count].name, all[i].icon, app_thunks[menu_app_count]};
        if (!strcasecmp(all[i].category, ADMIN_FOLDER)) { if (admin_count < MAX_CATEGORY_ITEMS) admin_menu[admin_count++] = item; }
        else category_add(category(all[i].category), item);
        ++menu_app_count;
    }
    if (system >= 0 && admin_slot >= 0) category_items[system][admin_slot].sub_count = admin_count;
    for (int c = 0; c < category_total; ++c)
        start_items[n++] = (struct menu_item){category_names[c], category_icon(category_names[c]), 0, category_items[c], category_counts[c]};
    start_items[n++] = (struct menu_item){0, 0, 0};
    start_items[n++] = (struct menu_item){"Control Panel", ICON_GEAR, open_control_panel};
    start_items[n++] = (struct menu_item){"About DOS-XC", ICON_INFO, open_about};
    start_items[n++] = (struct menu_item){0, 0, 0};
    start_items[n++] = (struct menu_item){"Exit to DOS", ICON_EXIT, act_exit};
    if (accounts_on && me.name[0]) start_items[n++] = (struct menu_item){"Log Out", ICON_LOGOUT, act_logout};
    start_items[n++] = (struct menu_item){"Restart", ICON_RESTART, ask_restart};
    start_items[n++] = (struct menu_item){"Shut Down", ICON_POWER, ask_shutdown};
    return n;
}

static void desktop_refresh(void) { icons_reload(); }
static const struct menu_item desktop_items[] = {
    {"New File...", ICON_FILE, icons_new_file},
    {"New Folder...", ICON_FOLDER, icons_new_folder},
    {"New Shortcut...", ICON_PROGRAM, icons_new_shortcut},
    {0, 0, 0},
    {"Refresh", ICON_RESTART, desktop_refresh},
    {"File Explorer", ICON_FOLDER, act_explorer},
    {0, 0, 0},
    {"Wallpaper...", ICON_PICTURE, open_wallpaper},
    {"Display Settings...", ICON_DISPLAY, open_display_settings},
    {"Control Panel", ICON_GEAR, open_control_panel},
};
static const struct menu_item icon_items[] = {
    {"Open", ICON_FOLDER, icons_open_selected},
    {0, 0, 0},
    {"Rename...", ICON_EDITOR, icons_rename},
    {"Delete", ICON_FILE, icons_delete},
};

static char popup_text[32][64];
static struct menu_item popup_items[32];
static struct window *popup_owner;
static int popup_tag;

static void popup_finish(int index) {
    struct window *w = popup_owner;
    popup_owner = 0;
    if (w) post_event(w, EV_POPUP, 0, 0, popup_tag, index);
}

static void popup_open(struct window *w, int x, int y, const char *items, int tag) {
    popup_finish(-1);
    int count = 0;
    while (*items && count < 32) {
        const char *end = strchr(items, '\n');
        if (!end) break;
        int n = (int)(end - items);
        if (n >= (int)sizeof(popup_text[0])) n = sizeof(popup_text[0]) - 1;
        memcpy(popup_text[count], items, (size_t)n);
        popup_text[count][n] = 0;
        popup_items[count] = (struct menu_item){strcmp(popup_text[count], "-") ? popup_text[count] : 0, -1, 0};
        ++count;
        items = end + 1;
    }
    popup_owner = w;
    popup_tag = tag;
    menu_open_at(w->r.x + x, w->r.y + y, popup_items, count, 0);
}

/* ---- wallpaper ---------------------------------------------------------- */

static void wallpaper_config(char *out, size_t size) {
    const char *home = getenv("HOME");
    snprintf(out, size, "%s\\DESKTOP.CFG", home && *home ? home : "\\Users\\Public");
}

const char *current_wallpaper(void) { return wall_path; }

static void wallpaper_apply(void) {
    for (int i = 0; i < screen_count; ++i) {
        struct screen *s = &screens[i];
        if (!wall_loaded) { free(s->wall.px); s->wall = (struct surface){0}; continue; }
        surface_size(&s->wall, s->r.w, s->r.h);
        image_cover(&wall_source, &s->wall);
    }
    damage_all();
}

static int wallpaper_load(const char *path) {
    struct surface fresh = {0};
    if (path[0] && image_load(path, &fresh)) return -1;
    free(wall_source.px);
    wall_source = fresh;
    wall_loaded = path[0] != 0;
    snprintf(wall_path, sizeof(wall_path), "%s", path);
    return 0;
}

static void desktop_config_save(void) {
    char config[300];
    wallpaper_config(config, sizeof(config));
    FILE *f = fopen(config, "w");
    if (!f) return;
    if (wall_path[0] || !wall_loaded) fprintf(f, "wallpaper=%s\n", wall_path);
    fprintf(f, "scale=%d\nvolume=%d\naccent=%06x\n", ui_scale, sound_volume, (unsigned)accent);
    if (cfg_browser[0]) fprintf(f, "browser=%s\n", cfg_browser);
    if (cfg_editor[0]) fprintf(f, "editor=%s\n", cfg_editor);
    fclose(f);
}

void set_wallpaper(const char *path) {
    if (wallpaper_load(path)) {
        message_box("Wallpaper", "That picture could not be read.", "BMP, PNG and JPEG pictures can be used.");
        return;
    }
    wallpaper_apply();
    desktop_config_save();
}

void set_sound_volume(int percent) {
    if (percent == sound_volume) return;
    sound_volume = percent;
    dosxc_sys_console(DOSXC_CONSOLE_VOLUME, percent, 0);
    damage_rect(volume_rect());
    desktop_config_save();
}

int sound_level(void) { return sound_volume; }

u32 current_accent(void) { return accent; }

/* Every window repaints in the new color; other programs see it on the board. */
void set_accent(u32 color, int save) {
    accent = color & 0xffffff;
    theme_set(accent);
    if (board) board->accent = accent;
    for (int i = 0; i < window_count; ++i)
        if (!windows[i]->remote) window_paint(windows[i]);
    damage_all();
    if (save) desktop_config_save();
}

void set_default_program(const char *kind, const char *name) {
    if (!strcmp(kind, "browser")) snprintf(cfg_browser, sizeof(cfg_browser), "%s", name);
    else snprintf(cfg_editor, sizeof(cfg_editor), "%s", name);
    desktop_config_save();
}

void desktop_damage(struct rect r) { damage_rect(r); }

struct rect desktop_area(void) { return main_work(); }

void window_raise(struct window *w) {
    if (w->minimized) w->minimized = 0;
    activate(w);
    damage_window(w);
}

static int config_wallpaper_found;
static char config_wallpaper[300];

static void config_load(void) {
    char config[300], line[320];
    wallpaper_config(config, sizeof(config));
    FILE *f = fopen(config, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "wallpaper=", 10)) {
            snprintf(config_wallpaper, sizeof(config_wallpaper), "%s", line + 10);
            config_wallpaper_found = 1;
        } else if (!strncmp(line, "scale=", 6)) {
            int v = atoi(line + 6);
            if (v == 100 || v == 125 || v == 150) ui_scale = v;
        } else if (!strncmp(line, "volume=", 7)) {
            int v = atoi(line + 7);
            if (v >= 0 && v <= 100) sound_volume = v;
        } else if (!strncmp(line, "browser=", 8)) {
            snprintf(cfg_browser, sizeof(cfg_browser), "%s", line + 8);
        } else if (!strncmp(line, "editor=", 7)) {
            snprintf(cfg_editor, sizeof(cfg_editor), "%s", line + 7);
        } else if (!strncmp(line, "accent=", 7)) {
            accent = (u32)strtoul(line + 7, 0, 16) & 0xffffff;
            theme_set(accent);
        }
    }
    fclose(f);
}

static void wallpaper_init(void) {
    char path[300];
    if (config_wallpaper_found) snprintf(path, sizeof(path), "%s", config_wallpaper);
    else desktop_path(path, sizeof(path), "Wallpapers\\Panda.jpg");
    if (!wallpaper_load(path)) wallpaper_apply();
}

/* ---- serving client programs -------------------------------------------- */

static void remote_mouse(struct window *w, int x, int y, int kind, int value) { post_event(w, EV_MOUSE, x, y, kind, value); }
static int key_modifiers;                   /* held with the key being delivered */
static void remote_key(struct window *w, int key) { post_event(w, EV_KEY, 0, 0, key_modifiers, key); }
static const struct app_ops remote_ops = {0, remote_mouse, remote_key, 0};

static void update_window_list(void) {
    int n = 0;
    for (int i = 0; i < task_count && n < GUI_WINDOWS; ++i) {
        struct window *w = task_order[i];
        struct gui_window_info *info = &board->windows[n++];
        info->pid = w->remote ? board->clients[w->client].pid : dosxc_sys_getpid();
        info->active = w == active;
        info->window = (uintptr_t)(w->remote ? w->remote : w);
        snprintf(info->title, sizeof(info->title), "%s", w->title);
    }
    board->window_count = n;
}

static void client_gone(int slot) {
    if (popup_owner && popup_owner->client == slot && popup_owner->remote) { popup_owner = 0; menu_close(); }
    for (int i = window_count - 1; i >= 0; --i)
        if (windows[i]->remote && windows[i]->client == slot) window_close(windows[i]);
    board->clients[slot].ev_read = board->clients[slot].ev_write = 0;
    board->clients[slot].rq_read = board->clients[slot].rq_write = 0;
    board->clients[slot].pid = 0;
}

static struct window *find_remote(int slot, uintptr_t pointer) {
    for (int i = 0; i < window_count; ++i)
        if (windows[i]->remote == (struct window *)pointer && windows[i]->client == slot) return windows[i];
    return 0;
}

static void client_request(int slot, const struct gui_request *r) {
    struct window *w = find_remote(slot, r->window);
    switch (r->type) {
    case RQ_OPEN: {
        struct window *c = (struct window *)r->window;
        int icon = c->icon >= 0 && c->icon < ICON_SLOTS ? c->icon : ICON_PROGRAM;
        if (icon == ICON_DOSFOX) icon_load_png(ICON_DOSFOX, "\\Programs\\dosfox\\DOSFOX.PNG");
        if (c->icon_file[0]) icon = icon_for_png(c->icon_file);
        struct window *fresh = window_create(c->content.w, c->content.h, c->title, icon, &remote_ops, 0, 1);
        if (fresh) {
            fresh->remote = c;
            fresh->client = slot;
            fresh->resizable = c->resizable;
            if (c->popup) {
                fresh->popup = 1;
                fresh->r = (struct rect){c->r.x, c->r.y, c->content.w, c->content.h};
                for (int i = 0; i < task_count; ++i)
                    if (task_order[i] == fresh) { task_order[i] = task_order[--task_count]; break; }
            }
        }
        break;
    }
    case RQ_CLOSE: if (w) window_close(w); break;
    case RQ_POPUP: if (w) popup_open(w, r->x, r->y, r->text, r->tag); break;
    case RQ_RESIZE:
        if (w && !w->maximized) {
            damage_window(w);
            w->r.w = r->x;
            w->r.h = r->y;
            window_resized(w);
            damage_window(w);
        }
        break;
    case RQ_WALLPAPER: set_wallpaper(r->text); break;
    case RQ_ACTIVATE:
        if (w && w->minimized) w->minimized = 0;
        if (!w) {
            for (int i = 0; i < window_count; ++i)
                if ((uintptr_t)windows[i] == r->window) w = windows[i];
        }
        if (w) { activate(w); damage_window(w); damage_rect(taskbar_rect()); }
        break;
    }
}

static void sync_client_windows(void) {
    board->screen_w = screens[main_screen].r.w;
    board->screen_h = screens[main_screen].r.h;
    for (int i = 0; i < window_count; ++i) {
        struct window *w = windows[i], *c = w->remote;
        if (!c) continue;
        if (c->seq != w->seen) {
            w->seen = c->seq;
            w->content.px = c->content.px;
            w->content.w = c->content.w;
            w->content.h = c->content.h;
            damage_window(w);
        }
        if (w->popup) {
            if (w->r.x != c->r.x || w->r.y != c->r.y || w->r.w != c->content.w || w->r.h != c->content.h) {
                damage_window(w);
                w->r = (struct rect){c->r.x, c->r.y, c->content.w, c->content.h};
                damage_window(w);
            }
        } else {
            c->r.x = w->r.x;
            c->r.y = w->r.y;
        }
        w->resizable = c->resizable;
        if (w->icon != c->icon && c->icon >= 0 && c->icon < ICON_SLOTS) { w->icon = c->icon; damage_window(w); }
        if (strcmp(w->title, c->title)) window_title(w, c->title);
    }
}

static void serve_clients(void) {
    static long long last_check;
    long long now = (long long)dosxc_sys_nanoseconds();
    int check = now - last_check > 250 * (long long)NS_PER_MS;
    if (check) last_check = now;
    for (int slot = 0; slot < GUI_CLIENTS; ++slot) {
        struct gui_client *c = &board->clients[slot];
        if (!c->pid) continue;
        if (check && dosxc_sys_task(DOSXC_TASK_ALIVE, 0, c->pid, 0) <= 0) { client_gone(slot); continue; }
        while (c->rq_read != c->rq_write) {
            struct gui_request request = c->requests[c->rq_read];
            c->rq_read = (c->rq_read + 1) % GUI_REQUESTS;
            client_request(slot, &request);
        }
    }
    sync_client_windows();
    if (check) update_window_list();
}

static void kill_clients(void) {
    for (int slot = 0; slot < GUI_CLIENTS; ++slot)
        if (board->clients[slot].pid) dosxc_sys_task(DOSXC_TASK_KILL, 0, board->clients[slot].pid, 0);
}

void leave_desktop(void) {
    dosxc_sys_display(DOSXC_DISPLAY_TEXT, 0, 0, 0);
    dosxc_sys_console(DOSXC_CONSOLE_MODE, 0, 0);
    fflush(stdout);
}

static void enter_desktop(void) {
    dosxc_sys_console(DOSXC_CONSOLE_MODE, DOSXC_CONSOLE_RAW, 0);
    layout_changed();
    mouse_seen = 0;
}

static void act_restart(void) { leave_desktop(); dosxc_sys_reboot(); }
static void act_shutdown(void) { leave_desktop(); dosxc_sys_power(DOSXC_POWER_OFF); }

/* ---- input -------------------------------------------------------------- */

static void move_cursor(int nx, int ny) {
    if (screen_at(nx, ny) < 0) {
        int on = screen_at(cursor_x, cursor_y);
        struct rect r = screens[on < 0 ? main_screen : on].r;
        nx = nx < r.x ? r.x : nx >= r.x + r.w ? r.x + r.w - 1 : nx;
        ny = ny < r.y ? r.y : ny >= r.y + r.h ? r.y + r.h - 1 : ny;
    }
    if (nx == cursor_x && ny == cursor_y) return;
    damage_rect((struct rect){cursor_x, cursor_y, CURSOR_W, CURSOR_H});
    cursor_x = nx;
    cursor_y = ny;
    damage_rect((struct rect){cursor_x, cursor_y, CURSOR_W, CURSOR_H});
}

static void update_drag(void) {
    struct window *w = drag.w;
    if (drag.mode == DRAG_MOVE) {
        struct rect old = frame_of(w);
        w->r.x = drag.start.x + cursor_x - drag.grab_x;
        w->r.y = drag.start.y + cursor_y - drag.grab_y;
        damage_rect(old);
        damage_window(w);
    } else if (drag.mode == DRAG_RESIZE) {
        struct rect old = frame_of(w), r = drag.start;
        int dx = cursor_x - drag.grab_x, dy = cursor_y - drag.grab_y;
        if (drag.edges & EDGE_R) r.w += dx;
        if (drag.edges & EDGE_B) r.h += dy;
        if (drag.edges & EDGE_L) { r.x += dx; r.w -= dx; }
        if (drag.edges & EDGE_T) { r.y += dy; r.h -= dy; }
        if (r.w < 160) { if (drag.edges & EDGE_L) r.x += r.w - 160; r.w = 160; }
        if (r.h < 80) { if (drag.edges & EDGE_T) r.y += r.h - 80; r.h = 80; }
        if (r.w != w->r.w || r.h != w->r.h) {
            w->r = r;
            window_resized(w);
        } else w->r = r;
        damage_rect(old);
        damage_window(w);
    } else if (drag.mode == DRAG_CAPTURE && w->ops->mouse) {
        w->ops->mouse(w, cursor_x - w->r.x, cursor_y - w->r.y, M_MOVE, 0);
    }
}

void open_network(void) {
    struct dosxc_netinfo info;
    int count = dosxc_sys_net(DOSXC_NET_INFO, 0, &info);
    if (count <= 0) { message_box("Network", "No network adapter was found.", "Install the ethernet package to add drivers."); return; }
    char line1[80], line2[80];
    snprintf(line1, sizeof(line1), "%s: %s", info.name, info.link ? "connected" : "no cable");
    if (info.ip) snprintf(line2, sizeof(line2), "Address %u.%u.%u.%u", info.ip >> 24, info.ip >> 16 & 255, info.ip >> 8 & 255, info.ip & 255);
    else snprintf(line2, sizeof(line2), "%s", "Waiting for an address (DHCP).");
    message_box("Network", line1, line2);
}

static void mouse_down_left(int doubled) {
    if (menu.open) {
        int n = menu_item_at(&nested, cursor_x, cursor_y);
        int s = n >= 0 ? -1 : menu_item_at(&submenu, cursor_x, cursor_y);
        int i = n >= 0 || s >= 0 ? -1 : menu_item_at(&menu, cursor_x, cursor_y);
        const struct menu_item *m = n >= 0 ? &nested.items[n] : s >= 0 && !submenu.items[s].sub ? &submenu.items[s]
                                  : i >= 0 && !menu.items[i].sub ? &menu.items[i] : 0;
        if ((i >= 0 && menu.items[i].sub) || (s >= 0 && submenu.items[s].sub)) return;
        int in_start = rect_has(start_button_rect(), cursor_x, cursor_y);
        int from_client = popup_owner != 0;
        menu_close();
        if (from_client) { popup_finish(m ? (s >= 0 ? s : i) : -1); if (m) return; }
        else if (m) m->act();
        if (m || in_start) return;
    }
    if (!logon_mode && rect_has(start_button_rect(), cursor_x, cursor_y)) {
        struct rect r = start_button_rect();
        int count = start_menu_build();
        int height = menu_height(start_items, count);
        menu_open_at(r.x, r.y - height, start_items, count, 1);
        return;
    }
    if (!logon_mode && rect_has(volume_rect(), cursor_x, cursor_y)) { open_sound(); return; }
    if (!logon_mode && rect_has(net_rect(), cursor_x, cursor_y)) { open_network(); return; }
    for (int i = 0; i < task_count; ++i)
        if (!logon_mode && rect_has(taskbar_rect(), cursor_x, cursor_y) && rect_has(task_button_rect(i), cursor_x, cursor_y)) {
            struct window *w = task_order[i];
            if (w->minimized) { w->minimized = 0; activate(w); }
            else if (w == active) window_minimize(w);
            else activate(w);
            damage_window(w);
            damage_rect(taskbar_rect());
            return;
        }
    if (!logon_mode && rect_has(taskbar_rect(), cursor_x, cursor_y)) return;
    for (int i = window_count - 1; i >= 0; --i) {
        struct window *w = windows[i];
        int edges = 0, hit = w->minimized ? HIT_NONE : window_hit(w, cursor_x, cursor_y, &edges);
        if (hit == HIT_NONE) continue;
        activate(w);
        drag.w = w;
        drag.grab_x = cursor_x;
        drag.grab_y = cursor_y;
        drag.start = w->r;
        drag.mode = DRAG_NONE;
        switch (hit) {
        case HIT_CONTENT:
            if (w->ops->mouse) w->ops->mouse(w, cursor_x - w->r.x, cursor_y - w->r.y, doubled ? M_DOUBLE : M_DOWN, 0);
            if (drag.w == w) drag.mode = DRAG_CAPTURE;
            break;
        case HIT_TITLE:
            if (doubled && w->resizable) window_maximize(w);
            else if (!w->maximized) { drag.mode = DRAG_MOVE; damage_window(w); }
            break;
        case HIT_EDGE:
            drag.mode = DRAG_RESIZE;
            drag.edges = edges;
            break;
        case HIT_CLOSE:
            if (w->remote) post_event(w, EV_CLOSE, 0, 0, 0, 0);
            else window_close(w);
            break;
        case HIT_MAX: window_maximize(w); break;
        case HIT_MIN: window_minimize(w); break;
        }
        return;
    }
    activate(0);
    if (logon_mode) { logon_click(cursor_x, cursor_y); return; }
    int at = icons_at(cursor_x, cursor_y);
    icons_select(at);
    if (doubled && at >= 0) icons_open(at);
}

static void mouse_up_left(void) {
    if (drag.mode == DRAG_CAPTURE && drag.w && drag.w->ops->mouse)
        drag.w->ops->mouse(drag.w, cursor_x - drag.w->r.x, cursor_y - drag.w->r.y, M_UP, 0);
    if (drag.mode == DRAG_MOVE) damage_window(drag.w);
    drag.mode = DRAG_NONE;
    drag.w = 0;
}

static void mouse_down_right(void) {
    if (menu.open) { popup_finish(-1); menu_close(); return; }
    if (logon_mode) return;
    if (rect_has(taskbar_rect(), cursor_x, cursor_y)) return;
    for (int i = window_count - 1; i >= 0; --i) {
        struct window *w = windows[i];
        if (w->minimized || !rect_has(frame_of(w), cursor_x, cursor_y)) continue;
        activate(w);
        if (rect_has(w->r, cursor_x, cursor_y) && w->ops->mouse)
            w->ops->mouse(w, cursor_x - w->r.x, cursor_y - w->r.y, M_RIGHT, 0);
        return;
    }
    activate(0);
    int at = icons_at(cursor_x, cursor_y);
    icons_select(at);
    if (at >= 0) {
        int count = icons_selected_is_item() ? COUNT_OF(icon_items) : 1;
        menu_open_at(cursor_x, cursor_y, icon_items, count, 0);
        return;
    }
    menu_open_at(cursor_x, cursor_y, desktop_items, COUNT_OF(desktop_items), 0);
}

static void update_menu_hover(void) {
    if (!menu.open) return;
    if (nested.open) {
        int k = menu_item_at(&nested, cursor_x, cursor_y);
        if (k != nested.hot) { nested.hot = k; damage_rect(nested.r); }
        if (rect_has(nested.r, cursor_x, cursor_y)) return;
    }
    if (submenu.open) {
        int k = menu_item_at(&submenu, cursor_x, cursor_y);
        if (k != submenu.hot) { submenu.hot = k; damage_rect(submenu.r); }
        if (k >= 0 && submenu.items[k].sub && k != nested_index) nested_open(k);
        else if (k >= 0 && !submenu.items[k].sub) nested_close();
        if (rect_has(submenu.r, cursor_x, cursor_y)) return;
    }
    int i = menu_item_at(&menu, cursor_x, cursor_y);
    if (i != menu.hot) { menu.hot = i; damage_rect(menu.r); }
    if (i < 0 || (submenu.open && i == submenu_index)) { submenu_pending = -2; return; }
    int want = menu.items[i].sub ? i : -1;
    if (!submenu.open) {
        if (want >= 0) submenu_open(want);
        return;
    }
    long long now = (long long)dosxc_sys_nanoseconds();
    if (want != submenu_pending) { submenu_pending = want; submenu_since = now; return; }
    if (now - submenu_since < 250 * (long long)NS_PER_MS) return;
    if (want >= 0) submenu_open(want);
    else submenu_close();
    submenu_pending = -2;
}

static void input_mouse(void) {
    struct dosxc_pointer p = {0, 0, 0, 0};
    int present = dosxc_sys_console(DOSXC_CONSOLE_POINTER, (long)&p, 0) > 0;
    if (present) mouse_seen = 1;
    if (p.dx || p.dy) {
        accum_x += p.dx * 100;
        accum_y += p.dy * 100;
        int mx = accum_x / ui_scale, my = accum_y / ui_scale;
        accum_x -= mx * ui_scale;
        accum_y -= my * ui_scale;
        move_cursor(cursor_x + mx, cursor_y + my);
        for (int i = window_count - 1; i >= 0; --i) {
            struct window *w = windows[i];
            if (w->minimized || !rect_has(frame_of(w), cursor_x, cursor_y)) continue;
            if (w->remote && w->remote->track_mouse && w == active && rect_has(w->r, cursor_x, cursor_y))
                post_event(w, EV_MOUSE, cursor_x - w->r.x, cursor_y - w->r.y, M_MOVE, 0);
            break;
        }
    }
    if (drag.mode != DRAG_NONE && (p.dx || p.dy)) update_drag();
    update_menu_hover();
    unsigned changed = p.buttons ^ buttons;
    buttons = p.buttons;
    if (changed & 1) {
        static long long last_click;
        static int last_x, last_y;
        if (p.buttons & 1) {
            long long now = (long long)dosxc_sys_nanoseconds();
            int dx = cursor_x - last_x, dy = cursor_y - last_y;
            int doubled = now - last_click < 450 * (long long)NS_PER_MS && dx * dx + dy * dy < 25;
            last_click = doubled ? 0 : now;
            last_x = cursor_x;
            last_y = cursor_y;
            mouse_down_left(doubled);
        } else mouse_up_left();
    }
    if ((changed & 2) && (p.buttons & 2)) mouse_down_right();
    if (p.wheel) {
        for (int i = window_count - 1; i >= 0; --i)
            if (!windows[i]->minimized && rect_has(frame_of(windows[i]), cursor_x, cursor_y)) {
                if (rect_has(windows[i]->r, cursor_x, cursor_y) && windows[i]->ops->mouse)
                    windows[i]->ops->mouse(windows[i], cursor_x - windows[i]->r.x, cursor_y - windows[i]->r.y, M_WHEEL, p.wheel);
                break;
            }
    }
}

static void input_keyboard(void) {
    for (int i = 0; i < 32; ++i) {
        int key = dosxc_sys_console(DOSXC_CONSOLE_GETKEY, 0, 0);
        if (key < 0) return;
        key_modifiers = dosxc_sys_console(DOSXC_CONSOLE_MODIFIERS, 0, 0);
        if (key == 0x116) { running = 0; return; }           /* F12: back to DOS, even without a mouse */
        if (key == 27 && menu.open) { popup_finish(-1); menu_close(); continue; }
        if (active && active->ops->key) active->ops->key(active, key);
        else if (!active && logon_mode) logon_key(key);
        else if (!active) icons_key(key);
    }
}

static void update_clock(void) {
    int minute = (int)(dosxc_sys_time() / 60);
    if (minute != clock_minute) {
        clock_minute = minute;
        damage_rect(clock_rect());
    }
    struct dosxc_display d;
    if (dosxc_sys_display(DOSXC_DISPLAY_INFO, 0, &d, 0) > 0 && !d.width) enter_desktop();
    long long now_ns = (long long)dosxc_sys_nanoseconds();
    if (now_ns >= tray_next) {
        tray_next = now_ns + 2000 * (long long)NS_PER_MS;
        struct dosxc_netinfo info;
        int count = dosxc_sys_net(DOSXC_NET_INFO, 0, &info);
        net_state = count > 0 && info.link && info.ip;
        if (net_state != net_shown) { net_shown = net_state; damage_rect(net_rect()); }
        if (sound_volume != volume_shown) { volume_shown = sound_volume; damage_rect(volume_rect()); }
    }
    if (identify_until && (long long)dosxc_sys_nanoseconds() >= identify_until) {
        identify_until = 0;
        damage_all();
    }
}

int main(int argc, char **argv) {
    icons_init();
    if (argc > 1 && !strcmp(argv[1], "/LOGON")) {
        logon_mode = 1;
        argc = 1;
    }
    accounts_on = dosxc_sys_user(DOSXC_USER_ENABLED, 0, 0) > 0;
    dosxc_sys_user(DOSXC_USER_WHOAMI, 0, &me);
    if (accounts_on && me.picture[0]) user_picture(me.picture, 40, &me_picture);
    if (!logon_mode) config_load();
    dosxc_sys_console(DOSXC_CONSOLE_VOLUME, sound_volume, 0);
    {
        struct dosxc_sysinfo info;
        dosxc_sys_sysinfo(&info);
        if (!info.live) system_count = COUNT_OF(system_items) - 1;
    }
    board = calloc(1, sizeof(*board));
    if (!board) return 1;
    board->magic = GUI_MAGIC;
    board->accent = accent;
    if (dosxc_sys_task(DOSXC_TASK_SHARE_SET, board, (long)sizeof(*board), 0)) {
        puts("DESKTOP: the desktop is already running.");
        return 1;
    }
    if (screens_init()) {
        puts("DESKTOP: this computer has no usable graphics display.");
        return 1;
    }
    dosxc_sys_console(DOSXC_CONSOLE_MODE, DOSXC_CONSOLE_RAW, 0);
    apply_modes();
    struct rect a = screens[main_screen].r;
    cursor_x = a.x + a.w / 2;
    cursor_y = a.y + a.h / 2;
    if (logon_mode) logon_init();
    else {
        wallpaper_init();
        icon_load_png(ICON_DOSFOX, "\\Programs\\dosfox\\DOSFOX.PNG");
        icons_reload();
    }
    damage_all();
    composite();
    struct dosxc_pointer probe;
    for (int i = 0; i < 40 && !mouse_seen; ++i) {
        if (dosxc_sys_console(DOSXC_CONSOLE_POINTER, (long)&probe, 0) > 0) mouse_seen = 1;
        else dosxc_sys_sleep(5 * NS_PER_MS);
    }
    if (!mouse_seen) message_box("No mouse", "DOS-XC could not find a mouse.", "Press F12 to return to DOS.");
    if (argc > 1)
        dosxc_sys_task(DOSXC_TASK_START, argv[1], (long)(argv + 1), 0);
    while (running) {
        input_mouse();
        input_keyboard();
        update_clock();
        serve_clients();
        if (!logon_mode) icons_poll();
        composite();
        dosxc_sys_sleep(6 * NS_PER_MS);
    }
    kill_clients();
    leave_desktop();
    return exit_status;
}
