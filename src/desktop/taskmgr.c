/*
 * DOS-XC /desktop/taskmgr.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * TASKMGR.EXE: Task Manager, laid out like the classic one: Applications,
 * Processes and Performance tabs, with CPU and memory history graphs.
 */
#include "wm.h"
#include <strings.h>

#define MAX_PROCS 40
#define HISTORY 120
#define TAB_Y (MENUBAR_H + 4)
#define TAB_H 22
#define PAGE_Y (TAB_Y + TAB_H)
#define STATUS_H 22
#define ROW 18

static const struct menubar bar = {
    {"File", "View", "Help"},
    {"New Task...\n-\nExit Task Manager\n", "Refresh Now\n", "About Task Manager\n"},
};
static const char *const tabs[3] = {"Applications", "Processes", "Performance"};

struct proc { int pid, percent; unsigned long long last_cpu, memory; char name[64], full[260]; };
static struct proc procs[MAX_PROCS];
static int proc_count, tab = 1, selected_app = -1, selected_proc = -1, cpu_percent;
static struct vscroll proc_scroll, app_scroll;
static struct gui_window_info apps[GUI_WINDOWS];
static int app_count;
static unsigned long long last_now, last_idle;
static unsigned char cpu_history[HISTORY];
static int history_count;
static unsigned char mem_history[HISTORY];
static struct dosxc_task_stats stats;
static struct window *main_w;

static struct rect tab_rect(int i) {
    int x = 8;
    for (int k = 0; k < i; ++k) x += text_width(&font_regular, tabs[k]) + 22;
    return (struct rect){x, TAB_Y + (i == tab ? 0 : 2), text_width(&font_regular, tabs[i]) + 20, TAB_H - (i == tab ? 0 : 2)};
}
static struct rect page_rect(const struct window *w) { return (struct rect){6, PAGE_Y, w->content.w - 12, w->content.h - PAGE_Y - STATUS_H - 6}; }
static struct rect list_rect(const struct window *w) {
    struct rect p = page_rect(w);
    return (struct rect){p.x + 8, p.y + 8, p.w - 16, p.h - 46};
}
static struct rect button_at(const struct window *w, int slot) {
    struct rect p = page_rect(w);
    return (struct rect){p.x + p.w - 8 - (slot + 1) * 94, p.y + p.h - 32, 88, 24};
}

static void refresh(void) {
    static struct dosxc_task_info list[MAX_PROCS];
    int n = (int)dosxc_sys_task(DOSXC_TASK_LIST, list, MAX_PROCS, 0);
    if (n < 0) n = 0;
    if (n > MAX_PROCS) n = MAX_PROCS;
    dosxc_sys_task(DOSXC_TASK_STATS, &stats, 0, 0);
    unsigned long long elapsed = stats.now_ns - last_now;
    struct proc fresh[MAX_PROCS];
    for (int i = 0; i < n; ++i) {
        struct proc *p = &fresh[i];
        memset(p, 0, sizeof(*p));
        p->pid = list[i].pid;
        p->memory = list[i].memory;
        snprintf(p->full, sizeof(p->full), "%s", list[i].path);
        const char *slash = strrchr(list[i].path, '\\');
        snprintf(p->name, sizeof(p->name), "%s", slash ? slash + 1 : list[i].path);
        unsigned long long before = 0;
        for (int k = 0; k < proc_count; ++k)
            if (procs[k].pid == p->pid) before = procs[k].last_cpu;
        if (elapsed && last_now) p->percent = (int)((list[i].cpu_ns - before) * 100 / elapsed);
        if (p->percent > 100) p->percent = 100;
        p->last_cpu = list[i].cpu_ns;
    }
    memcpy(procs, fresh, sizeof(struct proc) * (size_t)n);
    proc_count = n;
    proc_scroll.total = n;
    if (elapsed && last_now) {
        unsigned long long idle = stats.idle_ns - last_idle;
        int busy = 100 - (int)(idle * 100 / elapsed);
        cpu_percent = busy < 0 ? 0 : busy > 100 ? 100 : busy;
        memmove(cpu_history, cpu_history + 1, HISTORY - 1);
        memmove(mem_history, mem_history + 1, HISTORY - 1);
        cpu_history[HISTORY - 1] = (unsigned char)cpu_percent;
        mem_history[HISTORY - 1] = (unsigned char)((stats.memory_total - stats.memory_free) * 100 / stats.memory_total);
        if (history_count < HISTORY) ++history_count;
    }
    last_now = stats.now_ns;
    last_idle = stats.idle_ns;
    struct gui_board *board = gui_board();
    app_count = board->window_count;
    int me = dosxc_sys_getpid(), kept = 0;
    for (int i = 0; i < app_count && i < GUI_WINDOWS; ++i) apps[kept++] = board->windows[i];
    (void)me;
    app_count = kept;
    app_scroll.total = kept;
}

static void paint_tabs(const struct window *w, const struct canvas *c) {
    struct rect p = page_rect(w);
    bevel(c, p.x, p.y, p.w, p.h, 0);
    for (int i = 0; i < 3; ++i) {
        struct rect r = tab_rect(i);
        fill(c, r.x, r.y, r.w, r.h + (i == tab ? 2 : 0), C_WINDOW);
        fill(c, r.x, r.y, r.w, 1, C_HIGHLIGHT);
        fill(c, r.x, r.y, 1, r.h, C_HIGHLIGHT);
        fill(c, r.x + r.w - 1, r.y, 1, r.h, C_SHADOW2);
        fill(c, r.x + r.w - 2, r.y + 1, 1, r.h - 1, C_SHADOW1);
        text(c, r.x + 10, r.y + 6, i == tab ? &font_bold : &font_regular, tabs[i], C_TEXT);
    }
}

static void list_frame(const struct canvas *c, struct rect r, const char *const headers[], const int *columns, int count) {
    bevel(c, r.x, r.y, r.w, r.h, 1);
    fill(c, r.x + 2, r.y + 2, r.w - 4, r.h - 4, C_BASE);
    for (int i = 0; i < count; ++i) {
        int x = r.x + 2 + columns[i], w = (i + 1 < count ? columns[i + 1] : r.w - 18) - columns[i];
        fill(c, x, r.y + 2, w, ROW, C_WINDOW);
        bevel(c, x, r.y + 2, w, ROW, 0);
        text(c, x + 5, r.y + 7, &font_regular, headers[i], C_TEXT);
    }
}

static void paint_apps(struct window *w, const struct canvas *c) {
    static const char *const headers[] = {"Task", "Status"};
    struct rect r = list_rect(w);
    static const int cols[2] = {0, 0};
    int columns[2] = {cols[0], r.w - 110};
    list_frame(c, r, headers, columns, 2);
    app_scroll.visible = (r.h - 22) / ROW;
    vscroll_clamp(&app_scroll);
    for (int i = 0; i < app_scroll.visible && app_scroll.top + i < app_count; ++i) {
        int at = app_scroll.top + i, y = r.y + 22 + i * ROW, on = at == selected_app;
        if (on) fill(c, r.x + 2, y, r.w - 18, ROW, C_SELECTION);
        text(c, r.x + 8, y + 4, &font_regular, apps[at].title, on ? 0xffffff : C_TEXT);
        text(c, r.x + r.w - 104, y + 4, &font_regular, "Running", on ? 0xffffff : C_TEXT);
    }
    vscroll_paint(c, &app_scroll, (struct rect){r.x + r.w - 16, r.y + 2, 14, r.h - 4});
    button(c, button_at(w, 2), "End Task", 0, 0);
    button(c, button_at(w, 1), "Switch To", 0, 0);
    button(c, button_at(w, 0), "New Task...", 0, 0);
}

static void paint_processes(struct window *w, const struct canvas *c) {
    static const char *const headers[] = {"Image Name", "PID", "CPU", "Mem Usage"};
    struct rect r = list_rect(w);
    int columns[4] = {0, r.w - 240, r.w - 170, r.w - 110};
    list_frame(c, r, headers, columns, 4);
    proc_scroll.visible = (r.h - 22) / ROW;
    vscroll_clamp(&proc_scroll);
    for (int i = 0; i < proc_scroll.visible && proc_scroll.top + i < proc_count; ++i) {
        const struct proc *p = &procs[proc_scroll.top + i];
        int y = r.y + 22 + i * ROW, on = proc_scroll.top + i == selected_proc;
        u32 color = on ? 0xffffff : C_TEXT;
        if (on) fill(c, r.x + 2, y, r.w - 18, ROW, C_SELECTION);
        char line[32];
        text(c, r.x + 8, y + 4, &font_regular, p->name, color);
        snprintf(line, sizeof(line), "%d", p->pid);
        text(c, r.x + 2 + columns[1] + 5, y + 4, &font_regular, line, color);
        snprintf(line, sizeof(line), "%02d", p->percent);
        text(c, r.x + 2 + columns[2] + 5, y + 4, &font_regular, line, color);
        snprintf(line, sizeof(line), "%llu K", p->memory / 1024);
        text(c, r.x + 2 + columns[3] + 5, y + 4, &font_regular, line, color);
    }
    vscroll_paint(c, &proc_scroll, (struct rect){r.x + r.w - 16, r.y + 2, 14, r.h - 4});
    button(c, button_at(w, 0), "End Process", 0, 0);
}

static void graph_box(const struct canvas *c, struct rect r, const char *title) {
    bevel(c, r.x, r.y, r.w, r.h, 0);
    fill(c, r.x + 8, r.y - 1, text_width(&font_regular, title) + 8, 3, C_WINDOW);
    text(c, r.x + 12, r.y - 5, &font_regular, title, C_TEXT);
}

static void meter(const struct canvas *c, struct rect r, const char *title, int percent, const char *label) {
    graph_box(c, r, title);
    struct rect in = {r.x + 8, r.y + 10, r.w - 16, r.h - 18};
    fill(c, in.x, in.y, in.w, in.h, 0);
    int segments = (in.h - 22) / 3, lit = segments * percent / 100;
    for (int i = 0; i < segments; ++i) {
        int y = in.y + in.h - 22 - i * 3;
        u32 color = i < lit ? 0x00ff00 : 0x005000;
        fill(c, in.x + 12, y, 18, 2, color);
        fill(c, in.x + 34, y, 18, 2, color);
    }
    text(c, in.x + (in.w - text_width(&font_regular, label)) / 2, in.y + in.h - 14, &font_regular, label, 0x00ff00);
}

static void history(const struct canvas *c, struct rect r, const char *title, const unsigned char *values) {
    graph_box(c, r, title);
    struct rect in = {r.x + 8, r.y + 10, r.w - 16, r.h - 18};
    fill(c, in.x, in.y, in.w, in.h, 0);
    for (int y = in.y + in.h; y > in.y; y -= 12) fill(c, in.x, y - 1, in.w, 1, 0x004000);
    for (int x = in.x + 11; x < in.x + in.w; x += 12) fill(c, x, in.y, 1, in.h, 0x004000);
    int points = history_count < HISTORY ? history_count : HISTORY;
    int step = in.w / HISTORY ? in.w / HISTORY : 1;
    for (int i = 1; i < points; ++i) {
        int a = values[HISTORY - points + i - 1], b = values[HISTORY - points + i];
        int x1 = in.x + in.w - (points - i) * step, x2 = x1 + step;
        int y1 = in.y + in.h - 1 - a * (in.h - 2) / 100, y2 = in.y + in.h - 1 - b * (in.h - 2) / 100;
        for (int x = x1; x <= x2 && x < in.x + in.w; ++x) {
            int y = y1 + (y2 - y1) * (x - x1) / step;
            if (x >= in.x) fill(c, x, y, 1, 2, 0x00ff00);
        }
    }
}

static void paint_performance(struct window *w, const struct canvas *c) {
    struct rect p = page_rect(w);
    int left = p.x + 10, top = p.y + 16, gw = 90, rw = p.w - gw - 30, h = (p.h - 80) / 2;
    char label[24];
    snprintf(label, sizeof(label), "%d %%", cpu_percent);
    meter(c, (struct rect){left, top, gw, h}, "CPU Usage", cpu_percent, label);
    history(c, (struct rect){left + gw + 10, top, rw, h}, "CPU Usage History", cpu_history);
    int mem = stats.memory_total ? (int)((stats.memory_total - stats.memory_free) * 100 / stats.memory_total) : 0;
    snprintf(label, sizeof(label), "%llu M", (unsigned long long)(stats.memory_total - stats.memory_free) >> 20);
    meter(c, (struct rect){left, top + h + 12, gw, h}, "Mem Usage", mem, label);
    history(c, (struct rect){left + gw + 10, top + h + 12, rw, h}, "Memory Usage History", mem_history);
    int y = top + 2 * h + 22;
    char line[64];
    snprintf(line, sizeof(line), "Totals:  Processes %d    Tasks %u", proc_count, stats.tasks);
    text(c, left, y + 8, &font_regular, line, C_TEXT);
    snprintf(line, sizeof(line), "Physical Memory (K):  Total %llu    Available %llu",
             (unsigned long long)stats.memory_total >> 10, (unsigned long long)stats.memory_free >> 10);
    text(c, left, y + 24, &font_regular, line, C_TEXT);
}

static void tm_paint(struct window *w, const struct canvas *c) {
    menubar_paint(c, &bar, w->content.w);
    paint_tabs(w, c);
    if (tab == 0) paint_apps(w, c);
    else if (tab == 1) paint_processes(w, c);
    else paint_performance(w, c);
    fill(c, 0, w->content.h - STATUS_H, w->content.w, 1, C_SHADOW1);
    fill(c, 0, w->content.h - STATUS_H + 1, w->content.w, 1, C_HIGHLIGHT);
    char line[80];
    snprintf(line, sizeof(line), "Processes: %d", proc_count);
    text(c, 8, w->content.h - STATUS_H + 6, &font_regular, line, C_TEXT);
    snprintf(line, sizeof(line), "CPU Usage: %d%%", cpu_percent);
    text(c, 130, w->content.h - STATUS_H + 6, &font_regular, line, C_TEXT);
    snprintf(line, sizeof(line), "Mem Usage: %lluK / %lluK", (unsigned long long)(stats.memory_total - stats.memory_free) >> 10, (unsigned long long)stats.memory_total >> 10);
    text(c, 260, w->content.h - STATUS_H + 6, &font_regular, line, C_TEXT);
}

static void new_task_chosen(const char *path, void *data) { (void)data; if (path) gui_open_file(path); }

static void end_process(struct window *w, int pid, const char *name) {
    if (dosxc_sys_task(DOSXC_TASK_KILL, 0, pid, 0) < 0) {
        char line[80];
        snprintf(line, sizeof(line), "%s is needed by the system.", name);
        message_box("Task Manager", "This process cannot be ended.", line);
        return;
    }
    refresh();
    window_paint(w);
}

static void tm_mouse(struct window *w, int x, int y, int kind, int value) {
    if (menubar_mouse(w, &bar, x, y, kind)) return;
    struct rect r = list_rect(w);
    struct rect track = {r.x + r.w - 16, r.y + 2, 14, r.h - 4};
    struct vscroll *s = tab == 0 ? &app_scroll : &proc_scroll;
    if (tab < 2 && vscroll_mouse(s, track, x, y, kind, value)) { window_paint(w); return; }
    if (kind == M_WHEEL && tab < 2) { s->top -= value * 2; vscroll_clamp(s); window_paint(w); return; }
    if (kind != M_DOWN && kind != M_DOUBLE) return;
    for (int i = 0; i < 3; ++i)
        if (rect_has(tab_rect(i), x, y)) { tab = i; window_paint(w); return; }
    if (tab == 0) {
        if (rect_has(button_at(w, 2), x, y)) {
            if (selected_app >= 0 && selected_app < app_count) end_process(w, apps[selected_app].pid, apps[selected_app].title);
            return;
        }
        if (rect_has(button_at(w, 1), x, y)) {
            if (selected_app >= 0 && selected_app < app_count) gui_activate(&apps[selected_app]);
            return;
        }
        if (rect_has(button_at(w, 0), x, y)) { file_dialog("New Task", 0, 0, new_task_chosen, 0); return; }
        if (rect_has(r, x, y) && y >= r.y + 22) {
            int at = app_scroll.top + (y - r.y - 22) / ROW;
            selected_app = at < app_count ? at : -1;
            window_paint(w);
            if (kind == M_DOUBLE && selected_app >= 0) gui_activate(&apps[selected_app]);
        }
    } else if (tab == 1) {
        if (rect_has(button_at(w, 0), x, y)) {
            if (selected_proc >= 0 && selected_proc < proc_count) end_process(w, procs[selected_proc].pid, procs[selected_proc].name);
            return;
        }
        if (rect_has(r, x, y) && y >= r.y + 22) {
            int at = proc_scroll.top + (y - r.y - 22) / ROW;
            selected_proc = at < proc_count ? at : -1;
            window_paint(w);
        }
    }
}

static void tm_popup(struct window *w, int tag, int index) {
    if (index < 0) return;
    if (tag == 0 && index == 0) file_dialog("New Task", 0, 0, new_task_chosen, 0);
    else if (tag == 0 && index == 2) window_close(w);
    else if (tag == 1) { refresh(); window_paint(w); }
    else if (tag == 2) message_box("About Task Manager", "DOS-XC Task Manager", "Shows and ends running programs.");
}

static void tm_tick(struct window *w) {
    static int counter;
    if (++counter < 10) return;
    counter = 0;
    refresh();
    window_paint(w);
}

static const struct app_ops tm_ops = {tm_paint, tm_mouse, 0, 0, tm_popup, tm_tick};

int main(void) {
    if (gui_connect()) return 1;
    refresh();
    main_w = window_open(480, 420, "Task Manager", ICON_TASKS, &tm_ops, 0);
    if (!main_w) return 1;
    main_w->resizable = 1;
    gui_run();
    return 0;
}
