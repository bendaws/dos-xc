/*
 * DOS-XC /desktop/icons.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "wm.h"
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_ITEMS 128
#define CELL_W 76
#define CELL_H 68
#define MARGIN 8

enum { KIND_COMPUTER, KIND_FOLDER, KIND_FILE, KIND_SHORTCUT };

struct item {
    char name[96];                   /* as shown */
    char file[96];                   /* in the Desktop folder; "" for Computer */
    int kind, icon;                  /* icon: a built-in one, or -1 for custom */
    struct icon custom;
    struct rect r;                   /* the cell, in desktop coordinates */
};

static struct item items[MAX_ITEMS];
static int item_count, selected = -1;
static char folder[260];
static u32 listing_hash;
static long long next_poll;

static void item_path(const struct item *it, char *out, size_t size) {
    snprintf(out, size, "%s\\%s", folder, it->file);
}

static int ends_with(const char *s, const char *tail) {
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && !strcasecmp(s + a - b, tail);
}

static void shortcut_icon(struct item *it, const char *path) {
    struct shortcut s;
    it->icon = ICON_PROGRAM;
    if (shortcut_read(path, &s)) return;
    if (s.icon[0]) {
        int named = icon_by_name(s.icon);
        if (named >= 0) { it->icon = named; return; }
        if (!icon_read_png(s.icon, &it->custom)) { it->icon = -1; return; }
    }
    char png[300];
    snprintf(png, sizeof(png), "%s", s.target);
    char *dot = strrchr(png, '.');
    if (dot && !strchr(dot, '\\')) {
        strcpy(dot, ".PNG");
        if (!icon_read_png(png, &it->custom)) { it->icon = -1; return; }
    }
    struct dosxc_dirent probe[1];
    if (dosxc_sys_listdir(s.target, probe, 1) >= 0) it->icon = ICON_FOLDER;
    else if (!ends_with(s.target, ".EXE") && !ends_with(s.target, ".COM") && !ends_with(s.target, ".BAT"))
        it->icon = image_is_picture(s.target) ? ICON_PICTURE : ICON_FILE;
}

static int order(const void *a, const void *b) {
    const struct item *x = a, *y = b;
    if (x->kind != y->kind) return x->kind - y->kind;
    return strcasecmp(x->name, y->name);
}

static void layout(void) {
    struct rect area = desktop_area();
    int rows = (area.h - 2 * MARGIN) / CELL_H;
    if (rows < 1) rows = 1;
    for (int i = 0; i < item_count; ++i)
        items[i].r = (struct rect){area.x + MARGIN + i / rows * CELL_W, area.y + MARGIN + i % rows * CELL_H, CELL_W, CELL_H};
}

static void damage_items(void) {
    struct rect area = desktop_area();
    desktop_damage(area);
}

void icons_reload(void) {
    home_path(folder, sizeof(folder), "Desktop");
    mkdir(folder, 0777);
    char keep[96] = "";
    if (selected >= 0 && selected < item_count) snprintf(keep, sizeof(keep), "%s", items[selected].file);
    item_count = 0;
    struct item *c = &items[item_count++];
    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "Computer");
    c->kind = KIND_COMPUTER;
    c->icon = ICON_COMPUTER;
    static struct dosxc_dirent entries[MAX_ITEMS];
    long n = dosxc_sys_listdir(folder, entries, MAX_ITEMS);
    u32 hash = 2166136261u;
    for (long i = 0; i < n && item_count < MAX_ITEMS; ++i) {
        const struct dosxc_dirent *e = &entries[i];
        if (!strcmp(e->name, ".") || !strcmp(e->name, "..") || (e->attributes & DOSXC_ATTR_HIDDEN)) continue;
        for (const char *p = e->name; *p; ++p) hash = (hash ^ (unsigned char)*p) * 16777619u;
        hash = (hash ^ (u32)e->size) * 16777619u;
        struct item *it = &items[item_count++];
        memset(it, 0, sizeof(*it));
        snprintf(it->file, sizeof(it->file), "%s", e->name);
        snprintf(it->name, sizeof(it->name), "%s", e->name);
        char path[400];
        item_path(it, path, sizeof(path));
        if (e->attributes & DOSXC_ATTR_DIRECTORY) {
            it->kind = KIND_FOLDER;
            it->icon = ICON_FOLDER;
        } else if (ends_with(e->name, ".LNK")) {
            it->kind = KIND_SHORTCUT;
            it->name[strlen(it->name) - 4] = 0;
            shortcut_icon(it, path);
        } else {
            it->kind = KIND_FILE;
            it->icon = image_is_picture(e->name) ? ICON_PICTURE
                     : ends_with(e->name, ".EXE") || ends_with(e->name, ".COM") || ends_with(e->name, ".BAT") ? ICON_PROGRAM : ICON_FILE;
        }
    }
    listing_hash = hash;
    qsort(items + 1, (size_t)(item_count - 1), sizeof(*items), order);
    selected = -1;
    for (int i = 1; i < item_count && keep[0]; ++i)
        if (!strcasecmp(items[i].file, keep)) selected = i;
    layout();
    damage_items();
}

void icons_poll(void) {
    long long now = (long long)dosxc_sys_nanoseconds();
    if (now < next_poll) return;
    next_poll = now + 2000000000ll;
    static struct dosxc_dirent entries[MAX_ITEMS];
    long n = dosxc_sys_listdir(folder, entries, MAX_ITEMS);
    u32 hash = 2166136261u;
    for (long i = 0; i < n; ++i) {
        const struct dosxc_dirent *e = &entries[i];
        if (!strcmp(e->name, ".") || !strcmp(e->name, "..") || (e->attributes & DOSXC_ATTR_HIDDEN)) continue;
        for (const char *p = e->name; *p; ++p) hash = (hash ^ (unsigned char)*p) * 16777619u;
        hash = (hash ^ (u32)e->size) * 16777619u;
    }
    if (hash != listing_hash) icons_reload();
}

void icons_layout(void) { layout(); }

static void label_text(const struct item *it, char *out, size_t size) {
    snprintf(out, size, "%s", it->name);
    if (text_width(&font_regular, out) <= CELL_W - 6) return;
    size_t n = strlen(out);
    while (n > 1) {
        out[--n] = 0;
        char trial[100];
        snprintf(trial, sizeof(trial), "%s...", out);
        if (text_width(&font_regular, trial) <= CELL_W - 6) { snprintf(out, size, "%s", trial); return; }
    }
}

void icons_paint(const struct canvas *c) {
    for (int i = 0; i < item_count; ++i) {
        const struct item *it = &items[i];
        if (!rect_meets(it->r, c->clip)) continue;
        int ix = it->r.x + (CELL_W - 32) / 2, iy = it->r.y + 4;
        if (it->icon >= 0) draw_icon_big(c, ix, iy, &icons[it->icon]);
        else draw_icon_big(c, ix, iy, &it->custom);
        char label[100];
        label_text(it, label, sizeof(label));
        int tw = text_width(&font_regular, label), tx = it->r.x + (CELL_W - tw) / 2, ty = iy + 38;
        if (i == selected) {
            fill(c, tx - 2, ty - 2, tw + 4, font_regular.height + 4, C_SELECTION);
            text(c, tx, ty, &font_regular, label, 0xffffff);
        } else {
            text(c, tx + 1, ty + 1, &font_regular, label, 0x000000);
            text(c, tx, ty, &font_regular, label, 0xffffff);
        }
    }
}

int icons_at(int x, int y) {
    for (int i = 0; i < item_count; ++i) {
        struct rect r = items[i].r;
        struct rect hot = {r.x + 4, r.y + 2, r.w - 8, r.h - 8};
        if (rect_has(hot, x, y)) return i;
    }
    return -1;
}

void icons_select(int index) {
    if (index == selected) return;
    if (selected >= 0 && selected < item_count) desktop_damage(items[selected].r);
    selected = index;
    if (selected >= 0) desktop_damage(items[selected].r);
}

int icons_selected(void) { return selected; }
int icons_selected_is_item(void) { return selected > 0 && selected < item_count; }

void icons_open(int index) {
    if (index < 0 || index >= item_count) return;
    struct item *it = &items[index];
    if (it->kind == KIND_COMPUTER) {
        char app[260];
        desktop_path(app, sizeof(app), "Apps\\EXPLORER.EXE");
        char *argv[2] = {app, 0};
        dosxc_sys_task(DOSXC_TASK_START, app, (long)argv, 0);
        return;
    }
    char path[400];
    item_path(it, path, sizeof(path));
    if (open_path(path)) message_box("Cannot open", it->name, "No program could open it.");
}

void icons_open_selected(void) { icons_open(selected); }

static void unique_name(const char *base, const char *extension, char *out, size_t size) {
    char path[400];
    for (int n = 1; n < 100; ++n) {
        if (n == 1) snprintf(out, size, "%s%s", base, extension);
        else snprintf(out, size, "%s (%d)%s", base, n, extension);
        snprintf(path, sizeof(path), "%s\\%s", folder, out);
        struct stat st;
        if (stat(path, &st)) return;
    }
}

static void created(const char *name) {
    icons_reload();
    for (int i = 1; i < item_count; ++i)
        if (!strcasecmp(items[i].file, name)) icons_select(i);
}

static void new_file_named(const char *name, void *data) {
    (void)data;
    if (!name) return;
    char path[400];
    snprintf(path, sizeof(path), "%s\\%s", folder, name);
    struct stat st;
    if (!stat(path, &st)) { message_box("New File", "Something with that name is already there.", name); return; }
    FILE *f = fopen(path, "w");
    if (!f || fclose(f)) { message_box("New File", "The file could not be made.", name); return; }
    created(name);
}

void icons_new_file(void) {
    char name[96];
    unique_name("New Text Document", ".TXT", name, sizeof(name));
    input_box("New File", "Name of the new file:", name, new_file_named, 0);
}

static void new_folder_named(const char *name, void *data) {
    (void)data;
    if (!name) return;
    char path[400];
    snprintf(path, sizeof(path), "%s\\%s", folder, name);
    if (mkdir(path, 0777)) { message_box("New Folder", "The folder could not be made.", name); return; }
    created(name);
}

void icons_new_folder(void) {
    char name[96];
    unique_name("New Folder", "", name, sizeof(name));
    input_box("New Folder", "Name of the new folder:", name, new_folder_named, 0);
}

int icons_add_shortcut(const char *label, const struct shortcut *s) {
    if (!folder[0]) home_path(folder, sizeof(folder), "Desktop");
    char name[96], path[400];
    unique_name(label, ".LNK", name, sizeof(name));
    snprintf(path, sizeof(path), "%s\\%s", folder, name);
    if (shortcut_write(path, s)) return -1;
    created(name);
    return 0;
}

static void renamed(const char *name, void *data) {
    (void)data;
    if (!name || !icons_selected_is_item()) return;
    struct item *it = &items[selected];
    char from[400], to[400], file[100];
    snprintf(file, sizeof(file), "%s%s", name, it->kind == KIND_SHORTCUT ? ".LNK" : "");
    item_path(it, from, sizeof(from));
    snprintf(to, sizeof(to), "%s\\%s", folder, file);
    if (rename(from, to)) { message_box("Rename", "It could not be renamed.", name); return; }
    created(file);
}

void icons_rename(void) {
    if (!icons_selected_is_item()) return;
    input_box("Rename", "New name:", items[selected].name, renamed, 0);
}

static int remove_tree(const char *path) {
    struct dosxc_dirent entries[64];
    long n = dosxc_sys_listdir(path, entries, 64);
    if (n < 0) return remove(path);
    for (long i = 0; i < n; ++i) {
        if (!strcmp(entries[i].name, ".") || !strcmp(entries[i].name, "..")) continue;
        char child[400];
        snprintf(child, sizeof(child), "%s\\%s", path, entries[i].name);
        if (remove_tree(child)) return -1;
        if (n == 64) return remove_tree(path);
    }
    return rmdir(path);
}

static void delete_confirmed(int ok, void *data) {
    (void)data;
    if (!ok || !icons_selected_is_item()) return;
    char path[400];
    item_path(&items[selected], path, sizeof(path));
    if (remove_tree(path)) message_box("Delete", "It could not be deleted.", items[selected].name);
    selected = -1;
    icons_reload();
}

void icons_delete(void) {
    if (!icons_selected_is_item()) return;
    char question[160];
    snprintf(question, sizeof(question), "Delete \"%s\"?", items[selected].name);
    confirm_box("Delete", question, "Delete", ICON_FILE, delete_confirmed, 0);
}

void icons_key(int key) {
    if (key == '\n') icons_open_selected();
    else if (key == 0x108) icons_delete();
    else if (key == 0x10c) icons_rename();
    else if (key == 0x101 || key == 0x102) {
        int next = selected + (key == 0x102 ? 1 : -1);
        if (next >= 0 && next < item_count) icons_select(next);
    }
}

/* ---- New Shortcut: a program list, or any file or folder --------------- */

static const struct { const char *label, *app, *icon; } builtin_choices[] = {
    {"File Explorer", "EXPLORER.EXE", "folder"}, {"File Editor", "EDITOR.EXE", "editor"},
    {"Calculator", "CALC.EXE", "calc"}, {"Paint", "PAINT.EXE", "paint"},
    {"Task Manager", "TASKMGR.EXE", "tasks"}, {"DOS Prompt", "TERMINAL.EXE", "prompt"},
    {"Package Manager", "PKGMGR.EXE", "package"}, {"Solitaire", "SOLITAIRE.EXE", "cards"},
    {"Minesweeper", "MINES.EXE", "mines"}, {"Chess", "CHESS.EXE", "chess"},
};
#define MAX_CHOICES 40
#define ROW 20
#define VISIBLE 14
struct pick_item { char label[64], target[260], icon[200]; int icon_index, terminal; };
struct picker { struct pick_item items[MAX_CHOICES]; int count, selected, top; };
static const struct rect pick_list = {10, 30, 300, VISIBLE * ROW}, pick_ok = {148, 40 + VISIBLE * ROW, 76, 24},
                         pick_cancel = {232, 40 + VISIBLE * ROW, 76, 24}, pick_browse = {10, 40 + VISIBLE * ROW, 96, 24};

static void pick_paint(struct window *w, const struct canvas *c) {
    struct picker *p = w->data;
    text(c, 10, 10, &font_regular, "Put a shortcut to this program on the desktop:", C_TEXT);
    bevel(c, pick_list.x - 2, pick_list.y - 2, pick_list.w + 4, pick_list.h + 4, 1);
    fill(c, pick_list.x, pick_list.y, pick_list.w, pick_list.h, C_BASE);
    for (int i = p->top; i < p->count && i < p->top + VISIBLE; ++i) {
        const struct pick_item *it = &p->items[i];
        int y = pick_list.y + (i - p->top) * ROW;
        if (i == p->selected) fill(c, pick_list.x, y, pick_list.w, ROW, C_SELECTION);
        draw_icon(c, pick_list.x + 3, y + 2, it->icon_index);
        text(c, pick_list.x + 24, y + 5, &font_regular, it->label, i == p->selected ? 0xffffff : C_TEXT);
    }
    button(c, pick_browse, "Browse...", 0, 0);
    button(c, pick_ok, "OK", 0, 1);
    button(c, pick_cancel, "Cancel", 0, 0);
}

static void pick_accept(struct window *w) {
    struct picker *p = w->data;
    if (p->selected < 0 || p->selected >= p->count) return;
    const struct pick_item *it = &p->items[p->selected];
    struct shortcut s;
    memset(&s, 0, sizeof(s));
    snprintf(s.icon, sizeof(s.icon), "%s", it->icon);
    snprintf(s.target, sizeof(s.target), "%s", it->target);
    s.terminal = it->terminal;
    char label[64];
    snprintf(label, sizeof(label), "%s", it->label);
    window_close(w);
    if (icons_add_shortcut(label, &s)) message_box("New Shortcut", "The shortcut could not be made.", label);
}

static void browsed(const char *path, void *data) {
    (void)data;
    if (!path) return;
    struct shortcut s;
    memset(&s, 0, sizeof(s));
    snprintf(s.target, sizeof(s.target), "%s", path);
    s.terminal = ends_with(path, ".EXE") || ends_with(path, ".COM") || ends_with(path, ".BAT");
    const char *name = strrchr(path, '\\');
    char label[96];
    snprintf(label, sizeof(label), "%s", name ? name + 1 : path);
    char *dot = strrchr(label, '.');
    if (dot && dot != label) *dot = 0;
    if (icons_add_shortcut(label, &s)) message_box("New Shortcut", "The shortcut could not be made.", label);
}

static void pick_mouse(struct window *w, int x, int y, int kind, int value) {
    (void)value;
    struct picker *p = w->data;
    if (kind != M_DOWN && kind != M_DOUBLE) return;
    if (rect_has(pick_ok, x, y)) { pick_accept(w); return; }
    if (rect_has(pick_cancel, x, y)) { window_close(w); return; }
    if (rect_has(pick_browse, x, y)) {
        window_close(w);
        file_dialog("Shortcut to", 0, 0, browsed, 0);
        return;
    }
    if (rect_has(pick_list, x, y)) {
        int at = p->top + (y - pick_list.y) / ROW;
        if (at < p->count) {
            p->selected = at;
            window_paint(w);
            if (kind == M_DOUBLE) pick_accept(w);
        }
    }
}

static void pick_key(struct window *w, int key) {
    struct picker *p = w->data;
    if (key == '\n') pick_accept(w);
    else if (key == 27) window_close(w);
    else if (key == 0x101 && p->selected > 0) { if (--p->selected < p->top) p->top = p->selected; window_paint(w); }
    else if (key == 0x102 && p->selected + 1 < p->count) { if (++p->selected >= p->top + VISIBLE) ++p->top; window_paint(w); }
}

static void pick_closed(struct window *w) { free(w->data); }
static const struct app_ops pick_ops = {pick_paint, pick_mouse, pick_key, pick_closed};

void icons_new_shortcut(void) {
    struct picker *p = calloc(1, sizeof(*p));
    if (!p) return;
    for (int i = 0; i < (int)(sizeof(builtin_choices) / sizeof(*builtin_choices)); ++i) {
        struct pick_item *it = &p->items[p->count++];
        char relative[64];
        snprintf(relative, sizeof(relative), "Apps\\%s", builtin_choices[i].app);
        desktop_path(it->target, sizeof(it->target), relative);
        snprintf(it->label, sizeof(it->label), "%s", builtin_choices[i].label);
        snprintf(it->icon, sizeof(it->icon), "%s", builtin_choices[i].icon);
        it->icon_index = icon_by_name(it->icon);
    }
    static struct app_entry apps[32];
    int n = apps_list(apps, 32);
    for (int i = 0; i < n && p->count < MAX_CHOICES; ++i) {
        if (apps[i].builtin) continue;
        struct pick_item *it = &p->items[p->count++];
        snprintf(it->label, sizeof(it->label), "%s", apps[i].name);
        snprintf(it->target, sizeof(it->target), "%s", apps[i].program);
        snprintf(it->icon, sizeof(it->icon), "%s", apps[i].icon_file);
        it->icon_index = apps[i].icon;
        it->terminal = apps[i].terminal;
    }
    struct window *w = window_open(320, 74 + VISIBLE * ROW, "New Shortcut", ICON_PROGRAM, &pick_ops, p);
    if (!w) free(p);
}
