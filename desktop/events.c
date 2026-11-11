/*
 * DOS-XC /desktop/events.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "wm.h"
#include <strings.h>
#include <time.h>

#define MAX_EVENTS 512
#define ROW 18
#define BAR_H 30
#define DETAIL_H 64

static const struct menubar bar = {
    {"File", "View", "Help"},
    {"Save As...\nClear Log\n-\nExit\n", "All Events\nWarnings and Errors\nErrors\n-\nRefresh\n", "About Event Logs\n"},
};
static const char *const level_names[3] = {"Information", "Warning", "Error"};
static const u32 level_colors[3] = {0x1f5fbf, 0xc08000, 0xc00000};

static struct dosxc_event events[MAX_EVENTS];
static int event_count, selected = -1, minimum_level, denied;
static unsigned last_next;
static struct vscroll scroll;
static struct window *main_w;

/* Newest first; only levels at or above minimum_level. */
static void refresh(int force) {
    unsigned range[2] = {0, 0};
    dosxc_sys_events(DOSXC_EVENTS_RANGE, range, 0);
    if (!force && range[1] == last_next) return;
    last_next = range[1];
    unsigned keep = selected >= 0 && selected < event_count ? events[selected].number : 0;
    event_count = 0;
    selected = -1;
    for (unsigned n = range[1]; n-- > range[0] && n && event_count < MAX_EVENTS;) {
        struct dosxc_event e;
        long r = dosxc_sys_events(DOSXC_EVENTS_READ, (void *)(unsigned long)n, &e);
        if (r == -DOSXC_EACCES) { denied = 1; return; }
        if (r || !e.number || e.level < minimum_level) continue;
        if (e.number == keep) selected = event_count;
        events[event_count++] = e;
    }
    scroll.total = event_count;
}

static void when(const struct dosxc_event *e, char *out, size_t size) {
    time_t t = (time_t)e->time;
    struct tm *tm = gmtime(&t);
    if (tm) strftime(out, size, "%Y-%m-%d %H:%M:%S", tm);
    else snprintf(out, size, "?");
}

static struct rect list_rect(const struct window *w) {
    return (struct rect){6, MENUBAR_H + BAR_H, w->content.w - 12, w->content.h - MENUBAR_H - BAR_H - DETAIL_H - 12};
}

static void paint(struct window *w, const struct canvas *c) {
    fill(c, 0, 0, w->content.w, w->content.h, C_WINDOW);
    menubar_paint(c, &bar, w->content.w);
    static const char *const filters[3] = {"All Events", "Warnings and Errors", "Errors"};
    char heading[96];
    snprintf(heading, sizeof(heading), "%s: %d event%s", filters[minimum_level], event_count, event_count == 1 ? "" : "s");
    text(c, 10, MENUBAR_H + 9, &font_bold, heading, C_TEXT);
    button(c, (struct rect){w->content.w - 168, MENUBAR_H + 4, 78, 22}, "Refresh", 0, 0);
    button(c, (struct rect){w->content.w - 84, MENUBAR_H + 4, 78, 22}, "Clear", 0, 0);
    struct rect r = list_rect(w);
    bevel(c, r.x, r.y, r.w, r.h, 1);
    fill(c, r.x + 2, r.y + 2, r.w - 4, r.h - 4, C_BASE);
    if (denied) {
        text(c, r.x + 12, r.y + 12, &font_bold, "Only administrators can view the event log.", C_TEXT);
        return;
    }
    static const char *const headers[] = {"Level", "Date and Time", "Source", "Message"};
    int columns[4] = {0, 96, 236, 336};
    for (int i = 0; i < 4; ++i) {
        int x = r.x + 2 + columns[i], cw = (i < 3 ? columns[i + 1] : r.w - 18) - columns[i];
        bevel(c, x, r.y + 2, cw, ROW, 0);
        text(c, x + 5, r.y + 7, &font_regular, headers[i], C_TEXT);
    }
    scroll.visible = (r.h - ROW - 4) / ROW;
    vscroll_clamp(&scroll);
    struct canvas inner = *c;
    inner.clip = rect_both(c->clip, (struct rect){r.x + 2, r.y + 2, r.w - 20, r.h - 4});
    for (int i = 0; i < scroll.visible && scroll.top + i < event_count; ++i) {
        const struct dosxc_event *e = &events[scroll.top + i];
        int y = r.y + 2 + ROW + i * ROW, on = scroll.top + i == selected;
        u32 color = on ? 0xffffff : C_TEXT;
        if (on) fill(&inner, r.x + 2, y, r.w - 20, ROW, C_SELECTION);
        int level = e->level >= 0 && e->level < 3 ? e->level : 0;
        fill(&inner, r.x + 8, y + 5, 8, 8, level_colors[level]);
        text(&inner, r.x + 20, y + 4, &font_regular, level_names[level], color);
        char stamp[32];
        when(e, stamp, sizeof(stamp));
        text(&inner, r.x + 2 + columns[1] + 5, y + 4, &font_regular, stamp, color);
        text(&inner, r.x + 2 + columns[2] + 5, y + 4, &font_regular, e->source, color);
        text(&inner, r.x + 2 + columns[3] + 5, y + 4, &font_regular, e->text, color);
    }
    vscroll_paint(c, &scroll, (struct rect){r.x + r.w - 16, r.y + 2, 14, r.h - 4});
    int dy = r.y + r.h + 6;
    bevel(c, 6, dy, w->content.w - 12, DETAIL_H, 1);
    fill(c, 8, dy + 2, w->content.w - 16, DETAIL_H - 4, C_BASE);
    if (selected >= 0) {
        const struct dosxc_event *e = &events[selected];
        char line[200], stamp[32];
        when(e, stamp, sizeof(stamp));
        snprintf(line, sizeof(line), "Event %u, %s, %s", e->number, level_names[e->level >= 0 && e->level < 3 ? e->level : 0], stamp);
        text(c, 14, dy + 8, &font_bold, line, C_TEXT);
        snprintf(line, sizeof(line), "Source: %s    Process: %d    User: %s", e->source, e->pid, e->user[0] ? e->user : "System");
        text(c, 14, dy + 26, &font_regular, line, C_TEXT);
        text(c, 14, dy + 44, &font_regular, e->text, C_TEXT);
    } else text(c, 14, dy + 8, &font_regular, "Select an event to see its details.", C_SHADOW1);
}

static void saved(const char *path, void *data) {
    (void)data;
    if (!path) return;
    FILE *f = fopen(path, "w");
    if (!f) { message_box("Event Logs", "The log could not be saved.", path); return; }
    for (int i = event_count; i--;) {
        const struct dosxc_event *e = &events[i];
        char stamp[32];
        when(e, stamp, sizeof(stamp));
        fprintf(f, "%u\t%s\t%s\t%s\t%d\t%s\t%s\r\n", e->number, stamp, level_names[e->level >= 0 && e->level < 3 ? e->level : 0], e->source, e->pid,
                e->user[0] ? e->user : "System", e->text);
    }
    fclose(f);
}

static void cleared(int ok, void *data) {
    (void)data;
    if (ok && dosxc_sys_events(DOSXC_EVENTS_CLEAR, 0, 0)) message_box("Event Logs", "Only administrators can clear the log.", "");
    refresh(1);
    window_paint(main_w);
}

static void popup(struct window *w, int tag, int index) {
    if (tag == 0) {
        if (index == 0) file_dialog("Save the log as", 1, 0, saved, 0);
        else if (index == 1) confirm_box("Clear Log", "Remove every event from the log?", "Clear", ICON_INFO, cleared, 0);
        else if (index == 3) window_close(w);
    } else if (tag == 1) {
        if (index >= 0 && index <= 2) { minimum_level = index; refresh(1); }
        else if (index == 4) refresh(1);
    } else if (tag == 2 && index == 0) {
        message_box("About Event Logs", "Event Logs shows what DOS-XC records: programs starting,", "ending and stopping, drivers, devices, logins and settings.");
    }
    window_paint(w);
}

static void mouse(struct window *w, int x, int y, int kind, int value) {
    if (menubar_mouse(w, &bar, x, y, kind)) return;
    struct rect r = list_rect(w);
    if (vscroll_mouse(&scroll, (struct rect){r.x + r.w - 16, r.y + 2, 14, r.h - 4}, x, y, kind, value)) { window_paint(w); return; }
    if (kind == M_WHEEL) { scroll.top -= value * 3; vscroll_clamp(&scroll); window_paint(w); return; }
    if (kind != M_DOWN) return;
    if (y >= MENUBAR_H + 4 && y < MENUBAR_H + 26) {
        if (x >= w->content.w - 168 && x < w->content.w - 90) refresh(1);
        else if (x >= w->content.w - 84 && x < w->content.w - 6) popup(w, 0, 1);
    } else if (rect_has(r, x, y) && y >= r.y + 2 + ROW) {
        int at = scroll.top + (y - r.y - 2 - ROW) / ROW;
        if (at < event_count) selected = at;
    }
    window_paint(w);
}

static void key(struct window *w, int k) {
    if (k == 0x101 && selected > 0) --selected;
    else if (k == 0x102 && selected + 1 < event_count) ++selected;
    else if (k == 0x10f) refresh(1);
    else return;
    if (selected >= 0 && selected < scroll.top) scroll.top = selected;
    if (selected >= scroll.top + scroll.visible) scroll.top = selected - scroll.visible + 1;
    window_paint(w);
}

static void tick(struct window *w) {
    static int counter;
    if (++counter % 20) return;
    unsigned before = last_next;
    refresh(0);
    if (last_next != before) window_paint(w);
}

static const struct app_ops ops = {paint, mouse, key, 0, popup, tick, 0, 0};

int main(void) {
    if (gui_connect()) { puts("Event Logs needs the DOS-XC desktop."); return 1; }
    refresh(1);
    main_w = window_open(640, 420, "Event Logs", ICON_INFO, &ops, 0);
    if (!main_w) return 1;
    main_w->resizable = 1;
    gui_run();
    return 0;
}
