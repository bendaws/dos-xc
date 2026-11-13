/*
 * DOS-XC /openwork/numbers.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "suite.h"
#include "sheet.h"
#include <strings.h>

#define FORMULA_H 30
#define HEADER_W 40
#define HEADER_H 20
#define CELL_W 76
#define CELL_H 20
#define SHEETS_H 24
#define SCROLL_W 16
#define GRID_LINE 0xdadcdd

static struct sheet sheet;
static struct window *main_window;
static int dirty, top, left, row, col, mark_row = -1, mark_col, dragging;
static int editing;                      /* 1 entering a new value, 2 editing the old one */
static struct field edit;
static struct vscroll scroll;
static struct ribbon rb;
static char find_text[260];

enum { C_PASTE, C_CUT, C_COPY, C_AUTOSUM, C_CLEAR, C_FIND, C_SELECT_ALL, C_INSERT_FUNCTION, C_MATH, C_LOGICAL, C_TEXT_FUNCTIONS, C_FUNCTIONS_HELP };

static const char *const sums[] = {"SUM", "AVERAGE", "COUNT", "MAX", "MIN"};
static const char *const math_functions[] = {"SUM", "AVERAGE", "MIN", "MAX", "COUNT", "COUNTA", "ABS", "ROUND", "SQRT", "INT", "MOD", "POWER", "PI"};
static const char *const logical_functions[] = {"IF", "AND", "OR", "NOT"};
static const char *const text_functions[] = {"LEN", "UPPER", "LOWER", "CONCAT"};

static const struct rb_item clipboard_items[] = {
    {RB_LARGE, C_PASTE, IC_PASTE, 0, 0, 0, "Paste"},
    {RB_SMALL, C_CUT, IC_CUT, 0, 0, RB_LABEL, "Cut"},
    {RB_SMALL, C_COPY, IC_COPY, 1, 0, RB_LABEL, "Copy"},
};
static const struct rb_item editing_items[] = {
    {RB_SMALL, C_AUTOSUM, IC_AUTOSUM, 0, 0, RB_LABEL | RB_MENU, "AutoSum"},
    {RB_SMALL, C_CLEAR, IC_CLEAR, 1, 0, RB_LABEL, "Clear"},
    {RB_SMALL, C_FIND, IC_FIND, 2, 0, RB_LABEL, "Find"},
    {RB_SMALL, C_SELECT_ALL, IC_SELECT_ALL, 0, 0, RB_LABEL, "Select All"},
};
static const struct rb_group home_groups[] = {{"Clipboard", clipboard_items, 3}, {"Editing", editing_items, 4}};
static const struct rb_item library_items[] = {
    {RB_LARGE, C_INSERT_FUNCTION, IC_FUNCTION, 0, 0, RB_MENU, "Insert\nFunction"},
    {RB_LARGE, C_AUTOSUM, IC_AUTOSUM, 0, 0, RB_MENU, "AutoSum"},
    {RB_SMALL, C_MATH, IC_AUTOSUM, 0, 0, RB_LABEL | RB_MENU, "Math & Trig"},
    {RB_SMALL, C_LOGICAL, IC_MARK, 1, 0, RB_LABEL | RB_MENU, "Logical"},
    {RB_SMALL, C_TEXT_FUNCTIONS, IC_WORD_COUNT, 2, 0, RB_LABEL | RB_MENU, "Text"},
};
static const struct rb_item help_items[] = {{RB_LARGE, C_FUNCTIONS_HELP, IC_INFO, 0, 0, 0, "Function\nList"}};
static const struct rb_group formula_groups[] = {{"Function Library", library_items, 5}, {"Help", help_items, 1}};
static const struct rb_tab tabs[] = {{"HOME", home_groups, 2}, {"FORMULAS", formula_groups, 2}};

static struct rect grid_rect(struct window *w) {
    return (struct rect){0, RIBBON_H + FORMULA_H, w->r.w - SCROLL_W, w->r.h - RIBBON_H - FORMULA_H - SHEETS_H - STATUS_H};
}
static struct rect scroll_rect(struct window *w) { struct rect g = grid_rect(w); return (struct rect){g.x + g.w, g.y, SCROLL_W, g.h}; }
static struct rect name_box(void) { return (struct rect){6, RIBBON_H + 4, 92, 22}; }
static struct rect formula_box(struct window *w) { return (struct rect){168, RIBBON_H + 4, w->r.w - 174, 22}; }
static int visible_rows(struct window *w) { return (grid_rect(w).h - HEADER_H) / CELL_H; }
static int visible_cols(struct window *w) { return (grid_rect(w).w - HEADER_W) / CELL_W; }

static void column_name(int c, char *out) {
    if (c < 26) { out[0] = (char)('A' + c); out[1] = 0; }
    else { out[0] = (char)('A' + c / 26 - 1); out[1] = (char)('A' + c % 26); out[2] = 0; }
}

static void selection(int *r1, int *c1, int *r2, int *c2) {
    int mr = mark_row < 0 ? row : mark_row, mc = mark_row < 0 ? col : mark_col;
    *r1 = mr < row ? mr : row; *r2 = mr < row ? row : mr;
    *c1 = mc < col ? mc : col; *c2 = mc < col ? col : mc;
}

static void paint_formula_bar(struct window *w, const struct canvas *c) {
    int y = RIBBON_H;
    fill(c, 0, y, w->r.w, FORMULA_H, 0xffffff);
    fill(c, 0, y + FORMULA_H - 1, w->r.w, 1, OW_BORDER);
    struct rect nb = name_box();
    char name[16];
    cell_name(row, col, name, sizeof(name));
    fill(c, nb.x, nb.y, nb.w, nb.h, 0xffffff);
    frame_box(c, nb.x, nb.y, nb.w, nb.h, 0xc6c6c6);
    text(c, nb.x + 6, nb.y + 6, &font_regular, name, OW_TEXT);
    fill(c, 110, y + 6, 1, 18, OW_LINE);
    ow_icon(c, 116, y + 7, IC_CANCEL, 16, editing ? 0 : 0xc8c8c8);
    ow_icon(c, 132, y + 7, IC_MARK, 16, editing ? 0 : 0xc8c8c8);
    ow_icon(c, 148, y + 7, IC_FUNCTION, 16, 0);
    struct field shown = edit;
    if (!editing) field_set(&shown, sheet_get(&sheet, row, col));
    ow_field(c, &shown, formula_box(w), editing == 2, 0);
}

static void paint(struct window *w, const struct canvas *c) {
    if (ribbon_paint(&rb, w, c)) return;
    paint_formula_bar(w, c);
    struct rect g = grid_rect(w);
    fill(c, g.x, g.y, g.w, g.h, 0xffffff);
    int rows = visible_rows(w) + 1, cols = visible_cols(w) + 1, r1, c1, r2, c2;
    selection(&r1, &c1, &r2, &c2);
    int range = r1 != r2 || c1 != c2;
    u32 green = app->accent, header_on = 0xe1e1e1;
    fill(c, g.x, g.y, g.w, HEADER_H, 0xf8f8f8);
    fill(c, g.x, g.y, HEADER_W, g.h, 0xf8f8f8);
    struct canvas cells = *c;
    cells.clip = rect_both(c->clip, (struct rect){g.x + HEADER_W, g.y + HEADER_H, g.w - HEADER_W, g.h - HEADER_H});
    struct canvas heads = *c;
    heads.clip = rect_both(c->clip, g);
    for (int i = 0; i < cols && left + i < SHEET_COLS; ++i) {
        int x = g.x + HEADER_W + i * CELL_W, on = left + i >= c1 && left + i <= c2;
        char label[4];
        column_name(left + i, label);
        if (on) { fill(&heads, x, g.y, CELL_W, HEADER_H, header_on); fill(&heads, x, g.y + HEADER_H - 2, CELL_W, 2, green); }
        text(&heads, x + (CELL_W - text_width(&font_regular, label)) / 2, g.y + 5, &font_regular, label, on ? green : OW_TEXT);
        fill(&heads, x + CELL_W - 1, g.y, 1, HEADER_H, 0xd4d4d4);
        fill(&cells, x + CELL_W - 1, g.y, 1, g.h, GRID_LINE);
    }
    for (int j = 0; j < rows && top + j < SHEET_ROWS; ++j) {
        int y = g.y + HEADER_H + j * CELL_H, r = top + j, on = r >= r1 && r <= r2;
        char label[8];
        snprintf(label, sizeof(label), "%d", r + 1);
        if (on) { fill(&heads, g.x, y, HEADER_W, CELL_H, header_on); fill(&heads, g.x + HEADER_W - 2, y, 2, CELL_H, green); }
        text(&heads, g.x + HEADER_W - 6 - text_width(&font_regular, label), y + 5, &font_regular, label, on ? green : OW_TEXT);
        fill(&heads, g.x, y + CELL_H - 1, HEADER_W, 1, 0xd4d4d4);
        fill(&cells, g.x, y + CELL_H - 1, g.w, 1, GRID_LINE);
        for (int i = 0; i < cols && left + i < SHEET_COLS; ++i) {
            int k = left + i, x = g.x + HEADER_W + i * CELL_W;
            int in_range = range && on && k >= c1 && k <= c2 && !(r == row && k == col);
            if (in_range) fill(&cells, x, y, CELL_W - 1, CELL_H - 1, 0xd9d9d9);
            if (!*sheet_get(&sheet, r, k) || (editing && r == row && k == col)) continue;
            struct value v;
            sheet_value(&sheet, r, k, &v);
            char shown[160];
            value_format(&v, shown, sizeof(shown));
            int tw = text_width(&font_regular, shown), right_align = v.kind == VALUE_NUMBER, extra = 0;
            if (!right_align) while (tw > CELL_W * (extra + 1) - 6 && k + extra + 1 < SHEET_COLS && !*sheet_get(&sheet, r, k + extra + 1)) ++extra;
            struct canvas one = cells;
            one.clip = rect_both(cells.clip, (struct rect){x, y, CELL_W * (extra + 1) - 2, CELL_H});
            if (extra) fill(&one, x, y, CELL_W * (extra + 1) - 1, CELL_H - 1, in_range ? 0xd9d9d9 : 0xffffff);
            text(&one, right_align ? x + CELL_W - 5 - tw : x + 4, y + 5, &font_regular, shown, v.kind == VALUE_ERROR ? 0xc00000 : 0x000000);
        }
    }
    fill(c, g.x + HEADER_W - 1, g.y, 1, g.h, 0xababab);
    fill(c, g.x, g.y + HEADER_H - 1, g.w, 1, 0xababab);
    if (range && r2 >= top && c2 >= left) {
        int x1 = g.x + HEADER_W + (c1 - left) * CELL_W, y1 = g.y + HEADER_H + (r1 - top) * CELL_H;
        int x2 = g.x + HEADER_W + (c2 - left + 1) * CELL_W, y2 = g.y + HEADER_H + (r2 - top + 1) * CELL_H;
        frame_box(&cells, x1 - 1, y1 - 1, x2 - x1 + 1, y2 - y1 + 1, green);
        frame_box(&cells, x1, y1, x2 - x1 - 1, y2 - y1 - 1, green);
    }
    if (row >= top && row < top + rows && col >= left && col < left + cols) {
        int x = g.x + HEADER_W + (col - left) * CELL_W, y = g.y + HEADER_H + (row - top) * CELL_H;
        if (editing) {
            struct canvas one = cells;
            int tw = text_width(&font_regular, edit.text) + 12, bw = tw > CELL_W ? tw : CELL_W;
            one.clip = rect_both(cells.clip, (struct rect){x, y, bw, CELL_H});
            fill(&one, x, y, bw, CELL_H - 1, 0xffffff);
            char head[260];
            snprintf(head, sizeof(head), "%.*s", edit.cursor, edit.text);
            text(&one, x + 4, y + 5, &font_regular, edit.text, 0x000000);
            fill(&one, x + 4 + text_width(&font_regular, head), y + 3, 1, CELL_H - 6, 0x000000);
        }
        if (!range) {
            frame_box(&cells, x - 1, y - 1, CELL_W + 1, CELL_H + 1, green);
            frame_box(&cells, x, y, CELL_W - 1, CELL_H - 1, green);
        }
        if (!editing) {
            int hx = g.x + HEADER_W + (c2 - left + 1) * CELL_W, hy = g.y + HEADER_H + (r2 - top + 1) * CELL_H;
            fill(&cells, hx - 4, hy - 4, 6, 6, 0xffffff);
            fill(&cells, hx - 3, hy - 3, 5, 5, green);
        }
    }
    scroll.total = SHEET_ROWS;
    scroll.visible = visible_rows(w);
    scroll.top = top;
    ow_vscroll_paint(c, &scroll, scroll_rect(w));
    int sy = w->r.h - STATUS_H - SHEETS_H;
    fill(c, 0, sy, w->r.w, SHEETS_H, 0xf3f3f3);
    fill(c, 0, sy, w->r.w, 1, OW_BORDER);
    fill(c, 34, sy, 70, SHEETS_H - 2, 0xffffff);
    fill(c, 34, sy, 1, SHEETS_H - 2, OW_BORDER);
    fill(c, 103, sy, 1, SHEETS_H - 2, OW_BORDER);
    fill(c, 35, sy + SHEETS_H - 4, 68, 2, green);
    text(c, 34 + (70 - text_width(&font_bold, "Sheet1")) / 2, sy + 7, &font_bold, "Sheet1", green);
    char status[200] = "";
    if (range) {
        double sum = 0;
        int count = 0, filled = 0;
        for (int r = r1; r <= r2; ++r)
            for (int k = c1; k <= c2; ++k) {
                if (!*sheet_get(&sheet, r, k)) continue;
                ++filled;
                struct value v;
                sheet_value(&sheet, r, k, &v);
                if (v.kind == VALUE_NUMBER) { sum += v.number; ++count; }
            }
        if (count) snprintf(status, sizeof(status), "AVERAGE: %.10g     COUNT: %d     SUM: %.10g", sum / count, filled, sum);
        else if (filled) snprintf(status, sizeof(status), "COUNT: %d", filled);
    }
    ow_status(c, w->r.h - STATUS_H, w->r.w, editing == 1 ? "ENTER" : editing == 2 ? "EDIT" : "READY", status[0] ? status : 0);
}

static void keep_visible(struct window *w) {
    int rows = visible_rows(w), cols = visible_cols(w);
    if (row < top) top = row;
    if (row >= top + rows) top = row - rows + 1;
    if (col < left) left = col;
    if (col >= left + cols) left = col - cols + 1;
    if (top < 0) top = 0;
    if (left < 0) left = 0;
}

static void commit(void) {
    if (!editing) return;
    if (strcmp(edit.text, sheet_get(&sheet, row, col))) { sheet_set(&sheet, row, col, edit.text); dirty = 1; }
    editing = 0;
}

static void start_edit(int mode, const char *initial) {
    editing = mode;
    field_set(&edit, initial);
}

static void reset_view(void) { row = col = top = left = 0; mark_row = -1; editing = 0; }

static void new_file(void) {
    sheet_free(&sheet);
    sheet_init(&sheet);
    reset_view();
    dirty = 0;
    suite_document(main_window, "");
    window_paint(main_window);
}

static void open_file(const char *path) {
    commit();
    if (!strcasecmp(extension_of(path), "gsheet")) { open_in_browser(path); return; }
    char error[160] = "";
    sheet_free(&sheet);
    sheet_init(&sheet);
    if (sheet_load(&sheet, path, error, sizeof(error))) {
        message_box("OpenNumbers", "The workbook could not be opened.", error);
        suite_document(main_window, "");
    } else suite_document(main_window, path);
    reset_view();
    dirty = 0;
    window_paint(main_window);
}

static void saved(const char *path, void *data) {
    if (!path) return;
    char chosen[300], error[160] = "";
    snprintf(chosen, sizeof(chosen), "%s%s", path, extension_of(path)[0] ? "" : ".xlsx");
    if (sheet_save(&sheet, chosen, error, sizeof(error))) { message_box("OpenNumbers", "The workbook could not be saved.", error); return; }
    dirty = 0;
    suite_document(main_window, chosen);
    window_paint(main_window);
}

static void save_as(void) { commit(); file_dialog("Save As (.xlsx, .ods, .csv, .tsv)", 1, 0, saved, 0); }
static void save(void) { commit(); if (suite_path[0]) saved(suite_path, 0); else save_as(); }

static void info(char *out, size_t size) {
    int filled = 0, formulas = 0;
    for (int r = 0; r < SHEET_ROWS; ++r)
        for (int k = 0; k < SHEET_COLS; ++k) {
            const char *s = sheet_get(&sheet, r, k);
            if (*s) ++filled;
            if (*s == '=') ++formulas;
        }
    char last[16] = "-";
    if (sheet.rows && sheet.cols) cell_name(sheet.rows - 1, sheet.cols - 1, last, sizeof(last));
    snprintf(out, size, "Sheets\t1\nUsed range\tA1:%s\nCells with data\t%d\nFormulas\t%d\nChanged\t%s\n", last, filled, formulas, dirty ? "Yes, not saved" : "No");
}

static void copy_cells(int cut) {
    int r1, c1, r2, c2;
    selection(&r1, &c1, &r2, &c2);
    struct text t = {0};
    for (int r = r1; r <= r2; ++r) {
        for (int k = c1; k <= c2; ++k) {
            if (k > c1) text_put(&t, "\t");
            text_put(&t, sheet_get(&sheet, r, k));
            if (cut) sheet_set(&sheet, r, k, "");
        }
        text_put(&t, "\n");
    }
    if (t.data) gui_clipboard_set(t.data, (int)t.size);
    free(t.data);
    if (cut) dirty = 1;
}

static void paste_cells(void) {
    static char buffer[65536];
    int n = gui_clipboard_get(buffer, sizeof(buffer) - 1);
    if (n <= 0 || buffer[0] == 1) return;
    buffer[n] = 0;
    int r = row, k = col;
    char *p = buffer;
    while (*p && r < SHEET_ROWS) {
        char *end = p + strcspn(p, "\t\r\n");
        char saved_char = *end;
        *end = 0;
        if (k < SHEET_COLS) sheet_set(&sheet, r, k, p);
        if (saved_char == '\t') ++k;
        else { ++r; k = col; if (saved_char == '\r' && end[1] == '\n') ++end; }
        if (!saved_char || !end[1]) break;
        p = end + 1;
    }
    dirty = 1;
}

static void clear_cells(void) {
    int r1, c1, r2, c2;
    selection(&r1, &c1, &r2, &c2);
    for (int r = r1; r <= r2; ++r) for (int k = c1; k <= c2; ++k) sheet_set(&sheet, r, k, "");
    dirty = 1;
}

static void insert_sum(const char *name) {
    int r1, c1, r2, c2;
    selection(&r1, &c1, &r2, &c2);
    if (r1 == r2 && c1 == c2) {
        r2 = row - 1;
        r1 = r2;
        while (r1 > 0 && *sheet_get(&sheet, r1 - 1, col)) --r1;
        c1 = c2 = col;
        if (r2 < 0) r1 = r2 = 0;
    } else { row = r2 + 1 < SHEET_ROWS ? r2 + 1 : r2; col = c1; }
    char a[16], b[16], formula[64];
    cell_name(r1, c1, a, sizeof(a));
    cell_name(r2, c2, b, sizeof(b));
    snprintf(formula, sizeof(formula), "=%s(%s:%s)", name, a, b);
    sheet_set(&sheet, row, col, formula);
    mark_row = -1;
    dirty = 1;
}

static void start_function(const char *name) {
    char formula[64];
    snprintf(formula, sizeof(formula), "=%s(", name);
    mark_row = -1;
    start_edit(2, formula);
}

static void found(const char *what, void *data) {
    if (!what) return;
    snprintf(find_text, sizeof(find_text), "%s", what);
    size_t n = strlen(find_text);
    for (int step = 1; step <= SHEET_ROWS * SHEET_COLS; ++step) {
        int at = (row * SHEET_COLS + col + step) % (SHEET_ROWS * SHEET_COLS), r = at / SHEET_COLS, k = at % SHEET_COLS;
        const char *s = sheet_get(&sheet, r, k);
        if (!*s) continue;
        struct value v;
        sheet_value(&sheet, r, k, &v);
        char shown[160];
        value_format(&v, shown, sizeof(shown));
        for (const char *p = shown; *p; ++p)
            if (!strncasecmp(p, find_text, n)) { row = r; col = k; mark_row = -1; keep_visible(main_window); window_paint(main_window); return; }
    }
    message_box("OpenNumbers", "OpenNumbers cannot find the data you're searching for:", find_text);
}

static void go_to(const char *where, void *data) {
    int r, k;
    if (!where || !cell_parse(where, &r, &k) || r >= SHEET_ROWS || k >= SHEET_COLS) return;
    row = r;
    col = k;
    mark_row = -1;
    keep_visible(main_window);
    window_paint(main_window);
}

static const char *list_of(const char *const *names, int n) {
    static char items[256];
    items[0] = 0;
    for (int i = 0; i < n; ++i) { strcat(items, names[i]); strcat(items, "\n"); }
    return items;
}

static const char *menu(int command) {
    switch (command) {
    case C_AUTOSUM: return "Sum\nAverage\nCount Numbers\nMax\nMin\n";
    case C_MATH: return list_of(math_functions, 13);
    case C_LOGICAL: return list_of(logical_functions, 4);
    case C_TEXT_FUNCTIONS: return list_of(text_functions, 4);
    case C_INSERT_FUNCTION: return "SUM\nAVERAGE\nCOUNT\nMIN\nMAX\nIF\nROUND\nCONCAT\n";
    }
    return "";
}

static int state(int command) {
    if (command == C_PASTE) return gui_board()->clipboard_size > 0 ? 0 : RB_OFF;
    return 0;
}

static void command(int command, int choice) {
    commit();
    static const char *const inserted[] = {"SUM", "AVERAGE", "COUNT", "MIN", "MAX", "IF", "ROUND", "CONCAT"};
    switch (command) {
    case C_PASTE: paste_cells(); break;
    case C_CUT: copy_cells(1); break;
    case C_COPY: copy_cells(0); break;
    case C_AUTOSUM: insert_sum(sums[choice >= 0 && choice < 5 ? choice : 0]); break;
    case C_CLEAR: clear_cells(); break;
    case C_FIND: input_box("Find", "Find what:", find_text, found, 0); break;
    case C_SELECT_ALL: mark_row = 0; mark_col = 0; row = sheet.rows ? sheet.rows - 1 : 0; col = sheet.cols ? sheet.cols - 1 : 0; break;
    case C_INSERT_FUNCTION: if (choice >= 0 && choice < 8) start_function(inserted[choice]); break;
    case C_MATH: if (choice >= 0 && choice < 13) start_function(math_functions[choice]); break;
    case C_LOGICAL: if (choice >= 0 && choice < 4) start_function(logical_functions[choice]); break;
    case C_TEXT_FUNCTIONS: if (choice >= 0 && choice < 4) start_function(text_functions[choice]); break;
    case C_FUNCTIONS_HELP:
        message_box("Functions", "SUM AVERAGE MIN MAX COUNT COUNTA IF AND OR NOT ABS ROUND", "SQRT INT MOD POWER PI LEN UPPER LOWER CONCAT; + - * / ^ & % = <> < >");
        break;
    }
    window_paint(main_window);
}

static void popup(struct window *w, int tag, int index) { ribbon_popup(&rb, w, tag, index); }

static void mouse(struct window *w, int x, int y, int kind, int value) {
    if (rb.backstage) { ribbon_mouse(&rb, w, x, y, kind); return; }
    if (kind == M_UP) dragging = 0;
    if (ow_vscroll_mouse(&scroll, scroll_rect(w), x, y, kind)) { top = scroll.top; window_paint(w); return; }
    if (!dragging && ribbon_mouse(&rb, w, x, y, kind)) return;
    struct rect g = grid_rect(w);
    if (kind == M_WHEEL) {
        top -= value * 3;
        if (top < 0) top = 0;
        if (top > SHEET_ROWS - visible_rows(w)) top = SHEET_ROWS - visible_rows(w);
        window_paint(w);
        return;
    }
    if (kind == M_DOWN && rect_has(name_box(), x, y)) { commit(); input_box("Go To", "Reference (such as B12):", "", go_to, 0); return; }
    if (kind == M_DOWN && rect_has(formula_box(w), x, y)) {
        if (!editing) start_edit(2, sheet_get(&sheet, row, col));
        else editing = 2;
        window_paint(w);
        return;
    }
    if (kind == M_DOWN && editing && x >= 116 && x < 132 && y < RIBBON_H + FORMULA_H) { editing = 0; window_paint(w); return; }
    if (kind == M_DOWN && editing && x >= 132 && x < 148 && y < RIBBON_H + FORMULA_H) { commit(); window_paint(w); return; }
    if (kind == M_DOWN && x >= 148 && x < 164 && y < RIBBON_H + FORMULA_H) { window_popup(w, 148, RIBBON_H + FORMULA_H - 4, menu(C_INSERT_FUNCTION), RB_TAG + C_INSERT_FUNCTION); return; }
    if (!rect_has(g, x, y) && !dragging) return;
    if (y < g.y + HEADER_H && kind == M_DOWN && x >= g.x + HEADER_W) {
        commit();
        col = mark_col = left + (x - g.x - HEADER_W) / CELL_W;
        mark_row = 0;
        row = SHEET_ROWS - 1;
        window_paint(w);
        return;
    }
    if (x < g.x + HEADER_W && kind == M_DOWN && y >= g.y + HEADER_H) {
        commit();
        row = mark_row = top + (y - g.y - HEADER_H) / CELL_H;
        mark_col = 0;
        col = SHEET_COLS - 1;
        window_paint(w);
        return;
    }
    int r = top + (y - g.y - HEADER_H) / CELL_H, k = left + (x - g.x - HEADER_W) / CELL_W;
    if (y < g.y + HEADER_H) r = top;
    if (x < g.x + HEADER_W) k = left;
    if (r >= SHEET_ROWS) r = SHEET_ROWS - 1;
    if (k >= SHEET_COLS) k = SHEET_COLS - 1;
    if (kind == M_DOWN) {
        if (editing && edit.text[0] == '=' && (r != row || k != col)) {
            char ref[16];
            cell_name(r, k, ref, sizeof(ref));
            for (const char *p = ref; *p; ++p) field_key(&edit, *p);
        } else {
            commit();
            row = r; col = k;
            mark_row = r; mark_col = k;
            dragging = 1;
        }
    } else if (kind == M_MOVE && dragging && !editing) { row = r; col = k; }
    else if (kind == M_DOUBLE) { commit(); row = r; col = k; mark_row = -1; start_edit(2, sheet_get(&sheet, row, col)); }
    else if (kind != M_UP) return;
    if (kind == M_UP && mark_row == row && mark_col == col) mark_row = -1;
    window_paint(w);
}

static void key(struct window *w, int k) {
    if (ribbon_key(&rb, w, k)) return;
    int shift = dosxc_sys_console(DOSXC_CONSOLE_MODIFIERS, 0, 0) & 1;
    if (k == 19) { save(); return; }
    if (k == 15) { commit(); ribbon_file(&rb, w, 2); return; }
    if (k == 14) { commit(); new_file(); return; }
    if (k == 0x10b) { suite_about(); return; }
    if (editing) {
        if (k == '\n' || k == '\t') { commit(); if (k == '\n') row += shift ? -1 : 1; else col += shift ? -1 : 1; mark_row = -1; }
        else if (k == 27) editing = 0;
        else if (editing == 1 && k >= 0x101 && k <= 0x104 && edit.text[0] != '=') {
            commit();
            if (k == 0x101) --row; else if (k == 0x102) ++row; else if (k == 0x104) --col; else ++col;
            mark_row = -1;
        } else if (k == 0x101 || k == 0x102) { commit(); row += k == 0x101 ? -1 : 1; mark_row = -1; }
        else if (k == 0x10c) editing = editing == 1 ? 2 : 1;
        else field_key(&edit, k);
    } else {
        int moved = 1;
        if (shift && mark_row < 0 && ((k >= 0x101 && k <= 0x104) || k == 0x109 || k == 0x10a)) { mark_row = row; mark_col = col; }
        if (k == 0x101) --row;
        else if (k == 0x102 || k == '\n') ++row;
        else if (k == 0x104) --col;
        else if (k == 0x103 || k == '\t') ++col;
        else if (k == 0x109) row -= visible_rows(w);
        else if (k == 0x10a) row += visible_rows(w);
        else if (k == 0x105) col = 0;
        else moved = 0;
        if (moved && !shift) mark_row = -1;
        if (!moved) {
            if (k == 0x108) clear_cells();
            else if (k == 3) copy_cells(0);
            else if (k == 24) copy_cells(1);
            else if (k == 22) paste_cells();
            else if (k == 1) command(C_SELECT_ALL, -1);
            else if (k == 6) command(C_FIND, -1);
            else if (k == 0x10d && find_text[0]) found(find_text, 0);
            else if (k == 0x10c) start_edit(2, sheet_get(&sheet, row, col));
            else if (k == '\b') start_edit(1, "");
            else if (k >= 32 && k < 256 && k != 127) { start_edit(1, ""); field_key(&edit, k); mark_row = -1; }
        }
    }
    if (row < 0) row = 0;
    if (row >= SHEET_ROWS) row = SHEET_ROWS - 1;
    if (col < 0) col = 0;
    if (col >= SHEET_COLS) col = SHEET_COLS - 1;
    keep_visible(w);
    window_paint(w);
}

static void ask_save(int answer, void *data) {
    if (answer == 1) save();
    if (answer == 0 || (answer == 1 && !dirty)) { dirty = 0; window_close(main_window); }
}

static int closing(struct window *w) {
    commit();
    if (!dirty) return 1;
    ask_box("OpenNumbers", "Save the changes to this workbook?", "Save", "Don't Save", ask_save, 0);
    return 0;
}

static const struct app_ops ops = {paint, mouse, key, 0, popup, 0, 0, closing};

static const struct suite_app numbers = {
    "OpenNumbers", "NUMBERS", "Work with Excel, OpenDocument, CSV and TSV workbooks.",
    "Book1", "workbook", "Workbooks",
    "Excel Workbook (.xlsx)\nOpenDocument Spreadsheet (.ods)\nCSV, comma separated (.csv)\nText, tab separated (.tsv)\n\nType a name ending in one of these;\nwith no ending the workbook is saved as .xlsx.",
    0x217346, 0xffffff, BS_NEW | BS_OPEN | BS_SAVE | BS_SAVE_AS,
    new_file, open_file, save, save_as, info, 0,
};

int main(int argc, char **argv) {
    if (gui_connect()) { puts("OpenNumbers needs the DOS-XC desktop."); return 1; }
    suite_start(&numbers, argv[0]);
    rb = (struct ribbon){tabs, 2, 0, command, state, menu};
    rb.hover = -1;
    sheet_init(&sheet);
    int width = gui_board()->screen_w - 80, height = gui_board()->screen_h - 120;
    if (width > 920) width = 920;
    if (height > 640) height = 640;
    main_window = window_open(width < 560 ? 560 : width, height < 400 ? 400 : height, "Book1 - OpenNumbers", ICON_CALC, &ops, 0);
    if (!main_window) return 1;
    main_window->resizable = 1;
    main_window->track_mouse = 1;
    if (argc > 1 && argv[1][0]) open_file(argv[1]);
    gui_run();
    return 0;
}
