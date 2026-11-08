/*
 * DOS-XC /kernel/console.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once

void console_clear(void);
void console_putc(char c);
void console_write(const char *text);
void console_set_color(unsigned char attribute);
void console_set_text_color(unsigned char attribute);
unsigned char console_get_color(void);
void console_set_controls(void (*clear)(void *), void (*set_color)(void *, unsigned char));
void console_set_output(void *context, void (*write_char)(void *context, char character));

void console_set_cursor(unsigned row, unsigned column);
void console_set_cursor_output(void (*set_cursor)(void *, unsigned, unsigned));

void console_resume(void);
