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
#include "../pkg/dsp.h"
#include "target.h"

#define KEY_UP 0x101
#define KEY_DOWN 0x102
#define KEY_F1 0x10b
#define KEY_F3 0x10d
#define BLUE 0x17
#define WHITE_ON_BLUE 0x1f
#define BAR 0x70

static struct dosxc_sysinfo sys;
static char source_root[4];

struct disk_choice { unsigned index; unsigned long long sectors; };
static struct disk_choice disks[16];
static int disk_count;

struct package_choice { char file[300]; struct dsp_info info; int selected, required; };
static struct package_choice packages[64];
static int package_count;

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
    printf("DOS-XC %s Setup", sys.version + (strncmp(sys.version, "DOS-XC v", 8) ? 0 : 8));
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

/* ---- data ---------------------------------------------------------------- */

static void find_disks(void) {
    disk_count = 0;
    struct dosxc_disk info;
    int total = dosxc_sys_disk(DOSXC_DISK_INFO, 0, 0, &info, 0);
    for (int i = 0; i < total && disk_count < 16; ++i) {
        if (dosxc_sys_disk(DOSXC_DISK_INFO, (unsigned)i, 0, &info, 0) < 0) continue;
        if (info.flags & (DOSXC_DISK_SYSTEM | DOSXC_DISK_READONLY | DOSXC_DISK_REMOVABLE)) continue;
        if (info.sectors < 131072 || info.sectors > 0xffffffffull) continue;
        disks[disk_count].index = (unsigned)i;
        disks[disk_count++].sectors = info.sectors;
    }
}

static int required(const char *name) { return !strcasecmp(name, "coreutils"); }
static int preselected(const char *name) { return required(name) || !strcasecmp(name, "batch") || !strcasecmp(name, "ethernet"); }

static int compare_packages(const void *a, const void *b) {
    const struct package_choice *x = a, *y = b;
    if (x->required != y->required) return y->required - x->required;
    return strcasecmp(x->info.name, y->info.name);
}

static void find_packages(void) {
    char folder[32];
    snprintf(folder, sizeof(folder), "%s\\Packages", source_root);
    struct dosxc_dirent *entries = malloc(sizeof(*entries) * 128);
    int count = entries ? (int)dosxc_sys_listdir(folder, entries, 128) : 0;
    package_count = 0;
    for (int i = 0; i < count && i < 128 && package_count < 64; ++i) {
        size_t n = strlen(entries[i].name);
        if (n < 5 || strcasecmp(entries[i].name + n - 4, ".DSP")) continue;
        struct package_choice *p = &packages[package_count];
        snprintf(p->file, sizeof(p->file), "%s\\%s", folder, entries[i].name);
        char error[100];
        if (dsp_read_info(p->file, &p->info, error, sizeof(error))) continue;
        if (p->info.arch != 16 && p->info.arch != (int)sys.bits) continue;
        p->required = required(p->info.name);
        p->selected = preselected(p->info.name);
        ++package_count;
    }
    free(entries);
    qsort(packages, (size_t)package_count, sizeof(packages[0]), compare_packages);
}

static void size_text(char *out, size_t size, unsigned long long sectors) {
    unsigned long long mb = sectors / 2048;
    if (mb >= 10240) snprintf(out, size, "%llu GB", (mb + 512) / 1024);
    else snprintf(out, size, "%llu MB", mb);
}

/* ---- installation ---------------------------------------------------------- */

static unsigned long files_total, files_done;
static char failure[160];                 /* what went wrong, for the error screen */

static int fail(const char *what, const char *path) {
    snprintf(failure, sizeof(failure), "%s %s", what, path ? path : "");
    return -1;
}
static int progress_row;

static void progress(const char *name) {
    ++files_done;
    unsigned percent = files_total ? (unsigned)(files_done * 100 / files_total) : 100;
    if (percent > 100) percent = 100;
    at(progress_row, 4);
    printf("%-72.72s", name);
    at(progress_row + 2, 4);
    printf("%3u%% complete   (%lu of %lu files)   ", percent, files_done, files_total);
    unsigned filled = percent * 60 / 100;
    at(progress_row + 4, 4);
    console(DOSXC_CONSOLE_COLOR, 0x1e, 0);
    for (unsigned i = 0; i < 60; ++i) putchar(i < filled ? '\xdb' : '\xb0');
    console(DOSXC_CONSOLE_COLOR, WHITE_ON_BLUE, 0);
    fflush(stdout);
}

static unsigned char *read_file(const char *path, unsigned long *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc(n > 0 ? (size_t)n : 1);
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); data = 0; }
    fclose(f);
    *size = (unsigned long)n;
    return data;
}

static unsigned long count_tree(const char *folder) {
    struct dosxc_dirent *entries = malloc(sizeof(*entries) * 512);
    int count = entries ? (int)dosxc_sys_listdir(folder, entries, 512) : 0;
    unsigned long total = 0;
    for (int i = 0; i < count && i < 512; ++i) {
        if (entries[i].attributes & DOSXC_ATTR_DIRECTORY) {
            char child[300];
            snprintf(child, sizeof(child), "%s\\%s", folder, entries[i].name);
            total += count_tree(child);
        } else ++total;
    }
    free(entries);
    return total;
}

/* Copy SOURCE (on the live drive, "X:\SYSTEM") to the same path on the target. */
static int copy_tree(const char *source) {
    if (target_mkdir(source + 2)) return fail("Cannot create", source + 2);
    struct dosxc_dirent *entries = malloc(sizeof(*entries) * 512);
    int count = entries ? (int)dosxc_sys_listdir(source, entries, 512) : -1;
    if (count < 0 || count > 512) { free(entries); return -1; }
    for (int i = 0; i < count; ++i) {
        char child[300];
        snprintf(child, sizeof(child), "%s\\%s", source, entries[i].name);
        if (entries[i].attributes & DOSXC_ATTR_DIRECTORY) {
            if (copy_tree(child)) { free(entries); return -1; }
            continue;
        }
        unsigned long size;
        unsigned char *data = read_file(child, &size);
        if (!data) { free(entries); return fail("Cannot read", child); }
        progress(child + 2);
        int r = target_write(child + 2, data, size);
        free(data);
        if (r) { free(entries); return fail("Cannot write", child + 2); }
    }
    free(entries);
    return 0;
}

/* Is NAME one of the package's Driver= files (they go to \\SYSTEM\\DRIVERS)? */
static int is_driver(const struct dsp_info *info, const char *name) {
    size_t n = strlen(name);
    for (const char *p = info->driver; *p;) {
        size_t length = strcspn(p, ";");
        if (length == n && !strncasecmp(p, name, n)) return 1;
        p += length;
        if (*p) ++p;
    }
    return 0;
}

static int install_package(const struct package_choice *choice) {
    struct dsp package;
    char error[200], folder[128];
    if (dsp_open(&package, choice->file, error, sizeof(error))) return fail(error, choice->file);
    snprintf(folder, sizeof(folder), "\\Programs\\%s", package.info.name);
    if (target_mkdir(folder)) { dsp_close(&package); return -1; }
    for (unsigned i = 0; i < package.count; ++i) {
        char path[600];
        snprintf(path, sizeof(path), "%s\\%s", folder, package.entries[i].name);
        if (is_driver(&package.info, package.entries[i].name)) {
            if (target_mkdir("\\SYSTEM\\DRIVERS")) { dsp_close(&package); return fail("Cannot create", "\\SYSTEM\\DRIVERS"); }
            snprintf(path, sizeof(path), "\\SYSTEM\\DRIVERS\\%s", package.entries[i].name);
        }
        for (char *p = path; *p; ++p) if (*p == '/') *p = '\\';
        size_t n = strlen(path);
        int directory = n && path[n - 1] == '\\';
        if (directory) path[n - 1] = 0;
        for (char *p = is_driver(&package.info, package.entries[i].name) ? path + 1 : path + strlen(folder) + 1; *p; ++p) {      /* parent folders */
            if (*p != '\\') continue;
            *p = 0;
            int r = target_mkdir(path);
            *p = '\\';
            if (r) { dsp_close(&package); return fail("Cannot create", path); }
        }
        if (directory) {
            if (target_mkdir(path)) { dsp_close(&package); return fail("Cannot create", path); }
            continue;
        }
        unsigned char *data;
        size_t size;
        if (dsp_read_entry(&package, i, &data, &size)) { dsp_close(&package); return fail("Damaged or unreadable:", path); }
        progress(path);
        int r = target_write(path, data, size);
        free(data);
        if (r) { dsp_close(&package); return fail("Cannot write", path); }
    }
    char inf[160], text[1024];
    snprintf(inf, sizeof(inf), "%s\\PACKAGE.INF", folder);
    int length = dsp_format_info(&package.info, text, sizeof(text));
    dsp_close(&package);
    return length < 0 || target_write(inf, text, (unsigned long)length) ? -1 : 0;
}

static int install(const struct disk_choice *disk) {
    char path[64];
    unsigned long boot_size, mbr_size;
    snprintf(path, sizeof(path), "%s\\SYSTEM\\FAT32.BIN", source_root);
    unsigned char *boot = read_file(path, &boot_size);
    snprintf(path, sizeof(path), "%s\\SYSTEM\\MBR.BIN", source_root);
    unsigned char *mbr = read_file(path, &mbr_size);
    if (!boot || !mbr || boot_size != 512 || mbr_size != 512) return fail("Missing", "\\SYSTEM\\FAT32.BIN or MBR.BIN");
    files_total = files_done = 0;
    char system[32], efi[32];
    snprintf(system, sizeof(system), "%s\\SYSTEM", source_root);
    snprintf(efi, sizeof(efi), "%s\\EFI", source_root);
    files_total = count_tree(system) + count_tree(efi);
    for (int i = 0; i < package_count; ++i) {
        if (!packages[i].selected) continue;
        struct dsp package;
        char error[100];
        if (!dsp_open(&package, packages[i].file, error, sizeof(error))) {
            files_total += package.count;
            dsp_close(&package);
        }
    }
    at(progress_row, 4);
    printf("Preparing the disk...");
    fflush(stdout);
    if (target_format(disk->index, (unsigned long)disk->sectors, boot, mbr, (int)sys.bits)) return fail("Cannot format the disk.", 0);
    free(boot);
    free(mbr);
    if (copy_tree(system) || copy_tree(efi)) return -1;
    if (target_mkdir("\\TEMP") || target_mkdir("\\Home") || target_mkdir("\\Programs")) return fail("Cannot create the top folders.", 0);
    for (int i = 0; i < package_count; ++i)
        if (packages[i].selected && install_package(&packages[i])) return -1;
    at(progress_row, 4);
    printf("%-72s", "Finishing the installation...");
    fflush(stdout);
    return target_finish() ? fail("Cannot finish the boot record.", 0) : 0;
}

/* ---- screens -------------------------------------------------------------- */

static int leave(void) {
    console(DOSXC_CONSOLE_COLOR, 0x07 | 0x100, 0);
    console(DOSXC_CONSOLE_CLEAR, 0, 0);
    return 1;
}

int main(void) {
    dosxc_sys_sysinfo(&sys);
    snprintf(source_root, sizeof(source_root), "%c:", sys.drive);
    if (!sys.live) {
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
    find_disks();
    for (;;) {
        int row = screen("DOS-XC requires a partition on your hard drive to copy files to.\n\n"
                         "Installing DOS-XC will erase all data on your hard drive, so please back up\n"
                         "your documents before installing.",
                         "ENTER=Select  ESC=Back  F1=Help  F3=Exit");
        if (!disk_count) {
            at(row, 4);
            printf("No suitable hard disk was found.  Setup needs an internal disk of at");
            at(row + 1, 4);
            printf("least 64 MB with 512-byte sectors.");
        }
        for (int i = 0; i < disk_count; ++i) {
            char size[32];
            size_text(size, sizeof(size), disks[i].sectors);
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
        if (k == KEY_DOWN && selected_disk + 1 < disk_count) ++selected_disk;
        if ((k == '\n' || k == '\r') && disk_count) break;
    }
    find_packages();
packages_screen:
    for (;;) {
        int row = screen("DOS-XC Setup can configure and install the following programs for you\n"
                         "automatically.\n\n"
                         "Select the programs you would like SETUP to install with the arrow keys to\n"
                         "move and SPACE to select.",
                         "ENTER=Continue  SPACE=Select  ESC=Back  F1=Help  F3=Exit");
        if (!package_count) { at(row, 4); printf("(No packages were found on the installation media.)"); }
        for (int i = 0; i < package_count; ++i) {
            const struct package_choice *p = &packages[i];
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
        if (k == 27) { find_disks(); goto welcome; }
        if (k == KEY_UP && cursor) --cursor;
        if (k == KEY_DOWN && cursor + 1 < package_count) ++cursor;
        if (k == ' ' && package_count && !packages[cursor].required) packages[cursor].selected ^= 1;
        if (k == '\n' || k == '\r') break;
    }
    for (;;) {
        char size[32], body[600], list[300] = "";
        size_text(size, sizeof(size), disks[selected_disk].sectors);
        for (int i = 0; i < package_count; ++i) {
            if (!packages[i].selected) continue;
            if (*list) strcat(list, ", ");
            if (strlen(list) + strlen(packages[i].info.name) < sizeof(list) - 8) strcat(list, packages[i].info.name);
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
    if (install(&disks[selected_disk])) {
        char message[600];
        snprintf(message, sizeof(message),
                 "Setup could not complete the installation, so do not start the computer\n"
                 "from this disk.\n\n%.150s\n\n"
                 "Check the disk and its connection, then run Setup again.\n\n"
                 "Press any key to return to the live shell.", failure);
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
