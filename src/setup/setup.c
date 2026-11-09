/*
 * DOS-XC /setup/setup.c - SETUP.EXE, DOS-XC Setup
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * Runs from the live system on the installation media.  Screens: welcome,
 * choose the disk, choose packages (from \Packages on the media), confirm,
 * copy.  The disk gets one FAT32 partition holding \SYSTEM, \EFI, \Home,
 * \TEMP and \Programs with the chosen packages installed.
 */

#include <ctype.h>
#include <dosxc/syscall.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "install.h"

#define KEY_UP 0x101
#define KEY_DOWN 0x102
#define KEY_F1 0x10b
#define KEY_F3 0x10d
#define BLUE 0x17
#define WHITE_ON_BLUE 0x1f
#define BAR 0x70

static int progress_row;

/* ---- screen ------------------------------------------------------------- */

static void console(int op, long a, long b) {
    fflush(stdout);
    dosxc_sys_console(op, a, b);
}

static int key(void) { return dosxc_sys_console(DOSXC_CONSOLE_GETKEY, 1, 0); }

static void at(int row, int column) { console(DOSXC_CONSOLE_CURSOR, row, column); }

static void footer(const char *keys) {
    at(24, 0);
    console(DOSXC_CONSOLE_COLOR, BAR, 0);
    printf("  %-77s", keys);
    console(DOSXC_CONSOLE_COLOR, WHITE_ON_BLUE, 0);
}

/* Clear to blue, draw the caption, then write BODY from row 4 (lines are
   indented by 4 columns).  Returns the next free row. */
static int screen(const char *body, const char *keys) {
    console(DOSXC_CONSOLE_COLOR, BLUE | 0x100, 0);
    console(DOSXC_CONSOLE_CLEAR, 0, 0);
    at(1, 2);
    console(DOSXC_CONSOLE_COLOR, WHITE_ON_BLUE, 0);
    printf("DOS-XC %s Setup", setup_system.version + (strncmp(setup_system.version, "DOS-XC v", 8) ? 0 : 8));
    at(2, 2);
    printf("==================================");
    int row = 4;
    at(row, 4);
    for (const char *p = body; *p; ++p) {
        if (*p == '\n') { at(++row, 4); continue; }
        putchar(*p);
    }
    footer(keys);
    at(row + 2, 4);
    return row + 2;
}

static void help(void) {
    screen("DOS-XC Setup Help\n\n"
           "Setup installs DOS-XC on an internal hard disk.  The disk is erased and\n"
           "gets a single FAT32 partition; DOS-XC then starts from it with BIOS or\n"
           "UEFI firmware.\n\n"
           "Keys:  ENTER continues.   ESC goes back.   F3 leaves Setup.\n"
           "       Arrow keys move, SPACE selects packages.\n\n"
           "The disk must use 512-byte sectors and hold at least 64 MB.  The disk\n"
           "Setup is running from is never offered.\n\n"
           "Press any key to return.", "Press any key to return");
    key();
}

/* ---- installation ---------------------------------------------------------- */

static void show_status(const char *text) {
    at(progress_row, 4);
    printf("%-72s", text);
    fflush(stdout);
}

static void show_progress(const char *name, unsigned percent, unsigned long done, unsigned long total) {
    at(progress_row, 4);
    printf("%-72.72s", name);
    at(progress_row + 2, 4);
    printf("%3u%% complete   (%lu of %lu files)   ", percent, done, total);
    unsigned filled = percent * 60 / 100;
    at(progress_row + 4, 4);
    console(DOSXC_CONSOLE_COLOR, 0x1e, 0);
    for (unsigned i = 0; i < 60; ++i) putchar(i < filled ? '\xdb' : '\xb0');
    console(DOSXC_CONSOLE_COLOR, WHITE_ON_BLUE, 0);
    fflush(stdout);
}

/* ---- screens -------------------------------------------------------------- */

static int leave(void) {
    console(DOSXC_CONSOLE_COLOR, 0x07 | 0x100, 0);
    console(DOSXC_CONSOLE_CLEAR, 0, 0);
    return 1;
}

int main(void) {
    setup_init();
    if (!setup_system.live) {
        puts("Setup runs from the DOS-XC installation media.  Start the computer from the\n"
             "installation USB drive or CD, then type SETUP.");
        return 1;
    }
    setvbuf(stdout, 0, _IOFBF, 4096);
    int selected_disk = 0, cursor = 0;
welcome:
    for (;;) {
        screen("Welcome to DOS-XC!\n\n"
               "Setup will prepare to install DOS-XC on your system.\n\n"
               "To continue, press ENTER. To exit to the live shell, press F3. For help, press F1.",
               "ENTER=Continue  F1=Help  F3=Exit");
        int k = key();
        if (k == KEY_F3) return leave();
        if (k == KEY_F1) { help(); continue; }
        if (k == '\n' || k == '\r') break;
    }
    setup_find_disks();
    for (;;) {
        int row = screen("DOS-XC requires a partition on your hard drive to copy files to.\n\n"
                         "Installing DOS-XC will erase all data on your hard drive, so please back up\n"
                         "your documents before installing.",
                         "ENTER=Select  ESC=Back  F1=Help  F3=Exit");
        if (!setup_disk_count) {
            at(row, 4);
            printf("No suitable hard disk was found.  Setup needs an internal disk of at");
            at(row + 1, 4);
            printf("least 64 MB with 512-byte sectors.");
        }
        for (int i = 0; i < setup_disk_count; ++i) {
            char size[32];
            setup_size_text(size, sizeof(size), setup_disks[i].sectors);
            at(row + 1 + i, 6);
            if (i == selected_disk) console(DOSXC_CONSOLE_COLOR, BAR, 0);
            printf(" Disk %d   %-12s ", i + 1, size);
            console(DOSXC_CONSOLE_COLOR, WHITE_ON_BLUE, 0);
        }
        int k = key();
        if (k == KEY_F3) return leave();
        if (k == KEY_F1) { help(); continue; }
        if (k == 27) goto welcome;
        if (k == KEY_UP && selected_disk) --selected_disk;
        if (k == KEY_DOWN && selected_disk + 1 < setup_disk_count) ++selected_disk;
        if ((k == '\n' || k == '\r') && setup_disk_count) break;
    }
    setup_find_packages();
packages_screen:
    for (;;) {
        int row = screen("DOS-XC Setup can configure and install the following programs for you\n"
                         "automatically.\n\n"
                         "Select the programs you would like SETUP to install with the arrow keys to\n"
                         "move and SPACE to select.",
                         "ENTER=Continue  SPACE=Select  ESC=Back  F1=Help  F3=Exit");
        if (!setup_package_count) { at(row, 4); printf("(No packages were found on the installation media.)"); }
        for (int i = 0; i < setup_package_count; ++i) {
            const struct package_choice *p = &setup_packages[i];
            at(row + i, 6);
            if (i == cursor) console(DOSXC_CONSOLE_COLOR, BAR, 0);
            char line[80];
            snprintf(line, sizeof(line), " [%c] %-10.10s %-10.10s %-.40s ", p->selected ? 'X' : ' ', p->info.name,
                     p->info.version, p->info.description);
            printf("%-70s", line);
            console(DOSXC_CONSOLE_COLOR, WHITE_ON_BLUE, 0);
            if (p->required) printf(" (required)");
        }
        int k = key();
        if (k == KEY_F3) return leave();
        if (k == KEY_F1) { help(); continue; }
        if (k == 27) { setup_find_disks(); goto welcome; }
        if (k == KEY_UP && cursor) --cursor;
        if (k == KEY_DOWN && cursor + 1 < setup_package_count) ++cursor;
        if (k == ' ' && setup_package_count && !setup_packages[cursor].required) setup_packages[cursor].selected ^= 1;
        if (k == '\n' || k == '\r') break;
    }
    for (;;) {
        char size[32], body[600], list[300] = "";
        setup_size_text(size, sizeof(size), setup_disks[selected_disk].sectors);
        for (int i = 0; i < setup_package_count; ++i) {
            if (!setup_packages[i].selected) continue;
            if (*list) strcat(list, ", ");
            if (strlen(list) + strlen(setup_packages[i].info.name) < sizeof(list) - 8) strcat(list, setup_packages[i].info.name);
        }
        snprintf(body, sizeof(body),
                 "Setup is ready to install DOS-XC on disk %d (%s).\n\n"
                 "ALL DATA ON THIS DISK WILL BE ERASED.\n\n"
                 "Programs to install: %s\n\n"
                 "To erase the disk and install DOS-XC, press E.\n"
                 "To change your choices, press ESC.",
                 selected_disk + 1, size, *list ? list : "(none)");
        screen(body, "E=Erase and install  ESC=Back  F3=Exit");
        int k = key();
        if (k == KEY_F3) return leave();
        if (k == 27) goto packages_screen;
        if (k == 'e' || k == 'E') break;
    }
    screen("Setup is copying files to your hard disk.\n\n"
           "Do not turn off your computer.", "Copying files - please wait");
    progress_row = 8;
    if (setup_install(&setup_disks[selected_disk], show_progress, show_status)) {
        char message[600];
        snprintf(message, sizeof(message),
                 "Setup could not complete the installation, so do not start the computer\n"
                 "from this disk.\n\n%.150s\n\n"
                 "Check the disk and its connection, then run Setup again.\n\n"
                 "Press any key to return to the live shell.", setup_failure);
        screen(message, "Press any key");
        key();
        return leave(), 1;
    }
    screen("Setup has finished installing DOS-XC.\n\n"
           "Remove the installation media, then press ENTER to restart your computer.\n"
           "To return to the live shell instead, press F3.", "ENTER=Restart  F3=Live shell");
    for (;;) {
        int k = key();
        if (k == KEY_F3) return leave(), 0;
        if (k == '\n' || k == '\r') break;
    }
    dosxc_sys_reboot();
    return 0;
}
