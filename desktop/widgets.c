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
    if (!home || !*home) home = "\\Home";
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

static const struct { const char *name, *label, *path; int icon, terminal; } programs[] = {
    {"dosfox", "DOSfox", "\\Programs\\dosfox\\DOSFOX.EXE", ICON_DOSFOX, 0},
    {"editor", "File Editor", 0, ICON_EDITOR, 0},
    {"vim", "Vim", "\\Programs\\Vim\\BIN\\VIM.EXE", ICON_EDITOR, 1},
};

static int program_index(const char *name) {
    for (int i = 0; i < (int)(sizeof(programs) / sizeof(*programs)); ++i)
        if (!strcmp(programs[i].name, name)) return i;
    return -1;
}

int program_installed(const char *name) {
    int i = program_index(name);
    if (i < 0) return 0;
    if (!programs[i].path) return 1;
    char path[260];
    system_file(path, sizeof(path), programs[i].path);
    return exists(path);
}

int program_info(const char *name, char *label, size_t label_size, int *icon) {
    int i = program_index(name);
    if (i < 0 || !program_installed(name)) return -1;
    if (label) snprintf(label, label_size, "%s", programs[i].label);
    if (icon) *icon = programs[i].icon;
    return 0;
}

void default_program(const char *kind, char *out, size_t size) {
    out[0] = 0;
    char value[32];
    int browser = !strcmp(kind, "browser");
    if (!config_get(kind, value, sizeof(value))) {
        if (!strcmp(value, "none") || program_installed(value)) snprintf(out, size, "%s", value);
        if (out[0]) { if (!strcmp(out, "none")) out[0] = 0; return; }
    }
    if (browser) { if (program_installed("dosfox")) snprintf(out, size, "dosfox"); }
    else snprintf(out, size, "editor");
}

int program_start(const char *name, const char *argument) {
    int i = program_index(name);
    if (i < 0 || !program_installed(name)) return -1;
    char path[260], app[260];
    if (!programs[i].path) {
        desktop_path(app, sizeof(app), "Apps\\EDITOR.EXE");
        char *argv[3] = {app, (char *)argument, 0};
        return dosxc_sys_task(DOSXC_TASK_START, app, (long)argv, 0) < 0 ? -1 : 0;
    }
    system_file(path, sizeof(path), programs[i].path);
    char url[300];
    if (argument && !strcmp(name, "dosfox") && argument[0] && argument[1] == ':') {
        size_t n = (size_t)snprintf(url, sizeof(url), "file://");
        for (const char *p = argument + 2; *p && n + 1 < sizeof(url); ++p) url[n++] = *p == '\\' ? '/' : *p;
        url[n] = 0;
        argument = url;
    }
    if (programs[i].terminal) {
        desktop_path(app, sizeof(app), "Apps\\TERMINAL.EXE");
        char *argv[4] = {app, path, (char *)argument, 0};
        return dosxc_sys_task(DOSXC_TASK_START, app, (long)argv, 0) < 0 ? -1 : 0;
    }
    char *argv[3] = {path, (char *)argument, 0};
    return dosxc_sys_task(DOSXC_TASK_START, path, (long)argv, 0) < 0 ? -1 : 0;
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
    static const char *const web[] = {".HTM", ".HTML", ".SVG", ".XHTML", 0};
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
    char browser[32], editor[32];
    default_program("browser", browser, sizeof(browser));
    if (browser[0] && has_extension(path, web)) return program_start(browser, path);
    default_program("editor", editor, sizeof(editor));
    return program_start(editor, path);
}
