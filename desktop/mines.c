/*
 * DOS-XC /desktop/mines.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "wm.h"
#include <time.h>

#define CELL 20
#define TOP (MENUBAR_H + 46)
#define MARGIN 10
#define MAX_W 30
#define MAX_H 16

static const struct menubar bar = {{"Game"}, {"New\nBeginner\nIntermediate\nExpert\n-\nExit\n"}};
static int width = 9, height = 9, mines = 10;
static signed char count[MAX_W][MAX_H];  /* -1 is a mine */
static unsigned char state[MAX_W][MAX_H];  /* 0 covered, 1 open, 2 flag */
static int started, over, won, flags_placed, opened, elapsed;
static long long start_time;

static int window_w(void) { return MARGIN * 2 + width * CELL; }
static int window_h(void) { return TOP + height * CELL + MARGIN; }
static struct rect new_button(int w) { return (struct rect){(w - 64) / 2, MENUBAR_H + 8, 64, 28}; }

static void reset(void) {
    memset(count, 0, sizeof(count));
    memset(state, 0, sizeof(state));
    started = over = won = flags_placed = opened = elapsed = 0;
}

static void lay_mines(int safe_x, int safe_y) {
    srand((unsigned)dosxc_sys_nanoseconds());
    int placed = 0;
    while (placed < mines) {
        int x = rand() % width, y = rand() % height;
        if (count[x][y] < 0 || (abs(x - safe_x) <= 1 && abs(y - safe_y) <= 1)) continue;
        count[x][y] = -1;
        ++placed;
    }
    for (int x = 0; x < width; ++x)
        for (int y = 0; y < height; ++y) {
            if (count[x][y] < 0) continue;
            int n = 0;
            for (int i = -1; i <= 1; ++i)
                for (int j = -1; j <= 1; ++j)
                    if (x + i >= 0 && y + j >= 0 && x + i < width && y + j < height && count[x + i][y + j] < 0) ++n;
            count[x][y] = (signed char)n;
        }
}

static void reveal(int x, int y) {
    if (x < 0 || y < 0 || x >= width || y >= height || state[x][y]) return;
    state[x][y] = 1;
    ++opened;
    if (count[x][y] < 0) { over = 1; return; }
    if (!count[x][y])
        for (int i = -1; i <= 1; ++i)
            for (int j = -1; j <= 1; ++j) reveal(x + i, y + j);
}

static void check_won(void) {
    if (!over && opened == width * height - mines) { over = won = 1; }
}

static void disk(const struct canvas *c, int cx, int cy, int r, u32 color) {
    for (int dy = -r; dy <= r; ++dy) {
        int span = 0;
        while (span * span + dy * dy <= r * r) ++span;
        fill(c, cx - span + 1, cy + dy, 2 * span - 1, 1, color);
    }
}

static void paint_cell(const struct canvas *c, int x, int y) {
    static const u32 colors[9] = {0, 0x0000d0, 0x008000, 0xd00000, 0x000080, 0x800000, 0x008080, 0x000000, 0x606060};
    int px = MARGIN + x * CELL, py = TOP + y * CELL;
    if (state[x][y] != 1) {
        fill(c, px, py, CELL, CELL, C_WINDOW);
        bevel(c, px, py, CELL, CELL, 0);
        if (state[x][y] == 2) {
            fill(c, px + 9, py + 4, 2, 12, C_TEXT);
            fill(c, px + 5, py + 4, 5, 5, 0xd02020);
            fill(c, px + 6, py + 15, 8, 2, C_TEXT);
            if (over && count[x][y] >= 0) fill(c, px + 3, py + 9, 14, 2, 0xd02020);
        } else if (over && !won && count[x][y] < 0) disk(c, px + 10, py + 10, 5, C_TEXT);
        return;
    }
    fill(c, px, py, CELL, CELL, count[x][y] < 0 ? 0xd02020 : 0xc8c4bc);
    frame_box(c, px, py, CELL + 1, CELL + 1, C_SHADOW1);
    if (count[x][y] < 0) { disk(c, px + 10, py + 10, 5, C_TEXT); return; }
    if (count[x][y]) {
        char s[2] = {(char)('0' + count[x][y]), 0};
        text_big(c, px + 7, py + 5, s, colors[(int)count[x][y]], 1);
    }
}

static void counter(const struct canvas *c, int x, int value) {
    char s[12];
    snprintf(s, sizeof(s), "%03d", value);
    bevel(c, x, MENUBAR_H + 8, 52, 28, 1);
    fill(c, x + 2, MENUBAR_H + 10, 48, 24, C_TEXT);
    text_big(c, x + 7, MENUBAR_H + 11, s, 0xe03030, 2);
}

static void mines_paint(struct window *w, const struct canvas *c) {
    menubar_paint(c, &bar, w->content.w);
    counter(c, MARGIN, mines - flags_placed);
    counter(c, w->content.w - MARGIN - 52, elapsed > 999 ? 999 : elapsed);
    button(c, new_button(w->content.w), won ? "You won" : over ? "Again" : "New", 0, won);
    bevel(c, MARGIN - 2, TOP - 2, width * CELL + 4, height * CELL + 4, 1);
    for (int x = 0; x < width; ++x)
        for (int y = 0; y < height; ++y) paint_cell(c, x, y);
}

static void set_level(struct window *w, int nw, int nh, int nm) {
    width = nw; height = nh; mines = nm;
    reset();
    window_request_size(w, window_w(), window_h());
    window_paint(w);
}

static void mines_mouse(struct window *w, int x, int y, int kind, int value) {
    (void)value;
    if (menubar_mouse(w, &bar, x, y, kind)) return;
    if (kind == M_DOWN || kind == M_DOUBLE)
        if (rect_has(new_button(w->content.w), x, y)) { reset(); window_paint(w); return; }
    if (over || y < TOP) return;
    int cx = (x - MARGIN) / CELL, cy = (y - TOP) / CELL;
    if (x < MARGIN || cx >= width || cy >= height) return;
    if (kind == M_RIGHT) {
        if (state[cx][cy] == 1) return;
        flags_placed += state[cx][cy] == 2 ? -1 : 1;
        state[cx][cy] = state[cx][cy] == 2 ? 0 : 2;
        window_paint(w);
        return;
    }
    if (kind == M_DOUBLE && state[cx][cy] == 1 && count[cx][cy] > 0) {
        int around = 0;
        for (int i = -1; i <= 1; ++i)
            for (int j = -1; j <= 1; ++j)
                if (cx + i >= 0 && cy + j >= 0 && cx + i < width && cy + j < height && state[cx + i][cy + j] == 2) ++around;
        if (around == count[cx][cy])
            for (int i = -1; i <= 1; ++i)
                for (int j = -1; j <= 1; ++j)
                    if (cx + i >= 0 && cy + j >= 0 && cx + i < width && cy + j < height && !state[cx + i][cy + j]) reveal(cx + i, cy + j);
        check_won();
        window_paint(w);
        return;
    }
    if (kind != M_DOWN || state[cx][cy]) return;
    if (!started) {
        lay_mines(cx, cy);
        started = 1;
        start_time = (long long)dosxc_sys_nanoseconds();
    }
    reveal(cx, cy);
    check_won();
    window_paint(w);
}

static void mines_popup(struct window *w, int tag, int index) {
    (void)tag;
    switch (index) {
    case 0: reset(); window_paint(w); break;
    case 1: set_level(w, 9, 9, 10); break;
    case 2: set_level(w, 16, 16, 40); break;
    case 3: set_level(w, 30, 16, 99); break;
    case 5: window_close(w); break;
    }
}

static void mines_tick(struct window *w) {
    if (!started || over) return;
    int now = (int)(((long long)dosxc_sys_nanoseconds() - start_time) / 1000000000);
    if (now != elapsed) { elapsed = now; window_paint(w); }
}

static const struct app_ops mines_ops = {mines_paint, mines_mouse, 0, 0, mines_popup, mines_tick};

int main(void) {
    if (gui_connect()) return 1;
    window_open(window_w(), window_h(), "Minesweeper", ICON_MINES, &mines_ops, 0);
    gui_run();
    return 0;
}
