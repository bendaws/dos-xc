/*
 * DOS-XC /desktop/editor.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * EDITOR.EXE: the File Editor, a plain text editor.
 */
#include "wm.h"

#define LINE_H (font_mono.height + 3)
#define TEXT_LEFT 4
#define STATUS_H 20

static const struct menubar bar = {
    {"File", "Edit"},
    {"New\nOpen...\nSave\nSave As...\n-\nExit\n", "Cut\nCopy\nPaste\nDelete\n-\nSelect All\n"},
};

static char *text_buffer;
static int length, capacity;
static int *lines;
static int line_count, line_capacity;
static int caret, select_from = -1;
static int crlf;
static int left_scroll, dragging;
static struct vscroll scroll;
static char path[300];
static int modified;
static int pending_action;
static struct window *main_window;

static void file_new(struct window *w);
static void do_open(struct window *w);
static int save_now(struct window *w);
static void update_title(struct window *w);

static void reindex(void) {
    int n = 1;
    for (int i = 0; i < length; ++i) if (text_buffer[i] == '\n') ++n;
    if (n + 1 > line_capacity) {
        line_capacity = n * 2 + 16;
        lines = realloc(lines, (size_t)line_capacity * sizeof(int));
    }
    lines[0] = 0;
    n = 1;
    for (int i = 0; i < length; ++i) if (text_buffer[i] == '\n') lines[n++] = i + 1;
    lines[n] = length + 1;
    line_count = n;
    scroll.total = n;
}

static int line_of(int at) {
    int low = 0, high = line_count - 1;
    while (low < high) {
        int mid = (low + high + 1) / 2;
        if (lines[mid] <= at) low = mid; else high = mid - 1;
    }
    return low;
}

static int line_end(int line) { return lines[line + 1] - 1; }
static int char_w(void) { return font_mono.widths['M'] + font_mono.spacing; }
static int rows_visible(const struct window *w) { return (w->content.h - MENUBAR_H - STATUS_H - 4) / LINE_H; }
static int text_area_w(const struct window *w) { return w->content.w - 14 - TEXT_LEFT * 2; }

static void keep_visible(struct window *w) {
    int line = line_of(caret), rows = rows_visible(w);
    if (line < scroll.top) scroll.top = line;
    if (line >= scroll.top + rows) scroll.top = line - rows + 1;
    vscroll_clamp(&scroll);
    int column = (caret - lines[line]) * char_w(), area = text_area_w(w);
    if (column < left_scroll) left_scroll = column;
    if (column > left_scroll + area - char_w()) left_scroll = column - area + char_w();
    if (left_scroll < 0) left_scroll = 0;
}

static void reserve(int total) {
    if (total + 1 <= capacity) return;
    capacity = total * 2 + 256;
    text_buffer = realloc(text_buffer, (size_t)capacity);
}

static void remove_range(int from, int to) {
    memmove(text_buffer + from, text_buffer + to, (size_t)(length - to));
    length -= to - from;
    text_buffer[length] = 0;
    modified = 1;
}

static void insert_text(int at, const char *s, int n) {
    reserve(length + n);
    memmove(text_buffer + at + n, text_buffer + at, (size_t)(length - at));
    memcpy(text_buffer + at, s, (size_t)n);
    length += n;
    text_buffer[length] = 0;
    modified = 1;
}

static int has_selection(void) { return select_from >= 0 && select_from != caret; }
static int sel_low(void) { return select_from < caret ? select_from : caret; }
static int sel_high(void) { return select_from > caret ? select_from : caret; }

static void delete_selection(void) {
    if (!has_selection()) { select_from = -1; return; }
    int low = sel_low(), high = sel_high();
    remove_range(low, high);
    caret = low;
    select_from = -1;
}

static void edit_paint(struct window *w, const struct canvas *c) {
    menubar_paint(c, &bar, w->content.w);
    struct rect area = {0, MENUBAR_H, w->content.w - 14, w->content.h - MENUBAR_H - STATUS_H};
    fill(c, area.x, area.y, area.w, area.h, C_BASE);
    struct canvas inner = *c;
    inner.clip = rect_both(c->clip, area);
    scroll.visible = rows_visible(w);
    int low = has_selection() ? sel_low() : -1, high = has_selection() ? sel_high() : -1;
    int cw = char_w();
    for (int row = 0; row < scroll.visible && scroll.top + row < line_count; ++row) {
        int line = scroll.top + row, start = lines[line], end = line_end(line);
        int y = area.y + 2 + row * LINE_H, x = TEXT_LEFT - left_scroll;
        if (low >= 0 && low <= end && high >= start) {
            int a = low > start ? low : start, b = high < end ? high : end + (high > end ? 1 : 0);
            fill(&inner, x + (a - start) * cw, y - 1, (b - a) * cw, LINE_H, C_SELECTION);
        }
        for (int i = start; i < end; ++i) {
            char one[2] = {text_buffer[i], 0};
            int picked = low >= 0 && i >= low && i < high;
            text(&inner, x + (i - start) * cw, y, &font_mono, one, picked ? 0xffffff : C_TEXT);
        }
        if (caret >= start && caret <= end && !has_selection())
            fill(&inner, x + (caret - start) * cw, y - 1, 1, LINE_H, C_TEXT);
    }
    vscroll_paint(c, &scroll, (struct rect){w->content.w - 14, MENUBAR_H, 14, area.h});
    int line = line_of(caret);
    fill(c, 0, w->content.h - STATUS_H, w->content.w, STATUS_H, C_WINDOW);
    fill(c, 0, w->content.h - STATUS_H, w->content.w, 1, C_SHADOW1);
    char status[64];
    snprintf(status, sizeof(status), "Ln %d, Col %d", line + 1, caret - lines[line] + 1);
    text(c, 8, w->content.h - STATUS_H + 5, &font_regular, status, C_TEXT);
}

static int caret_from_point(struct window *w, int x, int y) {
    int row = (y - MENUBAR_H - 2) / LINE_H;
    int line = scroll.top + (row < 0 ? 0 : row);
    if (line >= line_count) line = line_count - 1;
    int column = (x - TEXT_LEFT + left_scroll + char_w() / 2) / char_w();
    if (column < 0) column = 0;
    int at = lines[line] + column, end = line_end(line);
    return at > end ? end : at;
}

static void edit_mouse(struct window *w, int x, int y, int kind, int value) {
    if (menubar_mouse(w, &bar, x, y, kind)) return;
    struct rect track = {w->content.w - 14, MENUBAR_H, 14, w->content.h - MENUBAR_H - STATUS_H};
    if (vscroll_mouse(&scroll, track, x, y, kind, value)) { window_paint(w); return; }
    if (kind == M_WHEEL) { scroll.top -= value * 3; vscroll_clamp(&scroll); window_paint(w); return; }
    if (kind == M_DOWN || kind == M_DOUBLE) {
        int at = caret_from_point(w, x, y);
        caret = at;
        select_from = at;
        dragging = 1;
        if (kind == M_DOUBLE) {                /* a word */
            int a = at, b = at;
            while (a > 0 && (text_buffer[a - 1] == '_' || (text_buffer[a - 1] > ' ' && text_buffer[a - 1] < 127))) --a;
            while (b < length && (text_buffer[b] == '_' || (text_buffer[b] > ' ' && text_buffer[b] < 127))) ++b;
            select_from = a;
            caret = b;
            dragging = 0;
        }
        window_paint(w);
    } else if (kind == M_MOVE && dragging) {
        caret = caret_from_point(w, x, y);
        keep_visible(w);
        window_paint(w);
    } else if (kind == M_UP) {
        dragging = 0;
    }
}

static void clipboard_copy(void) {
    if (has_selection()) gui_clipboard_set(text_buffer + sel_low(), sel_high() - sel_low());
}

static void paste(struct window *w) {
    int size = gui_board()->clipboard_size;
    char *data = malloc((size_t)size + 1);
    if (!data) return;
    size = gui_clipboard_get(data, size);
    delete_selection();
    insert_text(caret, data, size);
    caret += size;
    free(data);
    reindex();
    keep_visible(w);
    update_title(w);
    window_paint(w);
}

static void edit_key(struct window *w, int key) {
    switch (key) {
    case 1: select_from = 0; caret = length; window_paint(w); return;
    case 3: clipboard_copy(); return;
    case 24: clipboard_copy(); delete_selection(); reindex(); update_title(w); window_paint(w); return;
    case 22: paste(w); return;
    case 19: save_now(w); return;
    case 15: do_open(w); return;
    case 14: file_new(w); return;
    }
    int line = line_of(caret), column = caret - lines[line], moved = 1;
    switch (key) {
    case 0x101: if (line) { caret = lines[line - 1] + column; if (caret > line_end(line - 1)) caret = line_end(line - 1); } break;
    case 0x102: if (line + 1 < line_count) { caret = lines[line + 1] + column; if (caret > line_end(line + 1)) caret = line_end(line + 1); } break;
    case 0x103: if (caret < length) ++caret; break;
    case 0x104: if (caret) --caret; break;
    case 0x105: caret = lines[line]; break;
    case 0x106: caret = line_end(line); break;
    case 0x109: {
        int t = line - rows_visible(w); if (t < 0) t = 0;
        caret = lines[t] + column; if (caret > line_end(t)) caret = line_end(t);
        break;
    }
    case 0x10a: {
        int t = line + rows_visible(w); if (t >= line_count) t = line_count - 1;
        caret = lines[t] + column; if (caret > line_end(t)) caret = line_end(t);
        break;
    }
    default: moved = 0; break;
    }
    if (moved) { select_from = -1; keep_visible(w); window_paint(w); return; }
    if (key == '\b') {
        if (has_selection()) delete_selection();
        else if (caret) { remove_range(caret - 1, caret); --caret; }
    } else if (key == 0x108) {
        if (has_selection()) delete_selection();
        else if (caret < length) remove_range(caret, caret + 1);
    } else if (key == '\n' || key == '\t' || (key >= 32 && key < 256 && key != 127)) {
        delete_selection();
        if (key == '\t') { insert_text(caret, "    ", 4); caret += 4; }
        else { char c = (char)key; insert_text(caret, &c, 1); ++caret; }
    } else return;
    reindex();
    keep_visible(w);
    update_title(w);
    window_paint(w);
}

static void update_title(struct window *w) {
    char title[80];
    const char *name = path[0] ? strrchr(path, '\\') : 0;
    name = name ? name + 1 : path[0] ? path : "Untitled";
    snprintf(title, sizeof(title), "%s%s - File Editor", modified ? "*" : "", name);
    window_title(w, title);
}

static void file_new_now(struct window *w) {
    length = 0;
    reserve(0);
    text_buffer[0] = 0;
    path[0] = 0;
    caret = 0;
    select_from = -1;
    scroll.top = 0;
    left_scroll = 0;
    modified = 0;
    crlf = 1;
    reindex();
    update_title(w);
    window_paint(w);
}

static int load_file(struct window *w, const char *name) {
    FILE *f = fopen(name, "rb");
    if (!f) { message_box("File Editor", "The file could not be opened:", name); return -1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *raw = malloc((size_t)size + 1);
    if (!raw || (size && fread(raw, 1, (size_t)size, f) != (size_t)size)) { fclose(f); free(raw); return -1; }
    fclose(f);
    reserve((int)size * 4 + 4);
    int out = 0;
    crlf = 0;
    for (long i = 0; i < size; ++i) {
        char c = raw[i];
        if (c == '\r') { if (i + 1 < size && raw[i + 1] == '\n') { crlf = 1; continue; } c = '\n'; }
        if (c == '\t') { memcpy(text_buffer + out, "    ", 4); out += 4; continue; }
        text_buffer[out++] = c ? c : ' ';
    }
    free(raw);
    length = out;
    text_buffer[length] = 0;
    snprintf(path, sizeof(path), "%s", name);
    caret = 0;
    select_from = -1;
    scroll.top = 0;
    left_scroll = 0;
    modified = 0;
    reindex();
    update_title(w);
    window_paint(w);
    return 0;
}

static int write_file(struct window *w, const char *name) {
    FILE *f = fopen(name, "wb");
    int ok = f != 0;
    for (int i = 0; ok && i < length; ++i) {
        if (text_buffer[i] == '\n' && crlf && fputc('\r', f) == EOF) ok = 0;
        if (fputc(text_buffer[i], f) == EOF) ok = 0;
    }
    if (f && fclose(f)) ok = 0;
    if (!ok) { message_box("File Editor", "The file could not be saved:", name); return -1; }
    snprintf(path, sizeof(path), "%s", name);
    modified = 0;
    update_title(w);
    window_paint(w);
    return 0;
}

static void opened(const char *name, void *data) { if (name) load_file(data, name); }

static void after_save(struct window *w) {
    int action = pending_action;
    pending_action = 0;
    if (action == 1) window_close(w);
    else if (action == 2) file_dialog("Open", 0, 0, opened, w);
    else if (action == 3) file_new_now(w);
}

static void saved_as(const char *name, void *data) {
    if (name && !write_file(data, name)) after_save(data);
    else pending_action = 0;
}

static int save_now(struct window *w) {
    if (path[0]) return write_file(w, path);
    file_dialog("Save As", 1, 0, saved_as, w);
    return 1;
}

static void answer_save(int answer, void *data) {
    struct window *w = data;
    if (answer == 1) {
        int r = save_now(w);
        if (r == 0) after_save(w);
        else if (r < 0) pending_action = 0;
    } else if (answer == 0) {
        modified = 0;
        after_save(w);
    } else pending_action = 0;
}

static void guard_then(struct window *w, int action) {
    pending_action = action;
    if (modified) ask_box("File Editor", "Save the changes to this file?", "Save", "Don't Save", answer_save, w);
    else after_save(w);
}

static void file_new(struct window *w) { guard_then(w, 3); }
static void do_open(struct window *w) { guard_then(w, 2); }

static int edit_closing(struct window *w) {
    if (!modified) return 1;
    guard_then(w, 1);
    return 0;
}

static void edit_popup(struct window *w, int tag, int index) {
    if (index < 0) return;
    if (tag == 0) {
        switch (index) {
        case 0: file_new(w); break;
        case 1: do_open(w); break;
        case 2: save_now(w); break;
        case 3: file_dialog("Save As", 1, 0, saved_as, w); break;
        case 5: if (edit_closing(w)) window_close(w); break;
        }
    } else {
        switch (index) {
        case 0: clipboard_copy(); delete_selection(); reindex(); update_title(w); break;
        case 1: clipboard_copy(); return;
        case 2: paste(w); return;
        case 3: delete_selection(); reindex(); update_title(w); break;
        case 5: select_from = 0; caret = length; break;
        }
        window_paint(w);
    }
}

static void edit_resized(struct window *w) { keep_visible(w); }

static const struct app_ops edit_ops = {edit_paint, edit_mouse, edit_key, 0, edit_popup, 0, edit_resized, edit_closing};

int main(int argc, char **argv) {
    if (gui_connect()) return 1;
    reserve(256);
    text_buffer[0] = 0;
    crlf = 1;
    reindex();
    main_window = window_open(640, 460, "Untitled - File Editor", ICON_EDITOR, &edit_ops, 0);
    if (!main_window) return 1;
    main_window->resizable = 1;
    if (argc > 1) load_file(main_window, argv[1]);
    gui_run();
    return 0;
}
