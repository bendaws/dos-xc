/*
 * BenDOS /kernel/main.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "../drivers/keyboard.h"
#include "../drivers/pcspeaker.h"
#include "../drivers/fat32.h"
#include "../programs/shell.h"
#include "console.h"
#include "bootinfo.h"
#include "../drivers/storage.h"

extern char __core_bss_start[], __core_bss_end[], __shell_bss_start[], __shell_bss_end[];

void kmain(struct boot_info *boot_info) {
    for (char *p = __core_bss_start; p < __core_bss_end; ++p) *p = 0;
    for (char *p = __shell_bss_start; p < __shell_bss_end; ++p) *p = 0;
    keyboard_init();
    if (boot_info && boot_info->read_key)
        keyboard_set_boot_info(boot_info->keyboard_context, boot_info->read_key);
    if (boot_info && boot_info->write_char)
        console_set_output(boot_info->console_context, boot_info->write_char);
    if (boot_info)
        console_set_controls(boot_info->clear_screen, boot_info->set_color);
    if (boot_info) console_set_cursor_output(boot_info->set_cursor);
    pcspeaker_beep(440, 100);
    char drive = boot_info ? boot_info->drive_letter :
        (*(volatile unsigned int *)0x7000 == 0x53464442 && !*(volatile unsigned char *)0x7008 ? 'C' : 'X');
    shell_run(boot_info ? boot_info->volume : storage_system_volume(), !boot_info, drive);
    
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
