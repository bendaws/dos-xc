/*
 * DOS-XC /desktop/gui.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "wm.h"
#include <strings.h>

#define MAX_OWN 16
#define TICK_NS 100000000ll

static struct gui_board *board;
static struct gui_client *self;
static struct window *own[MAX_OWN];
static int own_count;
static struct window *modal;
static u32 accent_seen;
struct gui_board *gui_board(void) { return board; }

int gui_connect(void) {
    board = (struct gui_board *)dosxc_sys_task(DOSXC_TASK_SHARE_GET, 0, 0, 0);
    if (!board || board->magic != GUI_MAGIC) {
        puts("This program runs on the desktop.  Type DESKTOP to start it.");
        return -1;
    }
    icons_init();
    accent_seen = board->accent;
    theme_set(accent_seen);
    int pid = dosxc_sys_getpid();
    for (int i = 0; i < GUI_CLIENTS; ++i)
        if (__sync_bool_compare_and_swap(&board->clients[i].pid, 0, pid)) {
            self = &board->clients[i];
            return 0;
        }
    return -1;
}

static void request(const struct gui_request *r) {
    u32 next = (self->rq_write + 1) % GUI_REQUESTS;
    while (next == self->rq_read) dosxc_sys_sleep(2000000);
    self->requests[self->rq_write] = *r;
    self->rq_write = next;
}

static void simple_request(int type, struct window *w, const char *text) {
    struct gui_request r;
    memset(&r, 0, sizeof(r));
    r.type = type;
    r.window = (uintptr_t)w;
    if (text) snprintf(r.text, sizeof(r.text), "%s", text);
    request(&r);
}

static char icon_file[160];
void gui_set_icon_file(const char *path) { snprintf(icon_file, sizeof(icon_file), "%s", path ? path : ""); }

struct window *window_open(int w, int h, const char *title, int icon, const struct app_ops *ops, void *data) {
    if (own_count == MAX_OWN) return 0;
    struct window *win = calloc(1, sizeof(*win));
    if (!win) return 0;
    if (surface_size(&win->content, w, h)) { free(win); return 0; }
    win->r = (struct rect){0, 0, w, h};
    snprintf(win->title, sizeof(win->title), "%s", title);
    win->icon = icon;
    snprintf(win->icon_file, sizeof(win->icon_file), "%s", icon_file);
    win->ops = ops;
    win->data = data;
    win->needs_paint = 1;
    own[own_count++] = win;
    simple_request(RQ_OPEN, win, 0);
    return win;
}

void window_modal(struct window *w) { modal = w; }

void window_close(struct window *w) {
    for (int i = 0; i < own_count; ++i)
        if (own[i] == w) { own[i] = own[--own_count]; break; }
    if (modal == w) modal = 0;
    if (w->ops->closed) w->ops->closed(w);
    simple_request(RQ_CLOSE, w, 0);
    /* The desktop may be reading the pixels: the memory is left until the
       program ends; dialogs are small and few. */
}

void window_title(struct window *w, const char *title) { snprintf(w->title, sizeof(w->title), "%s", title); }

void window_popup(struct window *w, int x, int y, const char *items, int tag) {
    struct gui_request r;
    memset(&r, 0, sizeof(r));
    r.type = RQ_POPUP;
    r.window = (uintptr_t)w;
    r.x = x;
    r.y = y;
    r.tag = tag;
    snprintf(r.text, sizeof(r.text), "%s", items);
    request(&r);
}

int gui_launch(const char *program, const char *argument) {
    char path[128], relative[64], name[32], arg[300];
    snprintf(relative, sizeof(relative), "Apps\\%s", program);
    desktop_path(path, sizeof(path), relative);
    snprintf(name, sizeof(name), "%s", program);
    snprintf(arg, sizeof(arg), "%s", argument ? argument : "");
    char *argv[3] = {name, arg, 0};
    if (!argument) argv[1] = 0;
    return (int)dosxc_sys_task(DOSXC_TASK_START, path, (long)argv, 0);
}

void gui_open_file(const char *path) {
    if (open_path(path)) message_box("Cannot open", path, "No program could open it.");
}

void window_request_size(struct window *w, int width, int height) {
    struct gui_request r;
    memset(&r, 0, sizeof(r));
    r.type = RQ_RESIZE;
    r.window = (uintptr_t)w;
    r.x = width;
    r.y = height;
    request(&r);
}

void gui_run_text(const char *path) { gui_launch("TERMINAL.EXE", path); }

static int (*poll_hook)(void);
void gui_set_poll(int (*poll)(void)) { poll_hook = poll; }
void gui_set_wallpaper(const char *path) { simple_request(RQ_WALLPAPER, 0, path); }
void gui_activate(struct gui_window_info *w) { simple_request(RQ_ACTIVATE, (struct window *)w->window, 0); }

void gui_clipboard_set(const char *text, int size) {
    if (size > (int)sizeof(board->clipboard)) size = sizeof(board->clipboard);
    memcpy(board->clipboard, text, (size_t)size);
    board->clipboard_size = size;
}

int gui_clipboard_get(char *out, int capacity) {
    int n = board->clipboard_size;
    if (n > capacity) n = capacity;
    memcpy(out, board->clipboard, (size_t)n);
    return n;
}

static int is_own(const struct window *w) {
    for (int i = 0; i < own_count; ++i)
        if (own[i] == w) return 1;
    return 0;
}

static int dispatch(void) {
    int did = 0;
    if (board->accent != accent_seen) {
        accent_seen = board->accent;
        theme_set(accent_seen);
        for (int i = 0; i < own_count; ++i)
            if (!own[i]->needs_paint) own[i]->needs_paint = 1;
        did = 1;
    }
    while (self->ev_read != self->ev_write) {
        struct gui_event e = self->events[self->ev_read];
        self->ev_read = (self->ev_read + 1) % GUI_RING;
        did = 1;
        struct window *w = (struct window *)e.window;
        if (!is_own(w)) continue;
        if (modal && w != modal && (e.type == EV_MOUSE || e.type == EV_KEY)) continue;
        switch (e.type) {
        case EV_MOUSE: if (w->ops->mouse) w->ops->mouse(w, e.x, e.y, e.kind, e.value); break;
        case EV_KEY: if (w->ops->key) w->ops->key(w, e.value); break;
        case EV_CLOSE:
            if ((!modal || w == modal) && (!w->ops->closing || w->ops->closing(w))) window_close(w);
            break;
        case EV_POPUP: if (w->ops->popup) w->ops->popup(w, e.kind, e.value); break;
        case EV_RESIZE:
            w->r.w = e.x;
            w->r.h = e.y;
            w->needs_paint = 2;
            break;
        }
    }
    return did;
}

static void paint_all(void) {
    for (int i = 0; i < own_count; ++i) {
        struct window *w = own[i];
        if (!w->needs_paint) continue;
        if (w->needs_paint == 2) {
            struct surface next = {0};
            if (w->r.w != w->content.w || w->r.h != w->content.h) {
                if (surface_size(&next, w->r.w, w->r.h)) { w->needs_paint = 0; continue; }
                u32 *old = w->content.px;
                w->content = next;
                free(old);
            }
            if (w->ops->resized) w->ops->resized(w);
        }
        w->needs_paint = 0;
        struct canvas c = {&w->content, 0, 0, {0, 0, w->content.w, w->content.h}};
        fill(&c, 0, 0, w->content.w, w->content.h, C_WINDOW);
        if (w->ops->paint) w->ops->paint(w, &c);
        ++w->seq;
    }
}

void gui_pump(void) {
    dispatch();
    paint_all();
    dosxc_sys_task(DOSXC_TASK_YIELD, 0, 0, 0);
}

void gui_run(void) {
    long long next_tick = 0;
    for (;;) {
        int did = dispatch();
        if (poll_hook && poll_hook()) did = 1;
        long long now = (long long)dosxc_sys_nanoseconds();
        if (now >= next_tick) {
            next_tick = now + TICK_NS;
            for (int i = 0; i < own_count; ++i)
                if (own[i]->ops->tick) own[i]->ops->tick(own[i]);
        }
        paint_all();
        if (!own_count) exit(0);
        if (!did) dosxc_sys_sleep(4000000);
    }
}
