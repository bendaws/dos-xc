/*
 * DOS-XC /dosoffice/sheets.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "wm.h"
#include "sheet.h"
#include <strings.h>

#define BAR_H 26
#define HEADER_W 40
#define HEADER_H 18
#define CELL_W 84
#define CELL_H 18
#define STATUS_H 18

static struct sheet sheet;
static struct window *main_window;
static char file_path[300];
static int dirty, top, left, row, col, editing, mark_row = -1, mark_col;
static struct field edit;

static const struct menubar menu = {{"File", "Edit", "Insert", "Help"},
    {"New\nOpen...\nSave\nSave As...\n-\nExit\n", "Copy\nCut\nPaste\nClear\n", "Sum\nAverage\nCount\nMinimum\nMaximum\n", "Functions\nAbout\n"}};

static struct rect grid_rect(struct window *w) {
    return (struct rect){0, MENUBAR_H + BAR_H, w->r.w, w->r.h - MENUBAR_H - BAR_H - STATUS_H};
}
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

static void paint(struct window *w, const struct canvas *c) {
    menubar_paint(c, &menu, w->r.w);
    int y0 = MENUBAR_H;
    fill(c, 0, y0, w->r.w, BAR_H, C_WINDOW);
    char name[16];
    cell_name(row, col, name, sizeof(name));
    bevel(c, 4, y0 + 3, 56, 20, 1);
    text(c, 10, y0 + 7, &font_regular, name, C_TEXT);
    text(c, 66, y0 + 7, &font_bold, "fx", C_TEXT);
    if (!editing) field_set(&edit, sheet_get(&sheet, row, col));
    field_paint(c, &edit, (struct rect){86, y0 + 3, w->r.w - 92, 20}, editing);
    struct rect g = grid_rect(w);
    fill(c, g.x, g.y, g.w, g.h, 0xffffff);
    int rows = visible_rows(w) + 1, cols = visible_cols(w) + 1, r1, c1, r2, c2;
    selection(&r1, &c1, &r2, &c2);
    fill(c, g.x, g.y, g.w, HEADER_H, C_WINDOW);
    fill(c, g.x, g.y, HEADER_W, g.h, C_WINDOW);
    struct canvas cells = *c;
    cells.clip = rect_both(c->clip, (struct rect){g.x + HEADER_W, g.y + HEADER_H, g.w - HEADER_W, g.h - HEADER_H});
    for (int i = 0; i < cols && left + i < SHEET_COLS; ++i) {
        int x = g.x + HEADER_W + i * CELL_W;
        char label[4];
        column_name(left + i, label);
        if (left + i >= c1 && left + i <= c2) fill(c, x, g.y, CELL_W, HEADER_H, 0xc8c4bc);
        text(c, x + (CELL_W - text_width(&font_bold, label)) / 2, g.y + 3, &font_bold, label, C_TEXT);
        fill(c, x + CELL_W - 1, g.y, 1, g.h, 0xd8d8d8);
    }
    for (int j = 0; j < rows && top + j < SHEET_ROWS; ++j) {
        int y = g.y + HEADER_H + j * CELL_H;
        char label[8];
        snprintf(label, sizeof(label), "%d", top + j + 1);
        if (top + j >= r1 && top + j <= r2) fill(c, g.x, y, HEADER_W, CELL_H, 0xc8c4bc);
        text(c, g.x + HEADER_W - 6 - text_width(&font_regular, label), y + 3, &font_regular, label, C_TEXT);
        fill(&cells, g.x, y + CELL_H - 1, g.w, 1, 0xd8d8d8);
        for (int i = 0; i < cols && left + i < SHEET_COLS; ++i) {
            int r = top + j, k = left + i, x = g.x + HEADER_W + i * CELL_W;
            if (r >= r1 && r <= r2 && k >= c1 && k <= c2 && (r1 != r2 || c1 != c2)) fill(&cells, x, y, CELL_W - 1, CELL_H - 1, 0xdce8fa);
            if (!*sheet_get(&sheet, r, k)) continue;
            struct value v;
            sheet_value(&sheet, r, k, &v);
            char shown[160];
            value_format(&v, shown, sizeof(shown));
            struct canvas one = cells;
            int tw = text_width(&font_regular, shown);
            int right_align = v.kind == VALUE_NUMBER;
            int extra = 0;
            if (!right_align) while (tw > CELL_W * (extra + 1) - 6 && k + extra + 1 < SHEET_COLS && !*sheet_get(&sheet, r, k + extra + 1)) ++extra;
            one.clip = rect_both(cells.clip, (struct rect){x, y, CELL_W * (extra + 1) - 2, CELL_H});
            if (extra) fill(&one, x, y, CELL_W * (extra + 1) - 1, CELL_H - 1, r >= r1 && r <= r2 && k >= c1 && k <= c2 && (r1 != r2 || c1 != c2) ? 0xdce8fa : 0xffffff);
            u32 color = v.kind == VALUE_ERROR ? 0xc00000 : C_TEXT;
            text(&one, right_align ? x + CELL_W - 4 - tw : x + 3, y + 3, &font_regular, shown, color);
        }
    }
    fill(c, g.x + HEADER_W - 1, g.y, 1, g.h, C_SHADOW1);
    fill(c, g.x, g.y + HEADER_H - 1, g.w, 1, C_SHADOW1);
    if (row >= top && row < top + rows && col >= left && col < left + cols) {
        int x = g.x + HEADER_W + (col - left) * CELL_W, y = g.y + HEADER_H + (row - top) * CELL_H;
        frame_box(&cells, x - 1, y - 1, CELL_W + 1, CELL_H + 1, 0x1f4f9f);
        frame_box(&cells, x, y, CELL_W - 1, CELL_H - 1, 0x1f4f9f);
    }
    int sy = w->r.h - STATUS_H;
    fill(c, 0, sy, w->r.w, STATUS_H, C_WINDOW);
    char status[200] = "";
    if (r1 != r2 || c1 != c2) {
        double sum = 0;
        int count = 0;
        for (int r = r1; r <= r2; ++r)
            for (int k = c1; k <= c2; ++k) {
                struct value v;
                sheet_value(&sheet, r, k, &v);
                if (v.kind == VALUE_NUMBER) { sum += v.number; ++count; }
            }
        snprintf(status, sizeof(status), "Sum: %.10g   Average: %.10g   Count: %d", sum, count ? sum / count : 0.0, count);
    } else snprintf(status, sizeof(status), "%s%s", file_path[0] ? file_path : "Untitled", dirty ? " (changed)" : "");
    text(c, 6, sy + 3, &font_regular, status, C_TEXT);
}

static void set_title(void) {
    char title[96];
    const char *name = strrchr(file_path, '\\');
    snprintf(title, sizeof(title), "%s - DOS Sheets", file_path[0] ? (name ? name + 1 : file_path) : "Untitled");
    window_title(main_window, title);
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

static void loaded(const char *path, void *data) {
    (void)data;
    if (!path) return;
    if (!strcasecmp(extension_of(path), "gsheet")) { open_in_browser(path); return; }
    char error[160] = "";
    if (sheet_load(&sheet, path, error, sizeof(error))) {
        message_box("DOS Sheets", "The workbook could not be opened.", error);
        file_path[0] = 0;
    } else snprintf(file_path, sizeof(file_path), "%s", path);
    row = col = top = left = 0;
    mark_row = -1;
    dirty = 0;
    set_title();
    window_paint(main_window);
}

static void saved(const char *path, void *data) {
    (void)data;
    if (!path) return;
    char chosen[300], error[160] = "";
    snprintf(chosen, sizeof(chosen), "%s%s", path, extension_of(path)[0] ? "" : ".xlsx");
    if (sheet_save(&sheet, chosen, error, sizeof(error))) { message_box("DOS Sheets", "The workbook could not be saved.", error); return; }
    snprintf(file_path, sizeof(file_path), "%s", chosen);
    dirty = 0;
    set_title();
    window_paint(main_window);
}

static void save(void) {
    commit();
    if (file_path[0]) saved(file_path, 0);
    else file_dialog("Save As (.xlsx, .ods, .csv, .tsv)", 1, 0, saved, 0);
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
    while (*p) {
        char *end = p + strcspn(p, "\t\r\n");
        char saved_char = *end;
        *end = 0;
        sheet_set(&sheet, r, k, p);
        if (saved_char == '\t') ++k;
        else { ++r; k = col; if (saved_char == '\r' && end[1] == '\n') ++end; }
        if (!saved_char) break;
        p = end + 1;
    }
    dirty = 1;
}

static void insert_function(const char *name) {
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

static void popup(struct window *w, int tag, int index) {
    commit();
    if (tag == 0) {
        if (index == 0) { sheet_free(&sheet); file_path[0] = 0; row = col = top = left = 0; dirty = 0; set_title(); }
        else if (index == 1) file_dialog("Open a workbook", 0, 0, loaded, 0);
        else if (index == 2) save();
        else if (index == 3) file_dialog("Save As (.xlsx, .ods, .csv, .tsv)", 1, 0, saved, 0);
        else if (index == 5) window_close(w);
    } else if (tag == 1) {
        if (index == 0) copy_cells(0);
        else if (index == 1) copy_cells(1);
        else if (index == 2) paste_cells();
        else if (index == 3) {
            int r1, c1, r2, c2;
            selection(&r1, &c1, &r2, &c2);
            for (int r = r1; r <= r2; ++r) for (int k = c1; k <= c2; ++k) sheet_set(&sheet, r, k, "");
            dirty = 1;
        }
    } else if (tag == 2 && index >= 0) {
        static const char *const names[] = {"SUM", "AVERAGE", "COUNT", "MIN", "MAX"};
        insert_function(names[index]);
    } else if (tag == 3 && index == 0)
        message_box("Functions", "SUM AVERAGE MIN MAX COUNT COUNTA IF AND OR NOT ABS ROUND", "SQRT INT MOD POWER PI LEN UPPER LOWER CONCAT; + - * / ^ & % = <> < >");
    else if (tag == 3 && index == 1)
        message_box("About DOS Sheets", "DOS Sheets, part of DOS Office for DOS-XC.", "Excel (.xlsx), OpenDocument (.ods), CSV and TSV workbooks.");
    window_paint(w);
}

static void mouse(struct window *w, int x, int y, int kind, int value) {
    if (menubar_mouse(w, &menu, x, y, kind)) return;
    struct rect g = grid_rect(w);
    if (kind == M_WHEEL) {
        top -= value * 3;
        if (top < 0) top = 0;
        if (top > SHEET_ROWS - 1) top = SHEET_ROWS - 1;
        window_paint(w);
        return;
    }
    if (y >= MENUBAR_H && y < MENUBAR_H + BAR_H && kind == M_DOWN && x >= 86) {
        if (!editing) { editing = 1; field_set(&edit, sheet_get(&sheet, row, col)); }
        window_paint(w);
        return;
    }
    if (!rect_has(g, x, y) || x < g.x + HEADER_W || y < g.y + HEADER_H) return;
    int r = top + (y - g.y - HEADER_H) / CELL_H, k = left + (x - g.x - HEADER_W) / CELL_W;
    if (r >= SHEET_ROWS || k >= SHEET_COLS) return;
    if (kind == M_DOWN) {
        if (editing && edit.text[0] == '=' && (r != row || k != col)) {
            char ref[16];
            cell_name(r, k, ref, sizeof(ref));
            for (const char *p = ref; *p; ++p) field_key(&edit, *p);
        } else {
            commit();
            row = r; col = k;
            mark_row = r; mark_col = k;
        }
    } else if (kind == M_MOVE && mark_row >= 0 && !editing) { row = r; col = k; }
    else if (kind == M_DOUBLE) { commit(); row = r; col = k; editing = 1; field_set(&edit, sheet_get(&sheet, row, col)); }
    window_paint(w);
}

static void key(struct window *w, int k) {
    int shift = dosxc_sys_console(DOSXC_CONSOLE_MODIFIERS, 0, 0) & 1;
    if (k == 19) { save(); return; }
    if (k == 15) { commit(); file_dialog("Open a workbook", 0, 0, loaded, 0); return; }
    if (editing) {
        if (k == '\n' || k == '\t') { commit(); if (k == '\n') ++row; else ++col; mark_row = -1; }
        else if (k == 27) editing = 0;
        else if (k == 0x101 || k == 0x102) { commit(); row += k == 0x101 ? -1 : 1; mark_row = -1; }
        else field_key(&edit, k);
    } else {
        int moved = 1;
        if (shift && mark_row < 0 && k >= 0x101 && k <= 0x104) { mark_row = row; mark_col = col; }
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
            if (k == 0x108) { popup(w, 1, 3); return; }
            else if (k == 3) copy_cells(0);
            else if (k == 24) copy_cells(1);
            else if (k == 22) paste_cells();
            else if (k == 0x10c) { editing = 1; field_set(&edit, sheet_get(&sheet, row, col)); }
            else if (k == '\b') { editing = 1; field_set(&edit, ""); }
            else if (k >= 32 && k < 256 && k != 127) { editing = 1; field_set(&edit, ""); field_key(&edit, k); mark_row = -1; }
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
    (void)data;
    if (answer == 1) save();
    if (answer == 0 || (answer == 1 && !dirty)) { dirty = 0; window_close(main_window); }
}

static int closing(struct window *w) {
    (void)w;
    commit();
    if (!dirty) return 1;
    ask_box("DOS Sheets", "Save the changes to this workbook?", "Save", "Don't Save", ask_save, 0);
    return 0;
}

static const struct app_ops ops = {paint, mouse, key, 0, popup, 0, 0, closing};

int main(int argc, char **argv) {
    if (gui_connect()) { puts("DOS Sheets needs the DOS-XC desktop."); return 1; }
    char icon[260];
    snprintf(icon, sizeof(icon), "%s", argv[0]);
    char *slash = strrchr(icon, '\\');
    if (slash) { strcpy(slash + 1, "SHEETS.PNG"); gui_set_icon_file(icon); }
    sheet_init(&sheet);
    main_window = window_open(640, 440, "Untitled - DOS Sheets", ICON_CALC, &ops, 0);
    if (!main_window) return 1;
    main_window->resizable = 1;
    if (argc > 1) loaded(argv[1], 0);
    gui_run();
    return 0;
}
