/*
 * DOS-XC /os/kernel/bootinfo.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once

#define DOSXC_NAME "DOS-XC"
#define DOSXC_VERSION "0.2"
#define DOSXC_DOS_COMPAT_MAJOR 5
#define DOSXC_DOS_COMPAT_MINOR 0
#define DOSXC_LABEL DOSXC_NAME " v" DOSXC_VERSION

#include "../drivers/fat32.h"
#include "../programs/api.h"

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
    uint64_t memory_base, memory_size;   /* RAM reserved for programs */
    /* UEFI only: the firmware's physical disks and system reset. */
    disk_count_fn disk_count;
    disk_info_fn disk_info;
    disk_io_fn disk_io;
    void (__attribute__((sysv_abi)) *reboot)(void);
    unsigned (__attribute__((sysv_abi)) *disk_rescan)(void);
    int (__attribute__((sysv_abi)) *pointer)(int *, int *, int *, unsigned *);
    void (__attribute__((sysv_abi)) *poweroff)(void);
    uint64_t acpi_rsdp;
    int (__attribute__((sysv_abi)) *display)(int, unsigned, unsigned *, unsigned *, unsigned *, uint64_t *);
};
