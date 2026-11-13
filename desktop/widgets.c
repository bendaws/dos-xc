/*
 * DOS-XC /desktop/widgets.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "wm.h"
#include <strings.h>

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

void home_path(char *out, size_t size, const char *relative) {
    const char *home = getenv("HOME");
    struct dosxc_sysinfo info;
    dosxc_sys_sysinfo(&info);
    if (!home || !*home) home = "\\Users\\Public";
    char base[200];
    snprintf(base, sizeof(base), "%s", home);
    for (char *p = base; *p; ++p)
        if (*p == '/') *p = '\\';
    if (base[1] == ':') snprintf(out, size, "%s%s%s", base, *relative ? "\\" : "", relative);
    else snprintf(out, size, "%c:%s%s%s", info.drive, base, *relative ? "\\" : "", relative);
}

int config_get(const char *key, char *out, size_t size) {
    char path[300], line[320];
    home_path(path, sizeof(path), "DESKTOP.CFG");
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = strlen(key);
    int found = -1;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, key, n) && line[n] == '=') {
            snprintf(out, size, "%s", line + n + 1);
            found = 0;
        }
    }
    fclose(f);
    return found;
}

int shortcut_read(const char *path, struct shortcut *s) {
    char line[320];
    memset(s, 0, sizeof(*s));
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "target=", 7)) snprintf(s->target, sizeof(s->target), "%s", line + 7);
        else if (!strncmp(line, "args=", 5)) snprintf(s->args, sizeof(s->args), "%s", line + 5);
        else if (!strncmp(line, "icon=", 5)) snprintf(s->icon, sizeof(s->icon), "%s", line + 5);
        else if (!strncmp(line, "terminal=", 9)) s->terminal = atoi(line + 9);
    }
    fclose(f);
    return s->target[0] ? 0 : -1;
}

int shortcut_write(const char *path, const struct shortcut *s) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "target=%s\n", s->target);
    if (s->args[0]) fprintf(f, "args=%s\n", s->args);
    if (s->icon[0]) fprintf(f, "icon=%s\n", s->icon);
    if (s->terminal) fprintf(f, "terminal=1\n");
    return fclose(f) ? -1 : 0;
}

static int exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void system_file(char *out, size_t size, const char *path) {
    struct dosxc_sysinfo info;
    dosxc_sys_sysinfo(&info);
    snprintf(out, size, "%c:%s", info.drive, path);
}

/* ---- programs from package manifests ------------------------------------ */

static int icon_slots_used;
static char icon_slot_path[ICON_SLOTS - ICON_APP_FIRST][260];

static int icon_from(const char *name, const char *dir) {
    int named = icon_by_name(name);
    if (named >= 0) return named;
    char path[300];
    snprintf(path, sizeof(path), "%s\\%s", dir, name);
    for (int i = 0; i < icon_slots_used; ++i)
        if (!strcasecmp(icon_slot_path[i], path)) return ICON_APP_FIRST + i;
    if (icon_slots_used == ICON_SLOTS - ICON_APP_FIRST || icon_load_png(ICON_APP_FIRST + icon_slots_used, path)) return ICON_PROGRAM;
    snprintf(icon_slot_path[icon_slots_used], sizeof(icon_slot_path[0]), "%s", path);
    return ICON_APP_FIRST + icon_slots_used++;
}

int icon_for_png(const char *path) {
    for (int i = 0; i < icon_slots_used; ++i)
        if (!strcasecmp(icon_slot_path[i], path)) return ICON_APP_FIRST + i;
    if (icon_slots_used == ICON_SLOTS - ICON_APP_FIRST || icon_load_png(ICON_APP_FIRST + icon_slots_used, path)) return ICON_PROGRAM;
    snprintf(icon_slot_path[icon_slots_used], sizeof(icon_slot_path[0]), "%s", path);
    return ICON_APP_FIRST + icon_slots_used++;
}

static void finish_entry(struct app_entry *e, const char *dir, const char *icon) {
    snprintf(e->dir, sizeof(e->dir), "%s", dir);
    if (e->program[0] && e->program[1] != ':') {
        char full[300];
        snprintf(full, sizeof(full), "%s\\%s", dir, e->program);
        snprintf(e->program, sizeof(e->program), "%s", full);
    }
    e->icon = icon[0] ? icon_from(icon, dir) : ICON_PROGRAM;
    if (icon[0] && icon_by_name(icon) < 0) snprintf(e->icon_file, sizeof(e->icon_file), "%s\\%s", dir, icon);
    else snprintf(e->icon_file, sizeof(e->icon_file), "%s", icon);
    if (!e->category[0]) snprintf(e->category, sizeof(e->category), "Programs");
}

/* Every block of DIR\MENU.CFG ("key=value" lines; a blank line starts the next program). */
static int read_manifest(const char *dir, struct app_entry *out, int max) {
    char path[300], line[300], icon[260] = "";
    snprintf(path, sizeof(path), "%s\\MENU.CFG", dir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int n = 0, open = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') {
            if (!line[0] && open) { finish_entry(&out[n++], dir, icon); open = 0; icon[0] = 0; }
            continue;
        }
        if (!open) { if (n == max) break; memset(&out[n], 0, sizeof(out[n])); open = 1; }
        *eq = 0;
        const char *key = line, *value = eq + 1;
        struct app_entry *e = &out[n];
        if (!strcmp(key, "id")) snprintf(e->id, sizeof(e->id), "%s", value);
        else if (!strcmp(key, "name")) snprintf(e->name, sizeof(e->name), "%s", value);
        else if (!strcmp(key, "program")) snprintf(e->program, sizeof(e->program), "%s", value);
        else if (!strcmp(key, "icon")) snprintf(icon, sizeof(icon), "%s", value);
        else if (!strcmp(key, "category")) snprintf(e->category, sizeof(e->category), "%s", value);
        else if (!strcmp(key, "default")) snprintf(e->defaults, sizeof(e->defaults), "%s", value);
        else if (!strcmp(key, "types")) snprintf(e->types, sizeof(e->types), "%s", value);
        else if (!strcmp(key, "terminal")) e->terminal = atoi(value);
        else if (!strcmp(key, "url")) e->url = atoi(value);
    }
    if (open && n < max) finish_entry(&out[n++], dir, icon);
    fclose(f);
    int kept = 0;
    for (int i = 0; i < n; ++i)
        if (out[i].id[0] && out[i].name[0] && exists(out[i].program)) out[kept++] = out[i];
    return kept;
}

int apps_list(struct app_entry *out, int max) {
    int n = 0;
    if (max > 0) {
        memset(&out[0], 0, sizeof(out[0]));
        snprintf(out[0].id, sizeof(out[0].id), "editor");
        snprintf(out[0].name, sizeof(out[0].name), "File Editor");
        desktop_path(out[0].program, sizeof(out[0].program), "Apps\\EDITOR.EXE");
        snprintf(out[0].category, sizeof(out[0].category), "Utilities");
        snprintf(out[0].defaults, sizeof(out[0].defaults), "editor");
        snprintf(out[0].types, sizeof(out[0].types), "txt ini cfg bat c h cpp md log json xml");
        out[0].icon = ICON_EDITOR;
        out[0].builtin = 1;
        n = 1;
    }
    char programs[32];
    system_file(programs, sizeof(programs), "\\Programs");
    static struct dosxc_dirent entries[128];
    long count = dosxc_sys_listdir(programs, entries, 128);
    for (long i = 0; i < count && n < max; ++i) {
        if (!(entries[i].attributes & DOSXC_ATTR_DIRECTORY) || entries[i].name[0] == '.') continue;
        char dir[260];
        snprintf(dir, sizeof(dir), "%s\\%s", programs, entries[i].name);
        n += read_manifest(dir, out + n, max - n);
    }
    return n;
}

int app_find(const char *id, struct app_entry *out) {
    static struct app_entry list[64];
    int n = apps_list(list, 64);
    for (int i = 0; i < n; ++i)
        if (!strcasecmp(list[i].id, id)) { *out = list[i]; return 0; }
    return -1;
}

static int has_word(const char *list, const char *word) {
    size_t n = strlen(word);
    for (const char *p = list; *p;) {
        while (*p == ' ' || *p == ',') ++p;
        const char *end = p;
        while (*end && *end != ' ' && *end != ',') ++end;
        if ((size_t)(end - p) == n && !strncasecmp(p, word, n)) return 1;
        p = end;
    }
    return 0;
}

int program_installed(const char *name) {
    struct app_entry e;
    return !app_find(name, &e);
}

int program_info(const char *name, char *label, size_t label_size, int *icon) {
    struct app_entry e;
    if (app_find(name, &e)) return -1;
    if (label) snprintf(label, label_size, "%s", e.name);
    if (icon) *icon = e.icon;
    return 0;
}

int programs_for(const char *kind, struct app_entry *out, int max) {
    static struct app_entry list[64];
    int n = apps_list(list, 64), found = 0;
    for (int i = 0; i < n && found < max; ++i)
        if (has_word(list[i].defaults, kind)) out[found++] = list[i];
    return found;
}

void default_program(const char *kind, char *out, size_t size) {
    out[0] = 0;
    char value[32];
    if (!config_get(kind, value, sizeof(value))) {
        if (!strcmp(value, "none")) return;
        struct app_entry e;
        if (!app_find(value, &e) && has_word(e.defaults, kind)) { snprintf(out, size, "%s", value); return; }
    }
    struct app_entry first;
    if (programs_for(kind, &first, 1)) snprintf(out, size, "%s", first.id);
}

int app_start(const struct app_entry *e, const char *argument) {
    char app[260], url[300];
    if (argument && e->url && argument[0] && argument[1] == ':') {
        size_t n = (size_t)snprintf(url, sizeof(url), "file://");
        for (const char *p = argument + 2; *p && n + 1 < sizeof(url); ++p) url[n++] = *p == '\\' ? '/' : *p;
        url[n] = 0;
        argument = url;
    }
    if (e->terminal) {
        desktop_path(app, sizeof(app), "Apps\\TERMINAL.EXE");
        char *argv[4] = {app, (char *)e->program, (char *)argument, 0};
        return dosxc_sys_task(DOSXC_TASK_START, app, (long)argv, 0) < 0 ? -1 : 0;
    }
    char *argv[3] = {(char *)e->program, (char *)argument, 0};
    return dosxc_sys_task(DOSXC_TASK_START, (void *)e->program, (long)argv, 0) < 0 ? -1 : 0;
}

int program_start(const char *name, const char *argument) {
    struct app_entry e;
    if (app_find(name, &e)) return -1;
    return app_start(&e, argument);
}

/* The program for a file of type EXTENSION: a default program that takes it,
   else any program that does. */
static int program_for_type(const char *extension, struct app_entry *out) {
    static const char *const kinds[] = {"browser", "media", "document", "spreadsheet", "presentation", "editor"};
    for (unsigned k = 0; k < sizeof(kinds) / sizeof(*kinds); ++k) {
        char id[32];
        default_program(kinds[k], id, sizeof(id));
        if (id[0] && !app_find(id, out) && has_word(out->types, extension)) return 0;
    }
    static struct app_entry list[64];
    int n = apps_list(list, 64);
    for (int i = 0; i < n; ++i)
        if (has_word(list[i].types, extension)) { *out = list[i]; return 0; }
    return -1;
}

static int has_extension(const char *path, const char *const *list) {
    const char *dot = strrchr(path, '.');
    if (!dot || strchr(dot, '\\')) return 0;
    for (; *list; ++list)
        if (!strcasecmp(dot, *list)) return 1;
    return 0;
}

int open_path(const char *path) {
    static const char *const programs_ext[] = {".EXE", ".COM", ".BAT", 0};
    char app[260];
    struct dosxc_dirent probe[1];
    if (has_extension(path, (const char *const[]){".LNK", 0})) {
        struct shortcut s;
        if (shortcut_read(path, &s)) return -1;
        if (s.terminal) {
            desktop_path(app, sizeof(app), "Apps\\TERMINAL.EXE");
            char *argv[4] = {app, s.target, s.args[0] ? s.args : 0, 0};
            return dosxc_sys_task(DOSXC_TASK_START, app, (long)argv, 0) < 0 ? -1 : 0;
        }
        if (has_extension(s.target, programs_ext)) {
            char *argv[3] = {s.target, s.args[0] ? s.args : 0, 0};
            return dosxc_sys_task(DOSXC_TASK_START, s.target, (long)argv, 0) < 0 ? -1 : 0;
        }
        return open_path(s.target);
    }
    if (dosxc_sys_listdir(path, probe, 1) >= 0) {
        desktop_path(app, sizeof(app), "Apps\\EXPLORER.EXE");
        char *argv[3] = {app, (char *)path, 0};
        return dosxc_sys_task(DOSXC_TASK_START, app, (long)argv, 0) < 0 ? -1 : 0;
    }
    if (has_extension(path, (const char *const[]){".TTF", ".OTF", ".TTC", 0})) {
        desktop_path(app, sizeof(app), "Apps\\FONTS.EXE");
        char *argv[3] = {app, (char *)path, 0};
        return dosxc_sys_task(DOSXC_TASK_START, app, (long)argv, 0) < 0 ? -1 : 0;
    }
    if (image_is_picture(path)) {
        desktop_path(app, sizeof(app), "Apps\\PAINT.EXE");
        char *argv[3] = {app, (char *)path, 0};
        return dosxc_sys_task(DOSXC_TASK_START, app, (long)argv, 0) < 0 ? -1 : 0;
    }
    if (has_extension(path, programs_ext)) {
        desktop_path(app, sizeof(app), "Apps\\");
        if (!strncasecmp(path, app, strlen(app))) {
            char *argv[2] = {(char *)path, 0};
            return dosxc_sys_task(DOSXC_TASK_START, (void *)path, (long)argv, 0) < 0 ? -1 : 0;
        }
        desktop_path(app, sizeof(app), "Apps\\TERMINAL.EXE");
        char *argv[3] = {app, (char *)path, 0};
        return dosxc_sys_task(DOSXC_TASK_START, app, (long)argv, 0) < 0 ? -1 : 0;
    }
    const char *dot = strrchr(path, '.');
    struct app_entry e;
    if (dot && !strchr(dot, '\\') && !program_for_type(dot + 1, &e)) return app_start(&e, path);
    if (has_extension(path, (const char *const[]){".ZIP", ".7Z", ".RAR", ".TAR", ".TGZ", ".TXZ", ".GZ", ".XZ", ".ISO", ".IMG", ".IMA", 0})) {
        desktop_path(app, sizeof(app), "Apps\\EXPLORER.EXE");
        char *argv[4] = {app, "/open", (char *)path, 0};
        return dosxc_sys_task(DOSXC_TASK_START, app, (long)argv, 0) < 0 ? -1 : 0;
    }
    char editor[32];
    default_program("editor", editor, sizeof(editor));
    return program_start(editor[0] ? editor : "editor", path);
}
