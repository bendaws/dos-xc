/*
 * BenDOS /drivers/keyboard.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once

void keyboard_init(void);
int keyboard_read(void);
void keyboard_set_boot_info(void *context, int (*read_key)(void *context));
