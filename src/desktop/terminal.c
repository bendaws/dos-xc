/*
 * DOS-XC /desktop/terminal.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * TERMINAL.EXE: the DOS Prompt as a window.  COMMAND.EXE and everything it
 * starts run on a virtual console; this program draws what they write, an
 * 80 x 25 VGA-style screen with ANSI escape sequences and CP437 box
 * characters, and feeds them the keys.
 */
#include "wm.h"
#include "../vdm/screen.h"

#define ROWS 25
#define COLS 80
#define SCROLLBACK 500
#define BAR_W 14

struct cell { unsigned char ch, attr; };
static struct cell screen[ROWS][COLS], history[SCROLLBACK][COLS];
static int history_count, history_head;         /* a ring of old lines */
static int view_up;                             /* lines scrolled back */
static int row, column, saved_row, saved_column, pending_wrap;
static int top_margin, bottom_margin = ROWS - 1;
static unsigned char attribute = 7, base_attribute = 7;
static int bold, reverse_video, foreground = 7, background;
static int parser, params[8], param_count, private_mode;
static int console = -1, shell_pid, one_shot, finished, cursor_on = 1;
static struct vscroll scroll;
static struct window *main_w;
static int cell_w, cell_h;
/* A 16-bit program showing its screen here instead of this text (src/vdm). */
static struct vdm_screen *vm;
static unsigned vm_seen_frame;
static int vm_graphics, vm_width, vm_height, vm_right_ticks;
static char osc[32];
static int osc_length;

static const u32 vga[16] = {0x000000, 0x0000aa, 0x00aa00, 0x00aaaa, 0xaa0000, 0xaa00aa, 0xaa5500, 0xaaaaaa,
                            0x555555, 0x5555ff, 0x55ff55, 0x55ffff, 0xff5555, 0xff55ff, 0xffff55, 0xffffff};

/* CP437 0x80-0xAF as Latin-1, which is what the font has. */
static const unsigned char accents[48] = {
    0xC7, 0xFC, 0xE9, 0xE2, 0xE4, 0xE0, 0xE5, 0xE7, 0xEA, 0xEB, 0xE8, 0xEF, 0xEE, 0xEC, 0xC4, 0xC5,
    0xC9, 0xE6, 0xC6, 0xF4, 0xF6, 0xF2, 0xFB, 0xF9, 0xFF, 0xD6, 0xDC, 0xA2, 0xA3, 0xA5, '?', '?',
    0xE1, 0xED, 0xF3, 0xFA, 0xF1, 0xD1, 0xAA, 0xBA, 0xBF, '?', 0xAC, 0xBD, 0xBC, 0xA1, 0xAB, 0xBB};

/* CP437 0xB3-0xDA: the line weight going up, down, left and right. */
static const char lines[40][5] = {
    "1100", "1110", "1120", "2210", "0210", "0120", "2220", "2200", "0220", "2020", "2010", "1020", "0110",
    "1001", "1011", "0111", "1101", "0011", "1111", "1102", "2201", "2002", "0202", "2022", "0222", "2202",
    "0022", "2222", "1022", "2011", "0122", "0211", "2001", "1002", "0102", "0201", "2211", "1122", "1010", "0101"};

/* ---- the screen ----------------------------------------------------------- */

static void clear_cells(struct cell *c, int count) {
    for (int i = 0; i < count; ++i) c[i] = (struct cell){' ', attribute};
}

static void set_colors(void) {
    int fg = foreground | (bold ? 8 : 0), bg = background;
    attribute = (unsigned char)(reverse_video ? (fg & 7) << 4 | bg : bg << 4 | fg);
}

static void scroll_region(int top, int bottom, int up) {
    if (up) {
        if (top == 0 && bottom == ROWS - 1) {
            memcpy(history[history_head], screen[0], sizeof(screen[0]));
            history_head = (history_head + 1) % SCROLLBACK;
            if (history_count < SCROLLBACK) ++history_count;
        }
        memmove(screen[top], screen[top + 1], (size_t)(bottom - top) * sizeof(screen[0]));
        clear_cells(screen[bottom], COLS);
    } else {
        memmove(screen[top + 1], screen[top], (size_t)(bottom - top) * sizeof(screen[0]));
        clear_cells(screen[top], COLS);
    }
}

static void line_feed(void) {
    pending_wrap = 0;
    if (row == bottom_margin) scroll_region(top_margin, bottom_margin, 1);
    else if (row < ROWS - 1) ++row;
}

static void move_to(int r, int c) {
    row = r < 0 ? 0 : r >= ROWS ? ROWS - 1 : r;
    column = c < 0 ? 0 : c >= COLS ? COLS - 1 : c;
    pending_wrap = 0;
}

static void put_char(unsigned char ch) {
    if (pending_wrap) {
        pending_wrap = 0;
        column = 0;
        line_feed();
    }
    screen[row][column] = (struct cell){ch, attribute};
    if (column == COLS - 1) pending_wrap = 1;
    else ++column;
}

static int param(int i, int fallback) { return i < param_count && params[i] > 0 ? params[i] : fallback; }

static void push_text(const char *text) {
    int keys[32], n = 0;
    while (*text && n < 32) keys[n++] = (unsigned char)*text++;
    dosxc_sys_task(DOSXC_TASK_CONSOLE_KEYS, keys, console, n);
}

static void select_graphic(void) {
    static const unsigned char to_vga[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    if (!param_count) params[param_count++] = 0;
    for (int i = 0; i < param_count; ++i) {
        int p = params[i] < 0 ? 0 : params[i];
        if (p == 0) {
            bold = reverse_video = 0;
            foreground = base_attribute & 7;
            background = base_attribute >> 4 & 7;
            bold = base_attribute >> 3 & 1;
        } else if (p == 1) bold = 1;
        else if (p == 22) bold = 0;
        else if (p == 7) reverse_video = 1;
        else if (p == 27) reverse_video = 0;
        else if (p >= 30 && p <= 37) foreground = to_vga[p - 30];
        else if (p == 39) foreground = base_attribute & 7;
        else if (p >= 40 && p <= 47) background = to_vga[p - 40];
        else if (p == 49) background = base_attribute >> 4 & 7;
        else if (p >= 90 && p <= 97) { foreground = to_vga[p - 90]; bold = 1; }
        else if (p >= 100 && p <= 107) background = to_vga[p - 100];
    }
    set_colors();
}

static void erase_display(int mode) {
    int at = row * COLS + column;
    if (mode == 0) { clear_cells(&screen[row][column], COLS - column); for (int r = row + 1; r < ROWS; ++r) clear_cells(screen[r], COLS); }
    else if (mode == 1) { for (int r = 0; r < row; ++r) clear_cells(screen[r], COLS); clear_cells(screen[row], column + 1); }
    else for (int r = 0; r < ROWS; ++r) clear_cells(screen[r], COLS);
    (void)at;
}

static void erase_line(int mode) {
    if (mode == 0) clear_cells(&screen[row][column], COLS - column);
    else if (mode == 1) clear_cells(screen[row], column + 1);
    else clear_cells(screen[row], COLS);
}

static void control(char final) {
    int n = param(0, 1);
    switch (final) {
    case 'A': move_to(row - n, column); break;
    case 'B': case 'e': move_to(row + n, column); break;
    case 'C': case 'a': move_to(row, column + n); break;
    case 'D': move_to(row, column - n); break;
    case 'E': move_to(row + n, 0); break;
    case 'F': move_to(row - n, 0); break;
    case 'G': case '`': move_to(row, n - 1); break;
    case 'd': move_to(n - 1, column); break;
    case 'H': case 'f': move_to(param(0, 1) - 1, param(1, 1) - 1); break;
    case 'J': erase_display(param_count && params[0] > 0 ? params[0] : 0); break;
    case 'K': erase_line(param_count && params[0] > 0 ? params[0] : 0); break;
    case 'X': { int count = n < COLS - column ? n : COLS - column; clear_cells(&screen[row][column], count); break; }
    case 'L': case 'M':
        if (row >= top_margin && row <= bottom_margin)
            for (int i = 0; i < n && i <= bottom_margin - row; ++i) scroll_region(row, bottom_margin, final == 'M');
        move_to(row, 0);
        break;
    case 'S': case 'T':
        for (int i = 0; i < n; ++i) scroll_region(top_margin, bottom_margin, final == 'S');
        break;
    case 'r': {
        int top = param(0, 1) - 1, bottom = param(1, ROWS) - 1;
        if (bottom >= ROWS) bottom = ROWS - 1;
        if (top < bottom) { top_margin = top; bottom_margin = bottom; }
        move_to(0, 0);
        break;
    }
    case 'm': select_graphic(); break;
    case 's': saved_row = row; saved_column = column; break;
    case 'u': move_to(saved_row, saved_column); break;
    case 'n':
        if (param(0, 0) == 6) {
            char text[32];
            snprintf(text, sizeof(text), "\033[%d;%dR", row + 1, column + 1);
            push_text(text);
        } else if (param(0, 0) == 5) push_text("\033[0n");
        break;
    case 'c': if (!private_mode) push_text("\033[?1;0c"); break;
    case 'z': {                         /* DOS colors: attribute ; 1 = the whole screen */
        unsigned char a = (unsigned char)(param_count && params[0] >= 0 ? params[0] : 7);
        attribute = a;
        foreground = a & 7;
        bold = a >> 3 & 1;
        background = a >> 4 & 7;
        reverse_video = 0;
        if (param(1, 0)) {
            base_attribute = a;
            for (int r = 0; r < ROWS; ++r)
                for (int c = 0; c < COLS; ++c) screen[r][c].attr = a;
        }
        break;
    }
    }
}

static void vm_size_window(void);

/* "8086;address": the machine's shared screen, or 0 when it has finished. */
static void vm_announce(const char *text) {
    if (strncmp(text, "8086;", 5)) return;
    uintptr_t address = strtoul(text + 5, 0, 16);
    vm = (struct vdm_screen *)address;
    vm_graphics = vm_width = vm_height = -1;
    main_w->track_mouse = vm != 0;
    if (!vm) { vm_right_ticks = 0; window_request_size(main_w, COLS * cell_w + 4 + BAR_W, ROWS * cell_h + 4); }
    else vm_size_window();
    window_paint(main_w);
}

static void feed(unsigned char b) {
    if (parser == 1) {
        parser = 0;
        if (b == '[') {
            parser = 2;
            param_count = 0;
            private_mode = 0;
            for (int i = 0; i < 8; ++i) params[i] = -1;
        } else if (b == '7') { saved_row = row; saved_column = column; }
        else if (b == '8') move_to(saved_row, saved_column);
        else if (b == 'M') { if (row == top_margin) scroll_region(top_margin, bottom_margin, 0); else move_to(row - 1, column); }
        else if (b == 'D') line_feed();
        else if (b == 'E') { column = 0; line_feed(); }
        else if (b == 'c') {
            top_margin = 0;
            bottom_margin = ROWS - 1;
            params[0] = 0;
            param_count = 1;
            select_graphic();
            erase_display(2);
            move_to(0, 0);
        } else if (b == '(' || b == ')' || b == '#') parser = 3;
        else if (b == ']') { parser = 4; osc_length = 0; }
        return;
    }
    if (parser == 3) { parser = 0; return; }
    if (parser == 4) {                  /* ESC ] ... BEL: a virtual machine's screen */
        if (b == 7 || b == 27) { parser = b == 27 ? 1 : 0; osc[osc_length] = 0; vm_announce(osc); return; }
        if (osc_length + 1 < (int)sizeof(osc)) osc[osc_length++] = (char)b;
        return;
    }
    if (parser == 2) {
        if (b >= '0' && b <= '9') {
            if (!param_count) param_count = 1;
            int *p = &params[param_count - 1];
            *p = (*p < 0 ? 0 : *p) * 10 + (b - '0');
            if (*p > 9999) *p = 9999;
        } else if (b == ';') {
            if (!param_count) param_count = 1;
            if (param_count < 8) ++param_count;
        } else if (b == '?' || b == '>' || b == '=') private_mode = 1;
        else if (b < 0x20 || b >= 0x40) { parser = 0; control((char)b); }
        return;
    }
    switch (b) {
    case 27: parser = 1; return;
    case '\r': column = 0; pending_wrap = 0; return;
    case '\n': case 11: case 12: column = 0; line_feed(); return;   /* as the DOS console does */
    case '\b': if (pending_wrap) pending_wrap = 0; else if (column) --column; return;
    case '\t': { int next = (column / 8 + 1) * 8; move_to(row, next < COLS ? next : COLS - 1); return; }
    case 7: case 0: case 14: case 15: return;
    }
    if (b >= 32 && b != 127) put_char(b);
}

/* ---- drawing -------------------------------------------------------------- */

static void line_part(const struct canvas *c, int x, int y, int w, int h, int weight, int horizontal, u32 color) {
    if (!weight) return;
    if (weight == 1) { fill(c, x, y, w, h, color); return; }
    if (horizontal) { fill(c, x, y - 1, w, 1, color); fill(c, x, y + 1, w, 1, color); }
    else { fill(c, x - 1, y, 1, h, color); fill(c, x + 1, y, 1, h, color); }
}

static void draw_cell(const struct canvas *c, int x, int y, struct cell cell) {
    u32 fg = vga[cell.attr & 15], bg = vga[cell.attr >> 4 & 7];
    fill(c, x, y, cell_w, cell_h, bg);
    unsigned ch = cell.ch;
    if (ch == ' ' || !ch) return;
    int cx = x + cell_w / 2, cy = y + cell_h / 2;
    if (ch >= 0xb0 && ch <= 0xb2) {                 /* shades */
        int every = ch == 0xb0 ? 4 : ch == 0xb1 ? 2 : 1;
        for (int j = 0; j < cell_h; ++j)
            for (int i = (j & 1) * (every / 2); i < cell_w; i += every)
                fill(c, x + i, y + j, 1, 1, fg);
        return;
    }
    if (ch >= 0xb3 && ch <= 0xda) {
        const char *w = lines[ch - 0xb3];
        line_part(c, cx, y, 1, cy - y + 1, w[0] - '0', 0, fg);
        line_part(c, cx, cy, 1, y + cell_h - cy, w[1] - '0', 0, fg);
        line_part(c, x, cy, cx - x + 1, 1, w[2] - '0', 1, fg);
        line_part(c, cx, cy, x + cell_w - cx, 1, w[3] - '0', 1, fg);
        return;
    }
    switch (ch) {
    case 0xdb: fill(c, x, y, cell_w, cell_h, fg); return;
    case 0xdc: fill(c, x, y + cell_h / 2, cell_w, cell_h - cell_h / 2, fg); return;
    case 0xdd: fill(c, x, y, cell_w / 2, cell_h, fg); return;
    case 0xde: fill(c, x + cell_w / 2, y, cell_w - cell_w / 2, cell_h, fg); return;
    case 0xdf: fill(c, x, y, cell_w, cell_h / 2, fg); return;
    }
    if (ch >= 0x80 && ch <= 0xaf) ch = accents[ch - 0x80];
    else if (ch >= 0xe0) ch = ch == 0xe1 ? 0xdf : ch == 0xf8 ? 0xb0 : ch == 0xf1 ? 0xb1 : ch == 0xfd ? 0xb2 : ch == 0xfa ? 0xb7 : '?';
    char s[2] = {(char)ch, 0};
    text(c, x + (cell_w - font_mono.widths[ch]) / 2, y + (cell_h - font_mono.height) / 2, &font_mono, s, fg);
}

static void vm_size_window(void) {
    if (!vm) return;
    if (vm->magic != VDM_MAGIC) { vm = 0; return; }
    if (vm->graphics == vm_graphics && vm->width == vm_width && vm->height == vm_height) return;
    vm_graphics = vm->graphics;
    vm_width = vm->width;
    vm_height = vm->height;
    if (vm_graphics) {
        int scale = vm_width <= 320 ? 2 : 1;
        window_request_size(main_w, vm_width * scale, vm_height * scale);
    } else window_request_size(main_w, COLS * cell_w + 4 + BAR_W, ROWS * cell_h + 4);
}

static void vm_paint(struct window *w, const struct canvas *c) {
    fill(c, 0, 0, w->content.w, w->content.h, 0);
    if (!vm->graphics) {
        int rows = vm->height, cols = vm->width;
        const struct cell *cells = (const struct cell *)vm->text;
        for (int r = 0; r < rows && r < ROWS; ++r)
            for (int k = 0; k < cols && k < COLS; ++k) draw_cell(c, 2 + k * cell_w, 2 + r * cell_h, cells[r * cols + k]);
        if (vm->cursor_on && cursor_on) fill(c, 2 + vm->cursor_col * cell_w, 2 + vm->cursor_row * cell_h + cell_h - 2, cell_w, 2, 0xaaaaaa);
        return;
    }
    int width = vm->width, height = vm->height;
    if (width <= 0 || height <= 0) return;
    int sx = w->content.w / width, sy = w->content.h / height, scale = sx < sy ? sx : sy;
    if (scale < 1) scale = 1;
    int ox = (w->content.w - width * scale) / 2, oy = (w->content.h - height * scale) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    const u32 *pixels = vm->pixels;
    u32 *dst = c->s->px;
    int stride = c->s->w;
    for (int y = 0; y < height; ++y) {
        const u32 *from = pixels + y * width;
        for (int repeat = 0; repeat < scale; ++repeat) {
            int row = oy + y * scale + repeat;
            if (row >= w->content.h) break;
            u32 *line = dst + row * stride + ox;
            int columns = width * scale < w->content.w - ox ? width : (w->content.w - ox) / scale;
            if (scale == 1) { memcpy(line, from, (size_t)columns * 4); continue; }
            for (int x = 0; x < columns; ++x)
                for (int i = 0; i < scale; ++i) *line++ = from[x];
        }
    }
}

static const struct cell *line_at(int view_row) {
    /* VIEW_ROW counts from the top of the window, view_up lines back. */
    int back = view_up - view_row;
    if (back <= 0) return screen[view_row - view_up];
    return history[(history_head - back + SCROLLBACK * 2) % SCROLLBACK];
}

static void term_paint(struct window *w, const struct canvas *c) {
    if (vm) { vm_paint(w, c); return; }
    fill(c, 0, 0, w->content.w, w->content.h, 0);
    for (int r = 0; r < ROWS; ++r) {
        const struct cell *line = line_at(r);
        for (int k = 0; k < COLS; ++k) draw_cell(c, 2 + k * cell_w, 2 + r * cell_h, line[k]);
    }
    if (!view_up && cursor_on && !finished) fill(c, 2 + column * cell_w, 2 + row * cell_h + cell_h - 2, cell_w, 2, vga[attribute & 15]);
    scroll.total = history_count + ROWS;
    scroll.visible = ROWS;
    scroll.top = history_count - view_up;
    vscroll_paint(c, &scroll, (struct rect){w->content.w - BAR_W, 0, BAR_W, w->content.h});
}

/* ---- input ----------------------------------------------------------------- */

static void send_key(int key) {
    if (console >= 0) dosxc_sys_task(DOSXC_TASK_CONSOLE_KEYS, &key, console, 1);
}

static void term_key(struct window *w, int key) {
    if (finished) { window_close(w); return; }
    if (vm) { send_key(key); return; }
    if (view_up) view_up = 0;
    send_key(key);
    window_paint(w);
}

static void set_view(struct window *w, int up) {
    view_up = up < 0 ? 0 : up > history_count ? history_count : up;
    window_paint(w);
}

static void vm_mouse(struct window *w, int x, int y, int kind) {
    if (!vm) return;
    int gx, gy;
    if (vm->graphics) {
        int sx = w->content.w / vm->width, sy = w->content.h / vm->height, scale = sx < sy ? sx : sy;
        if (scale < 1) scale = 1;
        gx = (x - (w->content.w - vm->width * scale) / 2) / scale;
        gy = (y - (w->content.h - vm->height * scale) / 2) / scale;
    } else {
        gx = (x - 2) / cell_w;
        gy = (y - 2) / cell_h;
    }
    vm->mouse_x = gx < 0 ? 0 : gx >= vm->width ? vm->width - 1 : gx;
    vm->mouse_y = gy < 0 ? 0 : gy >= vm->height ? vm->height - 1 : gy;
    if (kind == M_DOWN || kind == M_DOUBLE) vm->mouse_buttons |= 1;
    else if (kind == M_UP) vm->mouse_buttons &= ~1;
    else if (kind == M_RIGHT) { vm->mouse_buttons |= 2; vm_right_ticks = 3; }  /* the desktop reports only the press */
}

static void term_mouse(struct window *w, int x, int y, int kind, int value) {
    if (vm) { vm_mouse(w, x, y, kind); return; }
    struct rect bar = {w->content.w - BAR_W, 0, BAR_W, w->content.h};
    scroll.total = history_count + ROWS;
    scroll.visible = ROWS;
    scroll.top = history_count - view_up;
    if (vscroll_mouse(&scroll, bar, x, y, kind, value)) { set_view(w, history_count - scroll.top); return; }
    if (kind == M_WHEEL) { set_view(w, view_up + value * 3); return; }
    if (kind == M_RIGHT) window_popup(w, x, y, "Copy Screen\nPaste\n", 0);
}

static void term_popup(struct window *w, int tag, int index) {
    (void)tag;
    if (index == 0) {
        static char text[ROWS * (COLS + 2)];
        int n = 0;
        for (int r = 0; r < ROWS; ++r) {
            int end = COLS;
            while (end && screen[r][end - 1].ch == ' ') --end;
            for (int k = 0; k < end; ++k) text[n++] = (char)screen[r][k].ch;
            text[n++] = '\r';
            text[n++] = '\n';
        }
        gui_clipboard_set(text, n);
    } else if (index == 1) {
        static char text[4096];
        int n = gui_clipboard_get(text, sizeof(text));
        for (int i = 0; i < n; ++i) if (text[i] != '\r') send_key((unsigned char)text[i]);
        (void)w;
    }
}

static void term_closed(struct window *w) {
    (void)w;
    if (console >= 0) dosxc_sys_task(DOSXC_TASK_CONSOLE_CLOSE, 0, console, 0);
    console = -1;
}

static void term_tick(struct window *w) {
    static int count;
    if (vm && vm->magic == VDM_MAGIC) {
        if (vm_right_ticks && !--vm_right_ticks) vm->mouse_buttons &= ~2;
        if (dosxc_sys_task(DOSXC_TASK_ALIVE, 0, vm->pid, 0) <= 0) vm_announce("8086;0");   /* it was ended */
        else vm_size_window();
    } else if (vm) vm_announce("8086;0");
    if (++count >= 5) { count = 0; cursor_on = !cursor_on; if (!view_up) window_paint(w); }
    if (finished || dosxc_sys_task(DOSXC_TASK_ALIVE, 0, shell_pid, 0) > 0) return;
    finished = 1;
    if (!one_shot) { window_close(w); return; }
    const char *note = "\r\n[Finished. Press any key to close this window.]";
    while (*note) feed((unsigned char)*note++);
    window_paint(w);
}

static int term_poll(void) {
    static unsigned char buffer[8192];
    if (console < 0) return 0;
    int any = 0, n;
    while ((n = (int)dosxc_sys_task(DOSXC_TASK_CONSOLE_READ, buffer, console, sizeof(buffer))) > 0) {
        for (int i = 0; i < n; ++i) feed(buffer[i]);
        any = 1;
        if (n < (int)sizeof(buffer)) break;
    }
    if (any) window_paint(main_w);
    if (vm && vm->frame != vm_seen_frame) {
        vm_seen_frame = vm->frame;
        vm_size_window();
        window_paint(main_w);
        return 1;
    }
    return any;
}

static const struct app_ops term_ops = {term_paint, term_mouse, term_key, term_closed, term_popup, term_tick};

int main(int argc, char **argv) {
    if (gui_connect()) return 1;
    cell_w = font_mono.widths['M'] + font_mono.spacing;
    cell_h = font_mono.height + 3;
    for (int r = 0; r < ROWS; ++r) clear_cells(screen[r], COLS);
    char title[80] = "DOS Prompt";
    one_shot = argc > 1;
    if (one_shot) {
        const char *name = strrchr(argv[1], '\\');
        snprintf(title, sizeof(title), "%s", name ? name + 1 : argv[1]);
    }
    main_w = window_open(COLS * cell_w + 4 + BAR_W, ROWS * cell_h + 4, title, ICON_PROMPT, &term_ops, 0);
    if (!main_w) return 1;
    console = (int)dosxc_sys_task(DOSXC_TASK_CONSOLE_OPEN, 0, 0, 0);
    if (console < 0) return 1;
    const char *comspec = getenv("COMSPEC");
    static char line[320];
    char *shell_argv[4] = {"COMMAND.EXE", 0, 0, 0};
    if (one_shot) {
        snprintf(line, sizeof(line), "\"%s\"", argv[1]);
        shell_argv[1] = "/C";
        shell_argv[2] = line;
    }
    shell_pid = (int)dosxc_sys_task(DOSXC_TASK_START, (void *)(comspec ? comspec : "\\SYSTEM\\COMMAND.EXE"), (long)shell_argv, console + 1);
    if (shell_pid < 0) { message_box("DOS Prompt", "The command shell could not be started.", ""); return 1; }
    gui_set_poll(term_poll);
    gui_run();
    return 0;
}
