/*
 * DOS-XC /os/setup/install.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "install.h"
#include "../runtime/sha256.h"
#include "target.h"

struct dosxc_sysinfo setup_system;
char setup_root[4];
char setup_failure[160];
struct disk_choice setup_disks[16];
int setup_disk_count;
struct package_choice setup_packages[64];
int setup_package_count;
static setup_progress on_progress;
static void (*on_status)(const char *text);

void setup_init(void) {
    dosxc_sys_sysinfo(&setup_system);
    snprintf(setup_root, sizeof(setup_root), "%c:", setup_system.drive);
}

void setup_find_disks(void) {
    setup_disk_count = 0;
    struct dosxc_disk info;
    int total = dosxc_sys_disk(DOSXC_DISK_INFO, 0, 0, &info, 0);
    for (int i = 0; i < total && setup_disk_count < 16; ++i) {
        if (dosxc_sys_disk(DOSXC_DISK_INFO, (unsigned)i, 0, &info, 0) < 0) continue;
        if (info.flags & (DOSXC_DISK_SYSTEM | DOSXC_DISK_READONLY | DOSXC_DISK_REMOVABLE)) continue;
        if (info.sectors < 131072 || info.sectors > 0xffffffffull) continue;
        setup_disks[setup_disk_count].index = (unsigned)i;
        setup_disks[setup_disk_count++].sectors = info.sectors;
    }
}

static int required(const char *name) { return !strcasecmp(name, "coreutils"); }
static int preselected(const char *name) {
    static const char *const builtins[] = {
        "batch", "DOS-XC Desktop", "dosfox", "openwork", "ethernet",
        "curl", "nano", "pkg", "VLC", "Vim", "xz", "zip"
    };
    if (required(name)) return 1;
    for (unsigned i = 0; i < sizeof(builtins) / sizeof(builtins[0]); ++i)
        if (!strcasecmp(name, builtins[i])) return 1;
    return 0;
}

static int compare_packages(const void *a, const void *b) {
    const struct package_choice *x = a, *y = b;
    if (x->required != y->required) return y->required - x->required;
    return strcasecmp(x->info.name, y->info.name);
}

void setup_find_packages(void) {
    char folder[32];
    snprintf(folder, sizeof(folder), "%s\\Packages", setup_root);
    struct dosxc_dirent *entries = malloc(sizeof(*entries) * 128);
    int count = entries ? (int)dosxc_sys_listdir(folder, entries, 128) : 0;
    setup_package_count = 0;
    for (int i = 0; i < count && i < 128 && setup_package_count < 64; ++i) {
        size_t n = strlen(entries[i].name);
        if (n < 5 || strcasecmp(entries[i].name + n - 4, ".DSP")) continue;
        struct package_choice *p = &setup_packages[setup_package_count];
        snprintf(p->file, sizeof(p->file), "%s\\%s", folder, entries[i].name);
        char error[100];
        if (dsp_read_info(p->file, &p->info, error, sizeof(error))) continue;
        if (p->info.arch != 16 && p->info.arch != (int)setup_system.bits) continue;
        if (!strcasecmp(p->info.name, "system")) continue;      /* Setup copies \SYSTEM itself */
        p->required = required(p->info.name) || p->info.required;
        p->selected = p->required || preselected(p->info.name);
        ++setup_package_count;
    }
    free(entries);
    qsort(setup_packages, (size_t)setup_package_count, sizeof(setup_packages[0]), compare_packages);
    setup_select_requirements();
}

static int requirement_met(const struct dsp_info *system, const char *requirement) {
    if (system && dsp_meets(system, requirement)) return 1;
    for (int i = 0; i < setup_package_count; ++i)
        if (setup_packages[i].selected && dsp_meets(&setup_packages[i].info, requirement)) return 1;
    return 0;
}

int setup_select_requirements(void) {
    struct dsp_info system;
    char path[32], text[2048], item[300];
    snprintf(path, sizeof(path), "%s\\SYSTEM\\PACKAGE.INF", setup_root);
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(text, 1, sizeof(text), f) : 0;
    if (f) fclose(f);
    int have_system = n && !dsp_parse_info(text, n, &system), added = 0;
    for (int changed = 1; changed;) {
        changed = 0;
        for (int i = 0; i < setup_package_count; ++i) {
            if (!setup_packages[i].selected) continue;
            for (const char *p = setup_packages[i].info.requires; (p = dsp_next_item(p, item, sizeof(item)));) {
                if (requirement_met(have_system ? &system : 0, item)) continue;
                for (int j = 0; j < setup_package_count; ++j)
                    if (!setup_packages[j].selected && dsp_meets(&setup_packages[j].info, item)) {
                        setup_packages[j].selected = 1;
                        changed = 1;
                        ++added;
                        break;
                    }
            }
        }
    }
    return added;
}

void setup_size_text(char *out, size_t size, unsigned long long sectors) {
    unsigned long long mb = sectors / 2048;
    if (mb >= 10240) snprintf(out, size, "%llu GB", (mb + 512) / 1024);
    else snprintf(out, size, "%llu MB", mb);
}

static unsigned long files_total, files_done;

static int fail(const char *what, const char *path) {
    snprintf(setup_failure, sizeof(setup_failure), "%s %s", what, path ? path : "");
    return -1;
}
static void progress(const char *name) {
    ++files_done;
    unsigned percent = files_total ? (unsigned)(files_done * 100 / files_total) : 100;
    if (percent > 100) percent = 100;
    on_progress(name, percent, files_done, files_total);
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
        for (char *p = is_driver(&package.info, package.entries[i].name) ? path + 1 : path + strlen(folder) + 1; *p; ++p) {
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
    char inf[160], text[2048];
    snprintf(inf, sizeof(inf), "%s\\PACKAGE.INF", folder);
    int length = dsp_format_info(&package.info, text, sizeof(text));
    dsp_close(&package);
    return length < 0 || target_write(inf, text, (unsigned long)length) ? -1 : 0;
}

struct setup_account setup_account;

long long fat32_clock_seconds(void) { return (long long)dosxc_sys_time(); }

int setup_valid_name(const char *name) {
    if (!*name || strlen(name) >= sizeof(setup_account.name)) return 0;
    for (const char *p = name; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))) return 0;
    return 1;
}

static int write_accounts(void) {
    static const char digits[] = "0123456789abcdef";
    unsigned char salt[16], hash[32];
    __dosxc_syscall2(DOSXC_SYS_RANDOM, (__dosxc_word)salt, sizeof(salt));
    password_hash(salt, setup_account.password, hash);
    char salt_hex[33], hash_hex[65], home[64], folder[64], text[700];
    for (int i = 0; i < 16; ++i) { salt_hex[i * 2] = digits[salt[i] >> 4]; salt_hex[i * 2 + 1] = digits[salt[i] & 15]; }
    for (int i = 0; i < 32; ++i) { hash_hex[i * 2] = digits[hash[i] >> 4]; hash_hex[i * 2 + 1] = digits[hash[i] & 15]; }
    salt_hex[32] = hash_hex[64] = 0;
    snprintf(folder, sizeof(folder), "\\Users\\%s", setup_account.name);
    snprintf(home, sizeof(home), "C:%s", folder);
    int n = snprintf(text, sizeof(text), "accounts\t1\nuser\t%s\t1\t0\t%s\t%s\t%s\tC:\\Programs\\DOS-XC Desktop\\Pictures\\Dog.jpg\t%s\n",
                     setup_account.name, salt_hex, hash_hex, home,
                     setup_account.nickname[0] ? setup_account.nickname : setup_account.name);
    memset(setup_account.password, 0, sizeof(setup_account.password));
    if (target_mkdir(folder) || target_write("\\SYSTEM\\USERS.DB", text, (unsigned long)n)) return fail("Cannot write the user accounts.", 0);
    return 0;
}

int setup_install(const struct disk_choice *disk, setup_progress progress_fn, void (*status)(const char *text)) {
    on_progress = progress_fn;
    on_status = status;
    char path[64];
    unsigned long boot_size, mbr_size;
    snprintf(path, sizeof(path), "%s\\SYSTEM\\FAT32.BIN", setup_root);
    unsigned char *boot = read_file(path, &boot_size);
    snprintf(path, sizeof(path), "%s\\SYSTEM\\MBR.BIN", setup_root);
    unsigned char *mbr = read_file(path, &mbr_size);
    if (!boot || !mbr || boot_size != 512 || mbr_size != 512) return fail("Missing", "\\SYSTEM\\FAT32.BIN or MBR.BIN");
    files_total = files_done = 0;
    char system[32], efi[32];
    snprintf(system, sizeof(system), "%s\\SYSTEM", setup_root);
    snprintf(efi, sizeof(efi), "%s\\EFI", setup_root);
    files_total = count_tree(system) + count_tree(efi);
    setup_select_requirements();
    for (int i = 0; i < setup_package_count; ++i) {
        if (!setup_packages[i].selected) continue;
        struct dsp package;
        char error[100];
        if (!dsp_open(&package, setup_packages[i].file, error, sizeof(error))) {
            files_total += package.count;
            dsp_close(&package);
        }
    }
    on_status("Preparing the disk...");
    if (target_format(disk->index, (unsigned long)disk->sectors, boot, mbr, (int)setup_system.bits)) return fail("Cannot format the disk.", 0);
    free(boot);
    free(mbr);
    if (copy_tree(system) || copy_tree(efi)) return -1;
    if (target_mkdir("\\TEMP") || target_mkdir("\\Users") || target_mkdir("\\Users\\Public") || target_mkdir("\\Programs")) return fail("Cannot create the top folders.", 0);
    for (int i = 0; i < setup_package_count; ++i)
        if (setup_packages[i].selected && install_package(&setup_packages[i])) return -1;
    if (setup_account.enabled && write_accounts()) return -1;
    on_status("Finishing the installation...");
    return target_finish() ? fail("Cannot finish the boot record.", 0) : 0;
}

