/*
 * DOS-XC /desktop/dialogs.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "wm.h"
#include <ctype.h>
#include <strings.h>

/* ---- message box -------------------------------------------------------- */

struct message { char line1[80], line2[80]; };
static const struct rect message_ok = {120, 58, 80, 24};

static void message_paint(struct window *w, const struct canvas *c) {
    struct message *m = w->data;
    text(c, 16, 14, &font_regular, m->line1, C_TEXT);
    text(c, 16, 30, &font_regular, m->line2, C_TEXT);
    button(c, message_ok, "OK", 0, 0);
}
static void message_mouse(struct window *w, int x, int y, int kind, int value) {
    (void)value;
    if (kind == M_DOWN && rect_has(message_ok, x, y)) window_close(w);
}
static void message_key(struct window *w, int key) { if (key == '\n' || key == 27) window_close(w); }
static void message_closed(struct window *w) { free(w->data); }
static const struct app_ops message_ops = {message_paint, message_mouse, message_key, message_closed};

void message_box(const char *title, const char *line1, const char *line2) {
    struct message *m = calloc(1, sizeof(*m));
    if (!m) return;
    snprintf(m->line1, sizeof(m->line1), "%s", line1);
    snprintf(m->line2, sizeof(m->line2), "%s", line2);
    struct window *w = window_open(320, 92, title, ICON_INFO, &message_ops, m);
    if (!w) free(m);
    else window_modal(w);
}

/* ---- a question --------------------------------------------------------- */

struct question { char line[100], yes[24], no[24]; void (*done)(int, void *); void *data; };
static const struct rect ask_yes = {40, 56, 80, 24}, ask_no = {128, 56, 80, 24}, ask_cancel = {216, 56, 80, 24};

static void ask_paint(struct window *w, const struct canvas *c) {
    struct question *q = w->data;
    text(c, 16, 18, &font_regular, q->line, C_TEXT);
    button(c, ask_yes, q->yes, 0, 1);
    button(c, ask_no, q->no, 0, 0);
    button(c, ask_cancel, "Cancel", 0, 0);
}
static void ask_finish(struct window *w, int answer) {
    struct question *q = w->data;
    void (*done)(int, void *) = q->done;
    void *data = q->data;
    q->done = 0;
    window_close(w);
    if (done) done(answer, data);
}
static void ask_mouse(struct window *w, int x, int y, int kind, int value) {
    (void)value;
    if (kind != M_DOWN) return;
    if (rect_has(ask_yes, x, y)) ask_finish(w, 1);
    else if (rect_has(ask_no, x, y)) ask_finish(w, 0);
    else if (rect_has(ask_cancel, x, y)) ask_finish(w, -1);
}
static void ask_key(struct window *w, int key) { if (key == 27) ask_finish(w, -1); else if (key == '\n') ask_finish(w, 1); }
static void ask_closed(struct window *w) { struct question *q = w->data; if (q->done) q->done(-1, q->data); free(q); }
static const struct app_ops ask_ops = {ask_paint, ask_mouse, ask_key, ask_closed};

void ask_box(const char *title, const char *question, const char *yes, const char *no, void (*done)(int, void *), void *data) {
    struct question *q = calloc(1, sizeof(*q));
    if (!q) return;
    snprintf(q->line, sizeof(q->line), "%s", question);
    snprintf(q->yes, sizeof(q->yes), "%s", yes);
    snprintf(q->no, sizeof(q->no), "%s", no);
    q->done = done;
    q->data = data;
    struct window *w = window_open(336, 92, title, ICON_INFO, &ask_ops, q);
    if (!w) free(q);
    else window_modal(w);
}

struct input { char prompt[100]; struct field field; int secret; void (*done)(const char *, void *); void *data; };
static const struct rect input_field = {16, 34, 304, 22}, input_ok = {152, 66, 80, 24}, input_cancel = {240, 66, 80, 24};

static void input_paint(struct window *w, const struct canvas *c) {
    struct input *in = w->data;
    text(c, 16, 14, &font_regular, in->prompt, C_TEXT);
    struct field shown = in->field;
    if (in->secret) for (size_t i = 0; shown.text[i]; ++i) shown.text[i] = '*';
    field_paint(c, &shown, input_field, 1);
    button(c, input_ok, "OK", 0, 1);
    button(c, input_cancel, "Cancel", 0, 0);
}
static void input_finish(struct window *w, int ok) {
    struct input *in = w->data;
    void (*done)(const char *, void *) = in->done;
    void *data = in->data;
    char text_copy[260];
    snprintf(text_copy, sizeof(text_copy), "%s", in->field.text);
    in->done = 0;
    window_close(w);
    if (done) done(ok && text_copy[0] ? text_copy : 0, data);
}
static void input_mouse(struct window *w, int x, int y, int kind, int value) {
    (void)value;
    if (kind != M_DOWN) return;
    if (rect_has(input_ok, x, y)) input_finish(w, 1);
    else if (rect_has(input_cancel, x, y)) input_finish(w, 0);
}
static void input_key(struct window *w, int key) {
    struct input *in = w->data;
    if (key == '\n') input_finish(w, 1);
    else if (key == 27) input_finish(w, 0);
    else if (field_key(&in->field, key)) window_paint(w);
}
static void input_closed(struct window *w) {
    struct input *in = w->data;
    if (in->done) in->done(0, in->data);
    memset(in, 0, sizeof(*in));
    free(in);
}
static const struct app_ops input_ops = {input_paint, input_mouse, input_key, input_closed};

static void open_input(const char *title, const char *prompt, const char *initial, int secret, void (*done)(const char *, void *), void *data);

void input_box(const char *title, const char *prompt, const char *initial, void (*done)(const char *, void *), void *data) {
    open_input(title, prompt, initial, 0, done, data);
}

void secret_box(const char *title, const char *prompt, void (*done)(const char *, void *), void *data) {
    open_input(title, prompt, "", 1, done, data);
}

static void open_input(const char *title, const char *prompt, const char *initial, int secret, void (*done)(const char *, void *), void *data) {
    struct input *in = calloc(1, sizeof(*in));
    if (!in) return;
    in->secret = secret;
    snprintf(in->prompt, sizeof(in->prompt), "%s", prompt);
    field_set(&in->field, initial ? initial : "");
    in->done = done;
    in->data = data;
    struct window *w = window_open(336, 100, title, ICON_EDITOR, &input_ops, in);
    if (!w) free(in);
    else window_modal(w);
}

struct confirm { char line[100], yes[24]; int icon; void (*done)(int, void *); void *data; };
static const struct rect confirm_yes = {120, 56, 96, 24}, confirm_no = {224, 56, 96, 24};

static void confirm_paint(struct window *w, const struct canvas *c) {
    struct confirm *q = w->data;
    draw_icon(c, 16, 15, q->icon);
    text(c, 40, 18, &font_regular, q->line, C_TEXT);
    button(c, confirm_yes, q->yes, 0, 1);
    button(c, confirm_no, "Cancel", 0, 0);
}
static void confirm_finish(struct window *w, int answer) {
    struct confirm *q = w->data;
    void (*done)(int, void *) = q->done;
    void *data = q->data;
    q->done = 0;
    window_close(w);
    if (done) done(answer, data);
}
static void confirm_mouse(struct window *w, int x, int y, int kind, int value) {
    (void)value;
    if (kind != M_DOWN) return;
    if (rect_has(confirm_yes, x, y)) confirm_finish(w, 1);
    else if (rect_has(confirm_no, x, y)) confirm_finish(w, 0);
}
static void confirm_key(struct window *w, int key) { if (key == 27) confirm_finish(w, 0); else if (key == '\n') confirm_finish(w, 1); }
static void confirm_closed(struct window *w) { struct confirm *q = w->data; if (q->done) q->done(0, q->data); free(q); }
static const struct app_ops confirm_ops = {confirm_paint, confirm_mouse, confirm_key, confirm_closed};

void confirm_box(const char *title, const char *question, const char *yes, int icon, void (*done)(int, void *), void *data) {
    struct confirm *q = calloc(1, sizeof(*q));
    if (!q) return;
    snprintf(q->line, sizeof(q->line), "%s", question);
    snprintf(q->yes, sizeof(q->yes), "%s", yes);
    q->icon = icon;
    q->done = done;
    q->data = data;
    struct window *w = window_open(336, 92, title, icon, &confirm_ops, q);
    if (!w) free(q);
    else window_modal(w);
}

#define FD_MAX 512
#define FD_ROW 18
struct dialog {
    char folder[260];
    struct dosxc_dirent *entries;
    int count, selected, save;
    struct vscroll scroll;
    struct field name;
    void (*done)(const char *path, void *data);
    void *data;
};
static const struct rect fd_list = {10, 34, 380, 216}, fd_field = {64, 258, 326, 22};
static const struct rect fd_ok = {232, 288, 76, 24}, fd_cancel = {314, 288, 76, 24}, fd_up = {10, 6, 40, 22};

static int dialog_order(const void *a, const void *b) {
    const struct dosxc_dirent *x = a, *y = b;
    int dx = !(x->attributes & DOSXC_ATTR_DIRECTORY), dy = !(y->attributes & DOSXC_ATTR_DIRECTORY);
    return dx != dy ? dx - dy : strcasecmp(x->name, y->name);
}

static void dialog_load(struct dialog *d) {
    d->selected = -1;
    d->scroll.top = 0;
    d->count = 0;
    if (!d->folder[0]) {
        struct dosxc_volume volumes[26];
        int n = dosxc_sys_volumes(volumes, 26);
        for (int i = 0; i < n && i < 26; ++i) {
            memset(&d->entries[i], 0, sizeof(d->entries[i]));
            snprintf(d->entries[i].name, sizeof(d->entries[i].name), "%c:", volumes[i].letter);
            d->entries[i].attributes = DOSXC_ATTR_DIRECTORY;
            ++d->count;
        }
    } else {
        int n = (int)dosxc_sys_listdir(d->folder, d->entries, FD_MAX);
        for (int i = 0; i < n && i < FD_MAX; ++i)
            if (strcmp(d->entries[i].name, ".") && strcmp(d->entries[i].name, "..")) d->entries[d->count++] = d->entries[i];
        qsort(d->entries, (size_t)d->count, sizeof(*d->entries), dialog_order);
    }
    d->scroll.total = d->count;
    d->scroll.visible = fd_list.h / FD_ROW;
}

static void dialog_paint(struct window *w, const struct canvas *c) {
    struct dialog *d = w->data;
    button(c, fd_up, "Up", 0, 0);
    text(c, 58, 11, &font_regular, d->folder[0] ? d->folder : "Computer", C_TEXT);
    bevel(c, fd_list.x - 2, fd_list.y - 2, fd_list.w + 4, fd_list.h + 4, 1);
    fill(c, fd_list.x, fd_list.y, fd_list.w, fd_list.h, C_BASE);
    for (int i = 0; i < d->scroll.visible && d->scroll.top + i < d->count; ++i) {
        const struct dosxc_dirent *e = &d->entries[d->scroll.top + i];
        int y = fd_list.y + i * FD_ROW, on = d->scroll.top + i == d->selected;
        if (on) fill(c, fd_list.x, y, fd_list.w - 14, FD_ROW, C_SELECTION);
        draw_icon(c, fd_list.x + 2, y + 1, (e->attributes & DOSXC_ATTR_DIRECTORY) ? (d->folder[0] ? ICON_FOLDER : ICON_DRIVE) : ICON_FILE);
        text(c, fd_list.x + 22, y + 4, &font_regular, e->name, on ? 0xffffff : C_TEXT);
    }
    vscroll_paint(c, &d->scroll, (struct rect){fd_list.x + fd_list.w - 14, fd_list.y, 14, fd_list.h});
    text(c, 10, 263, &font_regular, "File name:", C_TEXT);
    field_paint(c, &d->name, fd_field, 1);
    button(c, fd_ok, d->save ? "Save" : "Open", 0, 1);
    button(c, fd_cancel, "Cancel", 0, 0);
}

static void dialog_up(struct window *w) {
    struct dialog *d = w->data;
    size_t n = strlen(d->folder);
    if (!n) return;
    if (n <= 3) d->folder[0] = 0;
    else {
        while (n > 3 && d->folder[n - 1] != '\\') --n;
        if (n > 3) --n;
        d->folder[n] = 0;
    }
    dialog_load(d);
    window_paint(w);
}

static void dialog_enter(struct window *w, int index) {
    struct dialog *d = w->data;
    if (index < 0 || index >= d->count) return;
    const struct dosxc_dirent *e = &d->entries[index];
    if (!(e->attributes & DOSXC_ATTR_DIRECTORY)) return;
    if (!d->folder[0]) snprintf(d->folder, sizeof(d->folder), "%s\\", e->name);
    else {
        char next[260];
        snprintf(next, sizeof(next), "%s%s%s", d->folder, d->folder[strlen(d->folder) - 1] == '\\' ? "" : "\\", e->name);
        snprintf(d->folder, sizeof(d->folder), "%s", next);
    }
    dialog_load(d);
    window_paint(w);
}

static void dialog_accept(struct window *w) {
    struct dialog *d = w->data;
    if (!d->name.text[0] || !d->folder[0]) return;
    char path[520];
    if (strchr(d->name.text, ':') || d->name.text[0] == '\\') snprintf(path, sizeof(path), "%s", d->name.text);
    else snprintf(path, sizeof(path), "%s%s%s", d->folder, d->folder[strlen(d->folder) - 1] == '\\' ? "" : "\\", d->name.text);
    void (*done)(const char *, void *) = d->done;
    void *data = d->data;
    d->done = 0;
    window_close(w);
    if (done) done(path, data);
}

static void dialog_cancel(struct window *w) {
    struct dialog *d = w->data;
    void (*done)(const char *, void *) = d->done;
    void *data = d->data;
    d->done = 0;
    window_close(w);
    if (done) done(0, data);
}

static void dialog_mouse(struct window *w, int x, int y, int kind, int value) {
    struct dialog *d = w->data;
    struct rect bar = {fd_list.x + fd_list.w - 14, fd_list.y, 14, fd_list.h};
    if (vscroll_mouse(&d->scroll, bar, x, y, kind, value)) { window_paint(w); return; }
    if (kind == M_WHEEL) { d->scroll.top -= value * 2; vscroll_clamp(&d->scroll); window_paint(w); return; }
    if (kind != M_DOWN && kind != M_DOUBLE) return;
    if (rect_has(fd_up, x, y)) { dialog_up(w); return; }
    if (rect_has(fd_ok, x, y)) { dialog_accept(w); return; }
    if (rect_has(fd_cancel, x, y)) { dialog_cancel(w); return; }
    if (rect_has(fd_list, x, y)) {
        int at = d->scroll.top + (y - fd_list.y) / FD_ROW;
        if (at < d->count) {
            d->selected = at;
            if (!(d->entries[at].attributes & DOSXC_ATTR_DIRECTORY)) field_set(&d->name, d->entries[at].name);
            window_paint(w);
            if (kind == M_DOUBLE) {
                if (d->entries[at].attributes & DOSXC_ATTR_DIRECTORY) dialog_enter(w, at);
                else dialog_accept(w);
            }
        }
    }
}

static void dialog_key(struct window *w, int key) {
    struct dialog *d = w->data;
    if (key == '\n') { dialog_accept(w); return; }
    if (key == 27) { dialog_cancel(w); return; }
    if (field_key(&d->name, key)) window_paint(w);
}

static void dialog_closed(struct window *w) {
    struct dialog *d = w->data;
    if (d->done) d->done(0, d->data);
    free(d->entries);
    free(d);
}
static const struct app_ops dialog_ops = {dialog_paint, dialog_mouse, dialog_key, dialog_closed};

void file_dialog(const char *title, int save, const char *start, void (*done)(const char *path, void *data), void *data) {
    struct dialog *d = calloc(1, sizeof(*d));
    if (!d) return;
    d->entries = calloc(FD_MAX, sizeof(*d->entries));
    if (!d->entries) { free(d); return; }
    d->save = save;
    d->done = done;
    d->data = data;
    if (start && start[0]) snprintf(d->folder, sizeof(d->folder), "%s", start);
    else {
        const char *home = getenv("HOME");
        struct dosxc_sysinfo info;
        dosxc_sys_sysinfo(&info);
        snprintf(d->folder, sizeof(d->folder), "%s", home && *home ? home : "\\");
        if (d->folder[1] != ':') { char t[260]; snprintf(t, sizeof(t), "%c:%s", info.drive, d->folder); snprintf(d->folder, sizeof(d->folder), "%s", t); }
    }
    dialog_load(d);
    struct window *w = window_open(400, 320, title, ICON_FOLDER, &dialog_ops, d);
    if (!w) { free(d->entries); free(d); return; }
    window_modal(w);
}
