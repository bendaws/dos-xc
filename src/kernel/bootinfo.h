/*
 * BenDOS /kernel/bootinfo.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once

#define BENDOS_NAME "Ben's DOS"
#define BENDOS_VERSION "0.1"
#define BENDOS_DOS_COMPAT_MAJOR 5
#define BENDOS_DOS_COMPAT_MINOR 0
#define BENDOS_LABEL BENDOS_NAME " v" BENDOS_VERSION

#include "../drivers/fat32.h"

/* The ELF kernel uses the System V ABI; EFI itself uses the Microsoft ABI. */
typedef int (__attribute__((sysv_abi)) *boot_read_key_fn)(void *context);
typedef void (__attribute__((sysv_abi)) *boot_write_char_fn)(void *context, char character);
typedef void (__attribute__((sysv_abi)) *boot_clear_fn)(void *context);
typedef void (__attribute__((sysv_abi)) *boot_color_fn)(void *context, unsigned char attribute);

struct boot_info {
    boot_read_key_fn read_key;
    void *keyboard_context;
    boot_write_char_fn write_char;
    void *console_context;
    struct fat32_volume *volume;
    boot_clear_fn clear_screen;
    boot_color_fn set_color;
    char drive_letter;
    void (__attribute__((sysv_abi)) *set_cursor)(void *, unsigned, unsigned);
};
