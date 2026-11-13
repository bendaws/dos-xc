/*
 * DOS-XC /desktop/pkgmgr.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "wm.h"
#include "../os/pkg/dsp.h"
#include "../os/pkg/repo.h"
#include <strings.h>
#include <unistd.h>

#define MAX_ITEMS 128
#define ROW 20
#define LOG_LINES 400
#define LOG_WIDTH 100

struct item { struct dsp_info info; char file[300]; int state; };   /* state: 0 new, 1 same, 2 older installed */
static struct item installed[MAX_ITEMS], available[MAX_ITEMS], online[MAX_ITEMS];
static int installed_count, available_count, online_count, online_loaded, selected;
static int tab = 2; /* Open the online catalog by default. */
static char online_dir[300], online_file[340];
static struct vscroll scroll, log_scroll;
static struct window *win;

static char log_text[LOG_LINES][LOG_WIDTH];
static int log_count, log_column;
static int console = -1, pkg_pid;

static const struct rect tabs[3] = {{106, 8, 90, 24}, {200, 8, 90, 24}, {12, 8, 90, 24}};
static const struct rect act_refresh = {492, 8, 136, 24};
static const struct rect list = {12, 36, 616, 200}, log_area = {12, 324, 616, 100};
static const struct rect act_install = {12, 276, 110, 24}, act_update = {130, 276, 110, 24};
static const struct rect act_remove = {248, 276, 110, 24}, act_details = {366, 276, 110, 24};
static const struct rect act_from_file = {484, 276, 144, 24};

static const char *const details_label[] = {"Show Details", "Show Details"};

static int is_busy(void) { return console >= 0; }
static int item_count(void) { return tab == 2 ? online_count : tab ? available_count : installed_count; }
static const struct item *items(void) { return tab == 2 ? online : tab ? available : installed; }

static void log_append(char c) {
    if (c == '\n') {
        log_count = log_count + 1 < LOG_LINES ? log_count + 1 : log_count;
        if (log_count + 1 == LOG_LINES) {
            memmove(log_text[0], log_text[1], sizeof(log_text[0]) * (LOG_LINES - 1));
            --log_count;
        }
        memset(log_text[log_count], 0, LOG_WIDTH);
        log_column = 0;
        log_scroll.total = log_count + 1;
        log_scroll.top = log_scroll.total;
        vscroll_clamp(&log_scroll);
        return;
    }
    if (c == '\r') { log_column = 0; return; }
    if ((unsigned char)c < 32 || log_column >= LOG_WIDTH - 1) return;
    log_text[log_count][log_column++] = c;
    log_text[log_count][log_column] = 0;
}

static void log_write(const char *text) {
    for (; *text; ++text) log_append(*text);
    log_append('\n');
    window_paint(win);
}

static void load_installed(void) {
    struct dosxc_sysinfo info;
    dosxc_sys_sysinfo(&info);
    char programs[32];
    snprintf(programs, sizeof(programs), "%c:\\Programs", info.drive);
    struct dosxc_dirent *entries = malloc(sizeof(*entries) * 256);
    if (!entries) return;
    int n = (int)dosxc_sys_listdir(programs, entries, 256);
    installed_count = 0;
    for (int i = 0; i < n && i < 256 && installed_count < MAX_ITEMS; ++i) {
        if (!(entries[i].attributes & DOSXC_ATTR_DIRECTORY)) continue;
        char inf[600], text[2048];
        snprintf(inf, sizeof(inf), "%s\\%s\\PACKAGE.INF", programs, entries[i].name);
        FILE *f = fopen(inf, "rb");
        if (!f) continue;
        size_t got = fread(text, 1, sizeof(text), f);
        fclose(f);
        if (dsp_parse_info(text, got, &installed[installed_count].info)) continue;
        ++installed_count;
    }
    free(entries);
    char inf[32], text[2048];
    snprintf(inf, sizeof(inf), "%c:\\SYSTEM\\PACKAGE.INF", info.drive);
    FILE *f = installed_count < MAX_ITEMS ? fopen(inf, "rb") : 0;
    if (!f) return;
    size_t got = fread(text, 1, sizeof(text), f);
    fclose(f);
    if (!dsp_parse_info(text, got, &installed[installed_count].info)) ++installed_count;
}

static const struct item *installed_named(const char *name) {
    for (int i = 0; i < installed_count; ++i)
        if (!strcasecmp(installed[i].info.name, name)) return &installed[i];
    return 0;
}

static int order(const void *a, const void *b) { return strcasecmp(((const struct item *)a)->info.name, ((const struct item *)b)->info.name); }

static void load_available(void) {
    struct dosxc_sysinfo info;
    dosxc_sys_sysinfo(&info);
    available_count = 0;
    struct dosxc_volume volumes[26];
    int n = dosxc_sys_volumes(volumes, 26);
    struct dosxc_dirent *entries = malloc(sizeof(*entries) * 128);
    if (!entries) return;
    for (int v = 0; v < n && v < 26; ++v) {
        char folder[16];
        snprintf(folder, sizeof(folder), "%c:\\Packages", volumes[v].letter);
        int count = (int)dosxc_sys_listdir(folder, entries, 128);
        for (int i = 0; i < count && i < 128 && available_count < MAX_ITEMS; ++i) {
            size_t length = strlen(entries[i].name);
            if (length < 5 || strcasecmp(entries[i].name + length - 4, ".DSP")) continue;
            struct item *a = &available[available_count];
            snprintf(a->file, sizeof(a->file), "%s\\%s", folder, entries[i].name);
            char error[100];
            if (dsp_read_info(a->file, &a->info, error, sizeof(error))) continue;
            if (a->info.arch != 16 && a->info.arch != (int)info.bits) continue;
            const struct item *old = installed_named(a->info.name);
            a->state = !old ? 0 : dsp_compare_versions(a->info.version, old->info.version) > 0 ? 2 : 1;
            ++available_count;
        }
    }
    free(entries);
    qsort(available, (size_t)available_count, sizeof(available[0]), order);
    qsort(installed, (size_t)installed_count, sizeof(installed[0]), order);
}

static void refresh(void) {
    load_installed();
    load_available();
    if (online_file[0]) {
        struct repo_entry *entries=malloc(REPO_MAX_PACKAGES*sizeof(*entries));
        int next=0,n=entries ? repo_read_index(online_file,entries,MAX_ITEMS,&next) : -1;
        online_count=n<0 ? 0 : n;
        for (int i=0;i<online_count;++i) {
            online[i].info=entries[i].info;
            const struct item *old=installed_named(online[i].info.name);
            online[i].state=!old ? 0 : dsp_compare_versions(online[i].info.version,old->info.version)>0 ? 2 : 1;
        }
        free(entries);
        qsort(online,(size_t)online_count,sizeof(online[0]),order);
    }
    int total = item_count();
    if (selected >= total) selected = total - 1;
    scroll.total = total;
    scroll.visible = list.h / ROW;
    vscroll_clamp(&scroll);
}

/* Run PKG in a console; its output appears below the list. */
static void run_pkg(const char *verb, const char *argument, int yes, int force) {
    if (is_busy()) return;
    struct dosxc_sysinfo info;
    dosxc_sys_sysinfo(&info);
    char path[32], line[320];
    snprintf(path, sizeof(path), "%c:\\Programs\\pkg\\PKG.EXE", info.drive);
    snprintf(line, sizeof(line), "> PKG %s %s", verb, argument ? argument : "");
    log_write(line);
    console = (int)dosxc_sys_task(DOSXC_TASK_CONSOLE_OPEN, 0, 0, 0);
    if (console < 0) { log_write("Could not open a console for PKG."); console = -1; return; }
    char *argv[6] = {"PKG.EXE", (char *)verb, (char *)argument, yes ? "/Y" : 0, 0, 0};
    if (force) argv[yes ? 4 : 3] = "/F";
    long pid = dosxc_sys_task(DOSXC_TASK_START, path, (long)argv, console + 1);
    if (pid < 0) {
        log_write("PKG could not be started.");
        dosxc_sys_task(DOSXC_TASK_CONSOLE_CLOSE, 0, console, 0);
        console = -1;
        return;
    }
    pkg_pid = (int)pid;
    window_paint(win);
}

static void refresh_online(void) {
    if (is_busy()) return;
    if (!online_dir[0]) {
        const char *temp=getenv("TMPDIR");
        if (!temp || !*temp) temp="\\TEMP";
        if (snprintf(online_dir,sizeof(online_dir),"%s/pkgmgr-XXXXXX",temp)>=(int)sizeof(online_dir) || !mkdtemp(online_dir)) {
            online_dir[0]=0; log_write("Cannot create a temporary catalog directory."); return;
        }
        snprintf(online_file,sizeof(online_file),"%s/catalog.inf",online_dir);
    }
    remove(online_file);
    online_count=0; selected=0; scroll.top=0; scroll.total=0;
    online_loaded=1;
    run_pkg("CATALOG",online_file,0,0);
}

static int pkg_poll(void) {
    if (console < 0) return 0;
    static unsigned char buffer[2048];
    int any = 0, n;
    while ((n = (int)dosxc_sys_task(DOSXC_TASK_CONSOLE_READ, buffer, console, sizeof(buffer))) > 0) {
        for (int i = 0; i < n; ++i) if (buffer[i] != 27) log_append((char)buffer[i]);
        any = 1;
    }
    if (dosxc_sys_task(DOSXC_TASK_ALIVE, 0, pkg_pid, 0) <= 0) {
        dosxc_sys_task(DOSXC_TASK_CONSOLE_CLOSE, 0, console, 0);
        console = -1;
        refresh();
        any = 1;
    }
    if (any) window_paint(win);
    return any;
}

static void paint_list(const struct canvas *c) {
    bevel(c, list.x - 2, list.y - 2, list.w + 4, list.h + 4, 1);
    fill(c, list.x, list.y, list.w, list.h, C_BASE);
    text(c, list.x + 8, list.y + 2, &font_bold, "Name", C_TEXT);
    text(c, list.x + 170, list.y + 2, &font_bold, "Version", C_TEXT);
    text(c, list.x + 245, list.y + 2, &font_bold, tab ? "Status" : "Architecture", C_TEXT);
    text(c, list.x + 330, list.y + 2, &font_bold, "Description", C_TEXT);
    fill(c, list.x, list.y + 16, list.w - 14, 1, C_SHADOW1);
    struct canvas clip = *c;
    clip.clip = rect_both(c->clip, (struct rect){list.x, list.y + 18, list.w - 14, list.h - 18});
    const struct item *entries = items();
    int count = item_count();
    scroll.total = count;
    scroll.visible = (list.h - 18) / ROW;
    vscroll_clamp(&scroll);
    for (int i = 0; i < scroll.visible && scroll.top + i < count; ++i) {
        const struct item *it = &entries[scroll.top + i];
        int y = list.y + 18 + i * ROW, on = scroll.top + i == selected;
        if (on) fill(&clip, list.x, y, list.w - 14, ROW, C_SELECTION);
        u32 color = on ? 0xffffff : C_TEXT;
        draw_icon(&clip, list.x + 4, y + 2, ICON_PACKAGE);
        text(&clip, list.x + 26, y + 5, &font_bold, it->info.name, color);
        text(&clip, list.x + 170, y + 5, &font_regular, it->info.version, color);
        const char *middle = !tab ? (it->info.arch == 16 ? "16-bit DOS" : it->info.arch == 32 ? "32-bit" : "64-bit")
                                  : it->state == 2 ? "Update ready" : it->state == 1 ? "Installed" : "Not installed";
        text(&clip, list.x + 245, y + 5, &font_regular, middle, color);
        text(&clip, list.x + 330, y + 5, &font_regular, it->info.description, color);
    }
    vscroll_paint(c, &scroll, (struct rect){list.x + list.w - 14, list.y, 14, list.h});
}

static void action_button(const struct canvas *c, struct rect r, const char *label, int enabled) {
    fill(c, r.x, r.y, r.w, r.h, C_WINDOW);
    bevel(c, r.x, r.y, r.w, r.h, 0);
    text(c, r.x + (r.w - text_width(&font_regular, label)) / 2, r.y + (r.h - font_regular.height) / 2, &font_regular, label, enabled ? C_TEXT : C_DISABLED);
}

static const struct item *selected_item(void) {
    int count = item_count();
    if (selected < 0 || selected >= count) return 0;
    return &items()[selected];
}

static void pm_paint(struct window *w, const struct canvas *c) {
    for (int i = 0; i < 3; ++i) {
        static const char *const names[3] = {"Installed", "Local", "Online"};
        button(c, tabs[i], names[i], tab == i, tab == i);
    }
    if (tab==2) action_button(c,act_refresh,"Refresh",!is_busy());
    paint_list(c);
    const struct item *it = selected_item();
    char line[300] = "";
    if (it) snprintf(line, sizeof(line), "%s %s%s%s", it->info.name, it->info.version, it->info.author[0] ? "  by " : "", it->info.author);
    text(c, 14, 254, &font_regular, line, C_TEXT);

    int installable = tab && it && it->state != 1, updatable = tab && it && it->state >= 1, removable = it && !tab;
    action_button(c, act_install, "Install", installable && !it->state);
    action_button(c, act_update, tab && it && it->state == 1 ? "Reinstall" : "Update", updatable);
    action_button(c, act_remove, "Remove", removable);
    action_button(c, act_details, "Details", it != 0);
    action_button(c, act_from_file, "Install from File...", 1);

    bevel(c, log_area.x - 2, log_area.y - 2, log_area.w + 4, log_area.h + 4, 1);
    fill(c, log_area.x, log_area.y, log_area.w, log_area.h, 0);
    struct canvas clip = *c;
    clip.clip = rect_both(c->clip, (struct rect){log_area.x, log_area.y, log_area.w - 14, log_area.h});
    int rows = log_area.h / 12;
    log_scroll.total = log_count + 1;
    log_scroll.visible = rows;
    if (log_scroll.top > log_scroll.total) log_scroll.top = log_scroll.total;
    vscroll_clamp(&log_scroll);
    for (int i = 0; i < rows && log_scroll.top + i <= log_count; ++i)
        text(&clip, log_area.x + 4, log_area.y + 2 + i * 12, &font_mono, log_text[log_scroll.top + i], 0xd0d0d0);
    vscroll_paint(c, &log_scroll, (struct rect){log_area.x + log_area.w - 14, log_area.y, 14, log_area.h});
    char status[40];
    snprintf(status, sizeof(status), is_busy() ? "Working..." : "Output");
    text(c, 14, 311, &font_regular, status, C_DISABLED);
    (void)w;
}

static void removed(int answer, void *data) {
    (void)data;
    if (answer == 1) {
        const struct item *it = selected_item();
        if (it) run_pkg("REMOVE", it->info.name, 1, 0);
    }
}

static void installed_file(const char *path, void *data) {
    (void)data;
    if (path) run_pkg("INSTALL", path, 1, 0);
}

static void pm_mouse(struct window *w, int x, int y, int kind, int value) {
    struct rect bar = {list.x + list.w - 14, list.y, 14, list.h};
    struct rect logbar = {log_area.x + log_area.w - 14, log_area.y, 14, log_area.h};
    if (vscroll_mouse(&scroll, bar, x, y, kind, value) || vscroll_mouse(&log_scroll, logbar, x, y, kind, value)) { window_paint(w); return; }
    if (kind == M_WHEEL) {
        if (rect_has(log_area, x, y)) { log_scroll.top -= value * 3; vscroll_clamp(&log_scroll); }
        else { scroll.top -= value * 2; vscroll_clamp(&scroll); }
        window_paint(w);
        return;
    }
    if (kind != M_DOWN && kind != M_DOUBLE) return;
    for (int i = 0; i < 3; ++i)
        if (rect_has(tabs[i], x, y)) {
            tab = i; selected = 0; scroll.top = 0; scroll.total=item_count();
            if (tab==2 && !online_loaded) refresh_online();
            window_paint(w); return;
        }
    if (rect_has(list, x, y) && y >= list.y + 18) {
        int at = scroll.top + (y - list.y - 18) / ROW;
        if (at < item_count()) selected = at;
        window_paint(w);
        return;
    }
    if (is_busy()) return;
    if (tab==2 && rect_has(act_refresh,x,y)) { refresh_online(); return; }
    const struct item *it = selected_item();
    if (rect_has(act_from_file, x, y)) { file_dialog("Install Package", 0, 0, installed_file, 0); return; }
    if (!it) return;
    if (rect_has(act_details, x, y)) { run_pkg(tab==2 ? "SHOW" : "INFO",tab==1 ? it->file : it->info.name,0,0); return; }
    if (rect_has(act_install, x, y) && tab && !it->state) { run_pkg("INSTALL",tab==2 ? it->info.name : it->file,1,0); return; }
    if (rect_has(act_update, x, y) && tab && it->state >= 1) { run_pkg("UPDATE",tab==2 ? it->info.name : it->file,1,it->state==1); return; }
    if (rect_has(act_remove, x, y) && !tab) {
        char question[160];
        if (!strcasecmp(it->info.name, "DOS-XC Desktop"))
            snprintf(question, sizeof(question), "Remove the desktop?  DOS-XC will start at the prompt.");
        else if (!strcasecmp(it->info.name, "coreutils"))
            snprintf(question, sizeof(question), "coreutils provides DIR, COPY and more.  Remove it?");
        else snprintf(question, sizeof(question), "Remove %s %s?", it->info.name, it->info.version);
        ask_box("Remove Package", question, "Remove", "Keep", removed, 0);
    }
}

static void pm_key(struct window *w, int key) {
    int count = item_count();
    if (key == 0x101 && selected > 0) --selected;
    else if (key == 0x102 && selected + 1 < count) ++selected;
    else return;
    if (selected < scroll.top) scroll.top = selected;
    if (selected >= scroll.top + scroll.visible) scroll.top = selected - scroll.visible + 1;
    window_paint(w);
}

static int pm_closing(struct window *w) {
    (void)w;
    if (is_busy()) { log_write("Wait for the package operation to finish before closing."); return 0; }
    return 1;
}
static const struct app_ops pm_ops = {.paint=pm_paint,.mouse=pm_mouse,.key=pm_key,.closing=pm_closing};

int main(void) {
    if (gui_connect()) return 1;
    win = window_open(640, 436, "Package Manager", ICON_PACKAGE, &pm_ops, 0);
    if (!win) return 1;
    (void)details_label;
    refresh();
    gui_set_poll(pkg_poll);
    refresh_online();
    gui_run();
    if (online_file[0]) remove(online_file);
    if (online_dir[0]) rmdir(online_dir);
    return 0;
}
