/*
 * DOS-XC /desktop/vdm/screen.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once
#include <stdint.h>

#define VDM_MAGIC 0x564d4458u
struct vdm_screen {
    uint32_t magic;
    int pid;
    volatile uint32_t frame;        /* adds 1 when the picture changes */
    volatile int graphics;          /* 1: pixels; 0: 80 x 25 style text cells */
    volatile int width, height;     /* pixels, or columns and rows */
    const uint32_t *volatile pixels;    /* 0x00RRGGBB, width x height */
    const uint8_t *volatile text;       /* character, attribute pairs */
    volatile int cursor_row, cursor_col, cursor_on;
    volatile int mouse_x, mouse_y, mouse_buttons;   /* written by the terminal: in pixels or cells */
};
