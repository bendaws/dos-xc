/*
 * DOS-XC /os/kernel/console.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "console.h"

#define WIDTH 80
#define HEIGHT 25
static volatile unsigned short *const vga = (unsigned short *)0xb8000;
static unsigned int cursor;
static unsigned char color = 7;
static void *output_context;
static void (*output_char)(void *, char);
static void (*output_clear)(void *);
static void (*output_color)(void *, unsigned char);
static void (*output_cursor)(void *, unsigned, unsigned);

static void outb(unsigned short port, unsigned char value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void update_cursor(void) {
    if (output_char) return;
    outb(0x3d4, 14);
    outb(0x3d5, cursor >> 8);
    outb(0x3d4, 15);
    outb(0x3d5, cursor);
}

void console_set_output(void *context, void (*write_char)(void *, char)) {
    output_context = context;
    output_char = write_char;
}

void console_set_controls(void (*clear)(void *), void (*set_color)(void *, unsigned char)) {
    output_clear = clear;
    output_color = set_color;
}

void console_set_text_color(unsigned char attribute) {
    color=attribute;
    if(output_color)output_color(output_context,color);
}

void console_set_color(unsigned char attribute) {
    console_set_text_color(attribute);
    if (!output_char) {
        for (unsigned int i = 0; i < WIDTH * HEIGHT; ++i)
            vga[i] = (color << 8) | (vga[i] & 255);
    }
}
unsigned char console_get_color(void) { return color; }

void console_clear(void) {
    cursor = 0;
    if (output_char) {
        if (output_clear) output_clear(output_context);
        return;
    }
    for (unsigned int i = 0; i < WIDTH * HEIGHT; ++i) vga[i] = (color << 8) | ' ';
    update_cursor();
}

void console_putc(char c) {
    if (!output_char) outb(0xe9, (unsigned char)c);
    switch (c) {
    case '\r': cursor -= cursor % WIDTH; break;
    case '\n': cursor += WIDTH - cursor % WIDTH; break;
    case '\b': if (cursor) --cursor; break;
    case '\t': {
        unsigned int spaces = 8 - (cursor % WIDTH) % 8;
        while (spaces--) console_putc(' ');
        return;
    }
    case '\f': console_clear(); return;
    case '\a': return;
    default:
        if ((unsigned char)c < 32) return;
        if (!output_char) vga[cursor] = (color << 8) | (unsigned char)c;
        ++cursor;
    }
    if (output_char) output_char(output_context, c);
    while (cursor >= WIDTH * HEIGHT) {
        if (!output_char) {
            for (unsigned int i = 0; i < WIDTH * (HEIGHT - 1); ++i) vga[i] = vga[i + WIDTH];
            for (unsigned int i = WIDTH * (HEIGHT - 1); i < WIDTH * HEIGHT; ++i)
                vga[i] = (color << 8) | ' ';
        }
        cursor -= WIDTH;
    }
    update_cursor();
}

void console_write(const char *text) {
    while (*text) console_putc(*text++);
}

void console_set_cursor_output(void (*set_cursor)(void *,unsigned,unsigned)) { output_cursor=set_cursor; }
void console_set_cursor(unsigned row,unsigned column) {
    if(row>=HEIGHT)row=HEIGHT-1;if(column>=WIDTH)column=WIDTH-1;
    cursor=row*WIDTH+column;
    if(output_cursor)output_cursor(output_context,row,column);else update_cursor();
}

void console_get_cursor(unsigned *row, unsigned *column) {
    *row = cursor / WIDTH;
    *column = cursor % WIDTH;
}

/* Write one cell without moving the cursor or scrolling. */
void console_put_at(unsigned row, unsigned column, char c) {
    if (row >= HEIGHT || column >= WIDTH) return;
    if (!output_char) {
        outb(0xe9, (unsigned char)c);
        vga[row * WIDTH + column] = (color << 8) | (unsigned char)c;
        return;
    }
    if (row == HEIGHT - 1 && column == WIDTH - 1) return;
    unsigned saved = cursor;
    if (output_cursor) output_cursor(output_context, row, column);
    output_char(output_context, c);
    cursor = saved;
    if (output_cursor) output_cursor(output_context, saved / WIDTH, saved % WIDTH);
}

/* Blank COUNT cells from (row, column) in the current color. */
void console_erase(unsigned row, unsigned column, unsigned count) {
    if (row >= HEIGHT || column >= WIDTH) return;
    unsigned at = row * WIDTH + column;
    if (count > WIDTH * HEIGHT - at) count = WIDTH * HEIGHT - at;
    if (!output_char) {
        for (unsigned i = 0; i < count; ++i) vga[at + i] = (color << 8) | ' ';
        return;
    }
    unsigned saved = cursor;
    for (unsigned i = 0; i < count; ++i) {
        unsigned cell = at + i;
        if (cell == WIDTH * HEIGHT - 1) break;
        if (i == 0 || cell % WIDTH == 0) {
            if (output_cursor) output_cursor(output_context, cell / WIDTH, cell % WIDTH);
        }
        output_char(output_context, ' ');
    }
    cursor = saved;
    if (output_cursor) output_cursor(output_context, saved / WIDTH, saved % WIDTH);
}

/* Scroll rows top..bottom by one line; the vacated row is blanked. */
int console_scroll(unsigned top, unsigned bottom, int up) {
    if (output_char || top > bottom || bottom >= HEIGHT) return -1;
    if (up) {
        for (unsigned i = top * WIDTH; i < bottom * WIDTH; ++i) vga[i] = vga[i + WIDTH];
        for (unsigned i = bottom * WIDTH; i < (bottom + 1) * WIDTH; ++i) vga[i] = (color << 8) | ' ';
    } else {
        for (unsigned i = (bottom + 1) * WIDTH; i-- > (top + 1) * WIDTH;) vga[i] = vga[i - WIDTH];
        for (unsigned i = top * WIDTH; i < (top + 1) * WIDTH; ++i) vga[i] = (color << 8) | ' ';
    }
    return 0;
}

void console_resume(void) {
    unsigned short position=*(volatile unsigned short *)0x450;
    unsigned row=position>>8,column=position&255;
    console_set_cursor(row,column);
}
