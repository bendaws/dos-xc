/*
 * DOS-XC /desktop/calc.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * CALC.EXE: a basic four-function calculator.
 */
#include "wm.h"
#include <math.h>

#define BW 46
#define BH 30
#define GAP 6
#define LEFT 10
#define TOP 54

static const char *const labels[5][5] = {
    {"C", "CE", "Back", "", ""},
    {"7", "8", "9", "/", "sqrt"},
    {"4", "5", "6", "*", "%"},
    {"1", "2", "3", "-", "1/x"},
    {"0", "+/-", ".", "+", "="},
};
static char display[40] = "0";
static double accumulator, entry_value;
static char op;
static int fresh = 1, error;

static void format_value(double v) {
    if (isnan(v) || isinf(v)) { snprintf(display, sizeof(display), "Error"); error = 1; return; }
    snprintf(display, sizeof(display), "%.12g", v);
}

static double current(void) { return strtod(display, 0); }

static void apply(void) {
    double v = current();
    switch (op) {
    case '+': accumulator += v; break;
    case '-': accumulator -= v; break;
    case '*': accumulator *= v; break;
    case '/':
        if (v == 0) { snprintf(display, sizeof(display), "Cannot divide by zero"); error = 1; op = 0; fresh = 1; return; }
        accumulator /= v;
        break;
    default: accumulator = v; break;
    }
    format_value(accumulator);
}

static void press(const char *b) {
    if (error && strcmp(b, "C")) return;
    if ((b[0] >= '0' && b[0] <= '9' && !b[1]) || !strcmp(b, ".")) {
        if (fresh) { strcpy(display, "0"); fresh = 0; }
        size_t n = strlen(display);
        if (b[0] == '.') { if (!strchr(display, '.') && n < 20) strcat(display, "."); return; }
        if (!strcmp(display, "0")) display[0] = 0, n = 0;
        else if (!strcmp(display, "-0")) display[1] = 0, n = 1;
        if (n < 20) { display[n] = b[0]; display[n + 1] = 0; }
        return;
    }
    if (!strcmp(b, "C")) { strcpy(display, "0"); accumulator = entry_value = 0; op = 0; fresh = 1; error = 0; return; }
    if (!strcmp(b, "CE")) { strcpy(display, "0"); fresh = 1; return; }
    if (!strcmp(b, "Back")) {
        if (fresh) return;
        size_t n = strlen(display);
        if (n > 1 && !(n == 2 && display[0] == '-')) display[n - 1] = 0;
        else strcpy(display, "0");
        return;
    }
    if (!strcmp(b, "+/-")) {
        if (!strcmp(display, "0")) return;
        if (display[0] == '-') memmove(display, display + 1, strlen(display));
        else { memmove(display + 1, display, strlen(display) + 1); display[0] = '-'; }
        return;
    }
    if (!strcmp(b, "sqrt")) {
        double v = current();
        if (v < 0) { snprintf(display, sizeof(display), "Invalid input"); error = 1; return; }
        format_value(sqrt(v));
        fresh = 1;
        return;
    }
    if (!strcmp(b, "1/x")) {
        double v = current();
        if (v == 0) { snprintf(display, sizeof(display), "Cannot divide by zero"); error = 1; return; }
        format_value(1 / v);
        fresh = 1;
        return;
    }
    if (!strcmp(b, "%")) { format_value(accumulator * current() / 100); fresh = 0; return; }
    if (!strcmp(b, "=")) {
        if (op) apply();
        op = 0;
        fresh = 1;
        return;
    }
    if (!fresh || !op) apply();
    op = b[0];
    entry_value = current();
    fresh = 1;
}

static struct rect cell(int row, int col) { return (struct rect){LEFT + col * (BW + GAP), TOP + row * (BH + GAP), BW, BH}; }

static void calc_paint(struct window *w, const struct canvas *c) {
    bevel(c, LEFT, 10, 5 * BW + 4 * GAP, 32, 1);
    fill(c, LEFT + 2, 12, 5 * BW + 4 * GAP - 4, 28, C_BASE);
    text(c, LEFT + 5 * BW + 4 * GAP - 8 - text_width(&font_bold, display), 21, &font_bold, display, C_TEXT);
    for (int r = 0; r < 5; ++r)
        for (int k = 0; k < 5; ++k)
            if (labels[r][k][0]) button(c, cell(r, k), labels[r][k], 0, 0);
}

static void calc_mouse(struct window *w, int x, int y, int kind, int value) {
    (void)value;
    if (kind != M_DOWN && kind != M_DOUBLE) return;
    for (int r = 0; r < 5; ++r)
        for (int k = 0; k < 5; ++k)
            if (labels[r][k][0] && rect_has(cell(r, k), x, y)) { press(labels[r][k]); window_paint(w); return; }
}

static void calc_key(struct window *w, int key) {
    char one[2] = {(char)key, 0};
    if ((key >= '0' && key <= '9') || key == '.' || key == '+' || key == '-' || key == '*' || key == '/' || key == '%' || key == '=') press(one);
    else if (key == '\n') press("=");
    else if (key == '\b') press("Back");
    else if (key == 27) press("C");
    else return;
    window_paint(w);
}

static const struct app_ops calc_ops = {calc_paint, calc_mouse, calc_key};

int main(void) {
    if (gui_connect()) return 1;
    window_open(2 * LEFT + 5 * BW + 4 * GAP, TOP + 5 * BH + 4 * GAP + 10, "Calculator", ICON_CALC, &calc_ops, 0);
    gui_run();
    return 0;
}
