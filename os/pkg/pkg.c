/*
 * DOS-XC /os/pkg/pkg.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include <ctype.h>
#include <dosxc/syscall.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dsp.h"
#include "repo.h"

#define SYSTEM_PACKAGE "system"
#define MAX_PACKAGES 256

static char programs[16];
static char system_dir[16];       /* "C:\\SYSTEM", the system package's folder */
static char drivers[24];          /* "C:\\SYSTEM\\DRIVERS" */
static int assume_yes, live;

static int same(const char *a, const char *b) { return !strcasecmp(a, b); }

static int ask(const char *question) {
    if (assume_yes) return 1;
    fputs(question, stdout);
    fflush(stdout);
    for (;;) {
        int k = dosxc_sys_console(DOSXC_CONSOLE_GETKEY, 1, 0);
        if (k == 'y' || k == 'Y') { puts("Y"); return 1; }
        if (k == 'n' || k == 'N' || k == 27) { puts("N"); return 0; }
    }
}

static int is_dir(const char *path) {
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}

static int system_bits(void) {
    struct dosxc_sysinfo info;
    dosxc_sys_sysinfo(&info);
    return (int)info.bits;
}

static void package_folder(const char *name, char *out, size_t size) {
    if (same(name, SYSTEM_PACKAGE)) snprintf(out, size, "%s", system_dir);
    else snprintf(out, size, "%s\\%s", programs, name);
}

/* The installed package NAME (case-insensitive), or -1. */
static int installed(const char *name, struct dsp_info *info, char *folder, size_t size) {
    char inf[400];
    package_folder(name, folder, size);
    snprintf(inf, sizeof(inf), "%s\\PACKAGE.INF", folder);
    FILE *f = fopen(inf, "rb");
    if (!f) return -1;
    char text[2048];
    size_t n = fread(text, 1, sizeof(text), f);
    fclose(f);
    if (dsp_parse_info(text, n, info)) return -1;
    package_folder(info->name, folder, size);
    return 0;
}

/* Every installed package, the system first; free() the list. */
static int load_installed(struct dsp_info **out) {
    struct dsp_info *list = malloc(sizeof(*list) * (MAX_PACKAGES + 1));
    struct dosxc_dirent *entries = malloc(sizeof(*entries) * MAX_PACKAGES);
    int count = 0;
    char folder[300];
    if (list && entries) {
        if (!installed(SYSTEM_PACKAGE, &list[count], folder, sizeof(folder))) ++count;
        int n = (int)dosxc_sys_listdir(programs, entries, MAX_PACKAGES);
        for (int i = 0; i < n && i < MAX_PACKAGES; ++i)
            if ((entries[i].attributes & DOSXC_ATTR_DIRECTORY) && !installed(entries[i].name, &list[count], folder, sizeof(folder)) &&
                same(list[count].name, entries[i].name) && !same(list[count].name, SYSTEM_PACKAGE))
                ++count;
    }
    free(entries);
    *out = list;
    return list ? count : 0;
}

/* Is REQUIREMENT met by EXTRA or by a package of LIST other than SKIP? */
static int met(const struct dsp_info *list, int count, const char *skip, const struct dsp_info *extra, const char *requirement) {
    if (extra && dsp_meets(extra, requirement)) return 1;
    for (int i = 0; i < count; ++i)
        if (!(skip && same(list[i].name, skip)) && dsp_meets(&list[i], requirement)) return 1;
    return 0;
}

/* Prints what stops the installed package NAME (if any) being replaced by
   REPLACEMENT, or removed when REPLACEMENT is 0; returns how many problems. */
static int dependency_problems(const char *name, const struct dsp_info *replacement) {
    struct dsp_info *list;
    int count = load_installed(&list), problems = 0;
    char item[300];
    if (replacement)
        for (const char *p = replacement->requires; (p = dsp_next_item(p, item, sizeof(item)));)
            if (!met(list, count, name, 0, item)) {
                const struct dsp_info *older = 0;
                for (int i = 0; i < count && !older; ++i) {
                    size_t n = strlen(list[i].name);
                    if (!strncasecmp(item, list[i].name, n) && strchr(" >", item[n]) && item[n]) older = &list[i];
                }
                if (older) printf("%s %s needs %s; %s %s is installed.\n", replacement->name, replacement->version, item,
                                  older->name, older->version);
                else printf("%s %s needs %s, which is not installed.\n", replacement->name, replacement->version, item);
                ++problems;
            }
    for (int i = 0; i < count; ++i) {
        if (same(list[i].name, name)) continue;
        for (const char *p = list[i].requires; (p = dsp_next_item(p, item, sizeof(item)));)
            if (met(list, count, 0, 0, item) && !met(list, count, name, replacement, item)) {
                printf("%s needs %s.\n", list[i].name, item);
                ++problems;
            }
    }
    free(list);
    return problems;
}

/* ---- PATH ---------------------------------------------------------------- */

static void each_path(const struct dsp_info *info, void (*fn)(const char *dir)) {
    char list[256];
    snprintf(list, sizeof(list), "%s", info->path);
    for (char *part = strtok(list, ";"); part; part = strtok(0, ";")) {
        char dir[400], folder[300];
        package_folder(info->name, folder, sizeof(folder));
        if (!strcmp(part, ".")) snprintf(dir, sizeof(dir), "%s", folder);
        else snprintf(dir, sizeof(dir), "%s\\%s", folder, part);
        for (char *p = dir; *p; ++p) if (*p == '/') *p = '\\';
        fn(dir);
    }
}

/* Env=NAME=value entries; %PACKAGE% is "C:/Programs/<name>". */
static void each_env(const struct dsp_info *info, int set) {
    char list[512];
    snprintf(list, sizeof(list), "%s", info->env);
    for (char *line = strtok(list, "\n"); line; line = strtok(0, "\n")) {
        char *equals = strchr(line, '=');
        if (!equals) continue;
        *equals = 0;
        if (!set) { dosxc_sys_environ(DOSXC_ENV_SET, line, 0, 0); continue; }
        char value[600], folder[300];
        snprintf(folder, sizeof(folder), "%s/%s", programs, info->name);
        for (char *p = folder; *p; ++p) if (*p == '\\') *p = '/';
        size_t n = 0;
        for (const char *v = equals + 1; *v && n + 1 < sizeof(value);) {
            if (!strncasecmp(v, "%PACKAGE%", 9)) {
                for (const char *f = folder; *f && n + 1 < sizeof(value); ++f) value[n++] = *f;
                v += 9;
            } else value[n++] = *v++;
        }
        value[n] = 0;
        dosxc_sys_environ(DOSXC_ENV_SET, line, value, 0);
    }
}

static void path_remove(const char *dir) {
    char path[4096], out[4096] = "";
    if (dosxc_sys_environ(DOSXC_ENV_GET, "PATH", path, sizeof(path)) < 0) return;
    for (char *part = strtok(path, ";"); part; part = strtok(0, ";")) {
        if (same(part, dir)) continue;
        if (*out) strcat(out, ";");
        strcat(out, part);
    }
    dosxc_sys_environ(DOSXC_ENV_SET, "PATH", out, 0);
}

static void path_add(const char *dir) {
    char path[4096] = "";
    path_remove(dir);
    dosxc_sys_environ(DOSXC_ENV_GET, "PATH", path, sizeof(path));
    if (strlen(path) + strlen(dir) + 2 >= sizeof(path)) return;
    if (*path) strcat(path, ";");
    strcat(path, dir);
    dosxc_sys_environ(DOSXC_ENV_SET, "PATH", path, 0);
}

/* ---- files ----------------------------------------------------------------- */

static int make_dirs(const char *path) {
    char buffer[400];
    snprintf(buffer, sizeof(buffer), "%s", path);
    for (char *p = buffer + 3; *p; ++p) {
        if (*p != '\\') continue;
        *p = 0;
        if (!is_dir(buffer) && mkdir(buffer, 0777)) return -1;
        *p = '\\';
    }
    return is_dir(buffer) || !mkdir(buffer, 0777) ? 0 : -1;
}

static int remove_tree(const char *path) {
    struct dosxc_dirent *entries = malloc(sizeof(*entries) * 1024);
    if (!entries) return -1;
    int failed = 0;
    for (;;) {
        int count = (int)dosxc_sys_listdir(path, entries, 1024);
        if (count <= 0) break;
        int removed = 0;
        for (int i = 0; i < count && i < 1024; ++i) {
            char child[600];
            snprintf(child, sizeof(child), "%s\\%s", path, entries[i].name);
            if (entries[i].attributes & DOSXC_ATTR_DIRECTORY) { if (remove_tree(child)) failed = 1; else ++removed; }
            else if (unlink(child)) failed = 1;
            else ++removed;
        }
        if (!removed || count <= 1024) break;
    }
    free(entries);
    if (rmdir(path)) failed = 1;
    return failed ? -1 : 0;
}

/* ---- commands ----------------------------------------------------------------- */

static const char *arch_name(int arch) { return arch == 16 ? "16-bit DOS" : arch == 32 ? "32-bit" : "64-bit"; }

static void show_info(const struct dsp_info *info) {
    printf("Name:        %s\nVersion:     %s\nAuthor:      %s\nDescription: %s\nArch:        %s\n",
           info->name, info->version, info->author, info->description, arch_name(info->arch));
    if (info->path[0]) printf("Path:        %s\n", info->path);
    if (info->driver[0]) printf("Drivers:     %s\n", info->driver);
    if (info->provides[0]) printf("Provides:    %s\n", info->provides);
    if (info->requires[0]) printf("Requires:    %s\n", info->requires);
    for (const char *p = info->env; *p;) {
        size_t n = strcspn(p, "\n");
        printf("Env:         %.*s\n", (int)n, p);
        p += n;
        if (*p) ++p;
    }
}

static int list_packages(void) {
    struct dsp_info *list;
    int count = load_installed(&list);
    printf("%-16s %-14s %-11s %s\n", "Package", "Version", "Arch", "Description");
    for (int i = 0; i < count; ++i)
        printf("%-16s %-14s %-11s %.36s\n", list[i].name, list[i].version, arch_name(list[i].arch), list[i].description);
    free(list);
    printf("%d package(s) installed.\n", count);
    return 0;
}

static int is_package_file(const char *arg) {
    size_t n = strlen(arg);
    return (n > 4 && same(arg + n - 4, ".DSP")) || strchr(arg, '\\') || strchr(arg, '/');
}

static int info_command(const char *arg, int files) {
    if (!is_package_file(arg)) {
        struct dsp_info info;
        char folder[300];
        if (installed(arg, &info, folder, sizeof(folder))) { printf("Package %s is not installed.\n", arg); return 1; }
        if (!files) { show_info(&info); printf("Folder:      %s\n", folder); return 0; }
        char command[400];
        snprintf(command, sizeof(command), "DIR /S /B \"%s\"", folder);
        return system(command) ? 1 : 0;
    }
    struct dsp package;
    char error[200];
    if (dsp_open(&package, arg, error, sizeof(error))) { printf("%s: %s.\n", arg, error); return 1; }
    if (!files) { show_info(&package.info); printf("Files:       %u\n", package.count); }
    else for (unsigned i = 0; i < package.count; ++i) printf("%10lu  %s\n", package.entries[i].size, package.entries[i].name);
    dsp_close(&package);
    return 0;
}

static int extract(struct dsp *package, const char *folder) {
    if (make_dirs(folder)) { printf("Cannot create %s.\n", folder); return -1; }
    for (unsigned i = 0; i < package->count; ++i) {
        const struct dsp_entry *e = &package->entries[i];
        char path[600];
        snprintf(path, sizeof(path), "%s\\%s", folder, e->name);
        for (char *p = path; *p; ++p) if (*p == '/') *p = '\\';
        size_t n = strlen(path);
        if (n && path[n - 1] == '\\') {
            path[n - 1] = 0;
            if (make_dirs(path)) { printf("Cannot create %s.\n", path); return -1; }
            continue;
        }
        char *cut = strrchr(path, '\\');
        *cut = 0;
        if (make_dirs(path)) { printf("Cannot create %s.\n", path); return -1; }
        *cut = '\\';
        unsigned char *data;
        size_t size;
        if (dsp_read_entry(package, i, &data, &size)) { printf("%s is damaged in the package.\n", e->name); return -1; }
        FILE *f = fopen(path, "wb");
        int failed = !f || fwrite(data, 1, size, f) != size;
        if (f && fclose(f)) failed = 1;
        free(data);
        if (failed) { printf("Cannot write %s (is the disk full?).\n", path); return -1; }
        printf("\r  %3u%%  %u of %u files", (i + 1) * 100 / package->count, i + 1, package->count);
        fflush(stdout);
    }
    if (package->count) putchar('\n');
    char inf[400], text[2048];
    snprintf(inf, sizeof(inf), "%s\\PACKAGE.INF", folder);
    int length = dsp_format_info(&package->info, text, sizeof(text));
    FILE *f = fopen(inf, "wb");
    if (!f || length < 0 || fwrite(text, 1, (size_t)length, f) != (size_t)length || fclose(f)) {
        printf("Cannot write %s.\n", inf);
        return -1;
    }
    return 0;
}

/* ---- drivers: a package's Driver= files live in \\SYSTEM\\DRIVERS ------------ */

static void driver_path(const char *name, char *out, size_t size) { snprintf(out, size, "%s\\%s", drivers, name); }

static void each_driver(const struct dsp_info *info, void (*fn)(const char *name)) {
    char list[256];
    snprintf(list, sizeof(list), "%s", info->driver);
    for (char *name = strtok(list, ";"); name; name = strtok(0, ";")) fn(name);
}

static int driver_file_exists(const char *name) {
    char path[300];
    driver_path(name, path, sizeof(path));
    struct stat st;
    return !stat(path, &st);
}

static void driver_stop(const char *name) {
    char file[64], path[300];
    snprintf(file, sizeof(file), "%s", name);
    dosxc_sys_driver(DOSXC_DRIVER_UNLOAD, 0, file, 0);
    driver_path(name, path, sizeof(path));
    unlink(path);
}

static const char *install_folder;
static int driver_failed;

static void driver_install(const char *name) {
    char from[400], to[300];
    snprintf(from, sizeof(from), "%s\\%s", install_folder, name);
    driver_path(name, to, sizeof(to));
    FILE *in = fopen(from, "rb");
    if (!in) { printf("The package does not contain %s.\n", name); driver_failed = 1; return; }
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    unsigned char *data = malloc(size > 0 ? (size_t)size : 1);
    FILE *out = fopen(to, "wb");
    int ok = data && out && fread(data, 1, (size_t)size, in) == (size_t)size && fwrite(data, 1, (size_t)size, out) == (size_t)size;
    fclose(in);
    if (out && fclose(out)) ok = 0;
    free(data);
    if (!ok) { printf("Cannot write %s.\n", to); driver_failed = 1; return; }
    unlink(from);
    int r = dosxc_sys_driver(DOSXC_DRIVER_LOAD, 0, to, 0);
    if (r < 0 && r != -EEXIST) printf("%s was installed, but could not start: %s.  See DRIVERS /LOG.\n", name, strerror(-r));
}

static int uninstall(const struct dsp_info *info, const char *folder) {
    each_path(info, path_remove);
    each_env(info, 0);
    each_driver(info, driver_stop);
    if (remove_tree(folder)) { printf("Some files in %s could not be removed.\n", folder); return -1; }
    return 0;
}

static int owns_driver(const struct dsp_info *info, const char *name) {
    char list[256];
    snprintf(list, sizeof(list), "%s", info->driver);
    for (char *part = strtok(list, ";"); part; part = strtok(0, ";"))
        if (same(part, name)) return 1;
    return 0;
}

/* The system package: the boot core that cannot be replaced while it runs.
   It is unpacked into \\SYSTEM.NEW, and SHELL.DLL moves it into \\SYSTEM at
   the next start, keeping the files it replaces in \\SYSTEM.OLD until the
   new version has started (os/programs/shell.c). */
#define BDLL 0x4c4c4442u
#define BDEF 0x46454442u
static const struct boot_file { const char *name; unsigned long limit; unsigned magic; int bits, needed; } boot_files[] = {
    {"KERNEL.BIN", 65536, 0, 0, 1}, {"CORE.DLL", 65536, BDLL, 0, 1}, {"SHELL.DLL", 65536, BDLL, 0, 1},
    {"DOSAPI.DLL", 0x6000, 0, 0, 1}, {"ELFAPI.DLL", 0xc00000, BDEF, 0, 1}, {"COMMAND.EXE", 0, 0, 0, 1},
    {"LIB/libc.so", 0, 0, 0, 1}, {"KERNELU.BIN", 65536, 0, 64, 0}, {"COREU.DLL", 65536, BDLL, 64, 0},
    {"SHELLU.DLL", 65536, BDLL, 64, 0},
};

/* Files that stay where they are: the boot sector finds the first three by
   their place on the disk, and the rest are this computer's settings. */
static const char *const kept_files[] = {"LOADER.BIN", "SETUP.CFG", "DOSDATA.BIN", "USERS.DB", "NET.CFG", "FIREWALL.CFG", "PACKAGE.INF"};

static int check_system_files(struct dsp *package, int bits) {
    for (unsigned i = 0; i < package->count; ++i) {
        const char *name = package->entries[i].name, *slash = strchr(name, '/');
        if (strstr(name, "..") || (slash && (strncasecmp(name, "LIB/", 4) || strchr(slash + 1, '/')))) {
            printf("The system package cannot contain %s: only files of \\SYSTEM and \\SYSTEM\\LIB.\n", name);
            return -1;
        }
        for (unsigned k = 0; k < sizeof(kept_files) / sizeof(*kept_files); ++k)
            if (same(name, kept_files[k])) { printf("The system package cannot replace %s.\n", name); return -1; }
    }
    for (unsigned k = 0; k < sizeof(boot_files) / sizeof(*boot_files); ++k) {
        const struct boot_file *b = &boot_files[k];
        unsigned i = 0;
        while (i < package->count && !same(package->entries[i].name, b->name)) ++i;
        if (i == package->count) {
            if (!b->needed) continue;
            printf("The system package has no %s.\n", b->name);
            return -1;
        }
        if (b->limit && package->entries[i].size > b->limit) { printf("%s is too large to start.\n", b->name); return -1; }
        if (!b->magic) continue;
        unsigned char *data;
        size_t size;
        if (dsp_read_entry(package, i, &data, &size)) { printf("%s is damaged in the package.\n", b->name); return -1; }
        unsigned long magic = size >= 12 ? data[0] | data[1] << 8 | data[2] << 16 | (unsigned long)data[3] << 24 : 0;
        int ok = magic == b->magic && data[8] == (b->bits ? b->bits : bits) && (b->magic != BDLL || data[4] == 1);
        free(data);
        if (!ok) { printf("%s is not a %d-bit DOS-XC system module.\n", b->name, b->bits ? b->bits : bits); return -1; }
    }
    return 0;
}

static int stage_system(struct dsp *package, int force) {
    const struct dsp_info *info = &package->info;
    if (live) {
        puts("The live system runs from the installation media, which cannot be updated.\n"
             "Install DOS-XC with SETUP first.");
        return 1;
    }
    char trial[48], staging[32], previous[32], folder[32];
    snprintf(staging, sizeof(staging), "%s.NEW", system_dir);
    snprintf(previous, sizeof(previous), "%s.OLD", system_dir);
    snprintf(trial, sizeof(trial), "%s\\TRIAL", previous);
    struct stat st;
    if (!stat(trial, &st)) {
        puts("The last system update has not started yet.  Restart DOS-XC before updating again.");
        return 1;
    }
    struct dsp_info old;
    int present = !installed(SYSTEM_PACKAGE, &old, folder, sizeof(folder));
    if (present && !force && dsp_compare_versions(info->version, old.version) <= 0) {
        printf("DOS-XC system %s is installed; the package has version %s.  Use /F to install it anyway.\n", old.version,
               info->version);
        return 1;
    }
    if (check_system_files(package, system_bits())) return 1;
    if (dependency_problems(SYSTEM_PACKAGE, info) && !force) { puts("Use /F to update the system anyway."); return 1; }
    char question[300];
    snprintf(question, sizeof(question), "Update the DOS-XC system from %s to %s when DOS-XC next starts (Y/N)? ",
             present ? old.version : "an unknown version", info->version);
    if (!ask(question)) return 1;
    if ((is_dir(staging) && remove_tree(staging)) || (is_dir(previous) && remove_tree(previous))) {
        printf("Cannot clear %s and %s.\n", staging, previous);
        return 1;
    }
    if (extract(package, staging)) {
        puts("The update could not be unpacked; the system is unchanged.");
        remove_tree(staging);
        return 1;
    }
    printf("DOS-XC system %s is ready in %s.  It is put in place when DOS-XC restarts\n"
           "(type RESTART); if it fails to start, the current version comes back.\n", info->version, staging);
    return 0;
}

/* An update unpacks next to the old folder and swaps it in only when every
   file is out, so a failed update (even of PKG itself) leaves the old one. */
static int install_command(const char *file, int update, int force) {
    struct dsp package;
    char error[200];
    if (dsp_open(&package, file, error, sizeof(error))) { printf("%s: %s.\n", file, error); return 1; }
    const struct dsp_info *info = &package.info;
    int bits = system_bits();
    if (info->arch != 16 && info->arch != bits) {
        printf("%s %s is a %s package; this edition of DOS-XC runs %d-bit programs.\n", info->name, info->version,
               arch_name(info->arch), bits);
        dsp_close(&package);
        return 1;
    }
    if (same(info->name, SYSTEM_PACKAGE)) {
        int r = stage_system(&package, force);
        dsp_close(&package);
        return r;
    }
    struct dsp_info old;
    char folder[300];
    int present = !installed(info->name, &old, folder, sizeof(folder));
    if (present && !update) {
        printf("%s %s is already installed.  Use PKG UPDATE to replace it.\n", old.name, old.version);
        dsp_close(&package);
        return 1;
    }
    if (present && !force && dsp_compare_versions(info->version, old.version) <= 0) {
        printf("%s %s is installed; the package has version %s.  Use /F to install it anyway.\n", old.name, old.version, info->version);
        dsp_close(&package);
        return 1;
    }
    if (dependency_problems(info->name, info) && !force) {
        puts("Use /F to install it anyway.");
        dsp_close(&package);
        return 1;
    }
    char question[300];
    if (present) snprintf(question, sizeof(question), "Replace %s %s with %s (Y/N)? ", old.name, old.version, info->version);
    else snprintf(question, sizeof(question), "Install %s %s (%s) to %s\\%s (Y/N)? ", info->name, info->version,
                  info->description, programs, info->name);
    if (!ask(question)) { dsp_close(&package); return 1; }
    if (!present) package_folder(info->name, folder, sizeof(folder));
    char list[256];
    snprintf(list, sizeof(list), "%s", info->driver);
    for (char *name = strtok(list, ";"); name; name = strtok(0, ";"))
        if (driver_file_exists(name) && !(present && owns_driver(&old, name))) {
            printf("%s\\%s already exists; it belongs to another package.\n", drivers, name);
            dsp_close(&package);
            return 1;
        }
    char staging[320], previous[320];
    snprintf(staging, sizeof(staging), "%s.new", folder);
    snprintf(previous, sizeof(previous), "%s.old", folder);
    if (is_dir(staging)) remove_tree(staging);
    if (is_dir(previous)) remove_tree(previous);
    if (make_dirs(programs) || (info->driver[0] && make_dirs(drivers)) || extract(&package, staging)) {
        printf("Installation failed; %s\n", present ? "the installed version is unchanged." : "removing what was copied.");
        remove_tree(staging);
        dsp_close(&package);
        return 1;
    }
    dsp_close(&package);
    if (present && rename(folder, previous)) {
        printf("Cannot move %s aside; the installed version is unchanged.\n", folder);
        remove_tree(staging);
        return 1;
    }
    if (rename(staging, folder)) {
        printf("Cannot move the new files into %s.\n", folder);
        if (present) rename(previous, folder);
        remove_tree(staging);
        return 1;
    }
    if (present) {
        each_path(&old, path_remove);
        each_env(&old, 0);
        each_driver(&old, driver_stop);
    }
    install_folder = folder;
    driver_failed = 0;
    each_driver(info, driver_install);
    if (driver_failed) {
        printf("Installation failed; removing what was copied.\n");
        remove_tree(folder);
        if (present && !rename(previous, folder)) {
            install_folder = folder;
            each_path(&old, path_add);
            each_env(&old, 1);
            puts("The old version is back, but its drivers need it installed again.");
        }
        return 1;
    }
    each_path(info, path_add);
    each_env(info, 1);
    if (present && remove_tree(previous)) printf("Some old files in %s could not be removed.\n", previous);
    if (info->firewall[0]) dosxc_sys_net(DOSXC_NET_FIREWALL, 0, 0);
    printf("%s %s is installed in %s.\n", info->name, info->version, folder);
    return 0;
}

static int remove_command(const char *name, int force) {
    struct dsp_info info;
    char folder[300];
    if (installed(name, &info, folder, sizeof(folder))) { printf("Package %s is not installed.\n", name); return 1; }
    if (info.required) {
        printf("%s is part of DOS-XC and cannot be removed; PKG UPDATE replaces it with a newer version.\n", info.name);
        return 1;
    }
    if (dependency_problems(info.name, 0) && !force) {
        printf("Remove those first, or use /F to remove %s anyway.\n", info.name);
        return 1;
    }
    char question[300];
    if (same(info.name, "coreutils"))
        snprintf(question, sizeof(question), "coreutils provides DIR, COPY, DEL and the other basic commands.  Remove it (Y/N)? ");
    else snprintf(question, sizeof(question), "Remove %s %s and everything in %s (Y/N)? ", info.name, info.version, folder);
    if (!ask(question)) return 1;
    if (uninstall(&info, folder)) return 1;
    if (info.firewall[0]) dosxc_sys_net(DOSXC_NET_FIREWALL, 0, 0);
    printf("%s has been removed.\n", info.name);
    return 0;
}

/* Version 2 packing: the files' data, one after another, before XZ.EXE packs it. */
struct solid { FILE *stream; struct dsp_file *files; unsigned count, capacity; };

static int solid_add(struct solid *solid, const char *name, const unsigned char *data, size_t size) {
    if (strlen(name) >= sizeof(solid->files[0].name)) { printf("The name %s is too long for a package.\n", name); return -1; }
    if (solid->count == solid->capacity) {
        unsigned capacity = solid->capacity ? solid->capacity * 2 : 256;
        struct dsp_file *bigger = realloc(solid->files, capacity * sizeof(*bigger));
        if (!bigger) { puts("Out of memory."); return -1; }
        solid->files = bigger;
        solid->capacity = capacity;
    }
    struct dsp_file *f = &solid->files[solid->count++];
    snprintf(f->name, sizeof(f->name), "%s", name);
    f->size = (unsigned long)size;
    f->crc = dsp_crc32(0, data, size);
    if (size && fwrite(data, 1, size, solid->stream) != size) { puts("Cannot write the package (is the disk full?)."); return -1; }
    return 0;
}

/* Collect every file under FOLDER (relative names with '/') into W, or SOLID. */
static int pack_tree(struct dsp_writer *w, struct solid *solid, const char *folder, const char *relative, unsigned *count) {
    char path[600];
    snprintf(path, sizeof(path), "%s%s%s", folder, *relative ? "\\" : "", relative);
    struct dosxc_dirent *entries = malloc(sizeof(*entries) * 1024);
    int n = entries ? (int)dosxc_sys_listdir(path, entries, 1024) : -1;
    if (n < 0 || n > 1024) { free(entries); printf("Cannot read %s.\n", path); return -1; }
    for (int i = 0; i < n; ++i) {
        char name[600], full[900];
        snprintf(name, sizeof(name), "%s%s%s", relative, *relative ? "/" : "", entries[i].name);
        snprintf(full, sizeof(full), "%s\\%s", path, entries[i].name);
        if (entries[i].attributes & DOSXC_ATTR_DIRECTORY) {
            if (pack_tree(w, solid, folder, name, count)) { free(entries); return -1; }
            continue;
        }
        if (!*relative && same(entries[i].name, "PACKAGE.INF")) continue;
        FILE *f = fopen(full, "rb");
        unsigned char *data = malloc(entries[i].size ? (size_t)entries[i].size : 1);
        if (!f || !data || fread(data, 1, (size_t)entries[i].size, f) != (size_t)entries[i].size) {
            if (f) fclose(f);
            free(data);
            free(entries);
            printf("Cannot read %s.\n", full);
            return -1;
        }
        fclose(f);
        int r = solid ? solid_add(solid, name, data, (size_t)entries[i].size) : dsp_add(w, name, data, (size_t)entries[i].size);
        free(data);
        if (r) {
            free(entries);
            if (!solid) puts("Cannot write the package (is the disk full?).");
            return -1;
        }
        printf("  %s\n", name);
        ++*count;
    }
    free(entries);
    return 0;
}

/* Packs FOLDER as version 2 with XZ.EXE from the xz package: 0 when packed,
   -1 when that failed, 1 when the xz package is not installed, 2 when XZ failed. */
static int pack_solid(const char *folder, const char *out, const struct dsp_info *info, unsigned *count) {
    char xz[64], stream[320], packed[330];
    snprintf(xz, sizeof(xz), "%.2s\\Programs\\xz\\BIN\\XZ.EXE", programs);
    struct stat st;
    if (stat(xz, &st)) return 1;
    snprintf(stream, sizeof(stream), "%s.tmp", out);
    snprintf(packed, sizeof(packed), "%s.xz", stream);
    remove(packed);
    struct solid solid = {0};
    solid.stream = fopen(stream, "wb");
    if (!solid.stream) { printf("Cannot create %s.\n", stream); return -1; }
    int failed = pack_tree(0, &solid, folder, "", count);
    if (fclose(solid.stream)) failed = 1;
    if (!failed) {
        puts("Compressing with XZ...");
        char *argv[10] = {"XZ.EXE", "-z", "-f", "-q", "--format=xz", "--check=crc32", "--lzma2=preset=6e", stream, 0, 0};
        if (info->arch != 16) { argv[6] = "--x86"; argv[7] = "--lzma2=preset=6e"; argv[8] = stream; }
        fflush(stdout);
        long status = dosxc_sys_spawn(xz, argv, 0);
        if (status < 0 || (status >> 8) & 0xff) {
            remove(stream);
            remove(packed);
            free(solid.files);
            return 2;
        }
    }
    FILE *xz_file = failed ? 0 : fopen(packed, "rb");
    if (!failed && (!xz_file || dsp_write_solid(out, info, solid.files, solid.count, xz_file))) {
        printf("Cannot write %s (is the disk full?).\n", out);
        failed = 1;
    }
    if (xz_file) fclose(xz_file);
    remove(stream);
    remove(packed);
    free(solid.files);
    return failed ? -1 : 0;
}

static int pack_command(int argc, char **argv) {
    if (argc < 2) { puts("PKG PACK folder out.dsp [/N:name] [/V:version] [/A:author] [/D:description] [/B:bits] [/P:path] [/S:program]\n"
                         "         [/O:provides] [/U:requires] [/Z:1]"); return 1; }
    int zip_format = 0;
    const char *folder = argv[0], *out = argv[1];
    struct dsp_info info;
    memset(&info, 0, sizeof(info));
    char inf[400], text[2048];
    snprintf(inf, sizeof(inf), "%s\\PACKAGE.INF", folder);
    FILE *f = fopen(inf, "rb");
    if (f) {
        size_t n = fread(text, 1, sizeof(text), f);
        fclose(f);
        dsp_parse_info(text, n, &info);
    }
    for (int i = 2; i < argc; ++i) {
        const char *a = argv[i];
        if (a[0] != '/' || !a[1] || a[2] != ':') { printf("Unknown option %s\n", a); return 1; }
        const char *v = a + 3;
        switch (toupper((unsigned char)a[1])) {
        case 'N': snprintf(info.name, sizeof(info.name), "%s", v); break;
        case 'V': snprintf(info.version, sizeof(info.version), "%s", v); break;
        case 'A': snprintf(info.author, sizeof(info.author), "%s", v); break;
        case 'D': snprintf(info.description, sizeof(info.description), "%s", v); break;
        case 'B': info.arch = atoi(v); break;
        case 'P': snprintf(info.path, sizeof(info.path), "%s", v); break;
        case 'S': snprintf(info.start_in, sizeof(info.start_in), "%s", v); break;
        case 'R': snprintf(info.driver, sizeof(info.driver), "%s", v); break;
        case 'Q': info.required = *v == '1'; break;
        case 'O': snprintf(info.provides, sizeof(info.provides), "%s", v); break;
        case 'U': snprintf(info.requires, sizeof(info.requires), "%s", v); break;
        case 'Z': zip_format = *v == '1'; break;
        case 'E': {
            size_t used = strlen(info.env);
            if (used + strlen(v) + 2 < sizeof(info.env)) snprintf(info.env + used, sizeof(info.env) - used, "%s%s", used ? "\n" : "", v);
            break;
        }
        default: printf("Unknown option %s\n", a); return 1;
        }
    }
    if (!dsp_valid_name(info.name) || !info.version[0] || (info.arch != 16 && info.arch != 32 && info.arch != 64)) {
        puts("A package needs a name (letters, digits, spaces, - _ .), a version and an architecture (/B:16, 32 or 64).\n"
             "Give them with the options or in PACKAGE.INF inside the folder.");
        return 1;
    }
    if (!is_dir(folder)) { printf("%s is not a folder.\n", folder); return 1; }
    unsigned count = 0;
    if (!zip_format) {
        int r = pack_solid(folder, out, &info, &count);
        if (r <= 0) {
            if (r) { remove(out); puts("Packing failed."); return 1; }
            printf("Packed %u file(s) into %s (%s %s).\n", count, out, info.name, info.version);
            return 0;
        }
        if (r == 2) puts("XZ could not compress the package, so it is packed the older, larger way.");
        else puts("The xz package is not installed, so this package is packed the older, larger way\n"
                  "(PKG INSTALL xz.dsp gives smaller packages).");
        count = 0;
    }
    struct dsp_writer *w = dsp_create(out, &info);
    if (!w) { printf("Cannot create %s.\n", out); return 1; }
    int failed = pack_tree(w, 0, folder, "", &count);
    if (dsp_finish(w) || failed) { remove(out); puts("Packing failed."); return 1; }
    printf("Packed %u file(s) into %s (%s %s, format 1).\n", count, out, info.name, info.version);
    return 0;
}

#ifndef CATALOG_CHUNK
#define CATALOG_CHUNK (50ul*1024*1024)
#endif

/* curl provides TLS and certificate validation; no command shell parses URLs. range may be 0. */
static int catalog_get(const char *url, const char *output, unsigned long maximum, const char *range) {
    char curl[100],ca[100],limit[24];
    snprintf(curl,sizeof(curl),"%s\\curl\\BIN\\CURL.EXE",programs);
    snprintf(ca,sizeof(ca),"%s\\curl\\CACERT.PEM",programs);
    if (access(curl,F_OK)) { puts("Online packages need curl. Install curl from a local .dsp file first."); return -1; }
    const char *protocol=!strncmp(url,"https://",8) ? "=https" : "=http,https";
    snprintf(limit,sizeof(limit),"%lu",maximum);
    char *argv[]={"CURL.EXE","--disable","--fail-with-body","--silent","--show-error",
        "--location","--max-redirs","3","--connect-timeout","15","--max-time","300",
        "--proto",(char *)protocol,"--proto-redir",(char *)protocol,
        "--cacert",ca,"--max-filesize",limit,"--output",(char *)output,"--url",(char *)url,
        range ? "--range" : 0,(char *)range,0};
    fflush(stdout);
    long status=dosxc_sys_spawn(curl,argv,0);
    if (status<0 || status) {
        puts("The package catalog request failed (check the network, clock and curl certificates).");
        FILE *f=fopen(output,"rb");
        if (f) { char message[201]; size_t n=fread(message,1,sizeof(message)-1,f); fclose(f); message[n]=0;
            if (n && message[0]=='{') puts(message); }
        return -1;
    }
    return 0;
}

/* Large packages arrive as ranged chunks so proxies that cap transfer sizes pass them. */
static int catalog_transfer(const char *url, const char *output, unsigned long maximum, int exact, void *context) {
    (void)context;
    if (!exact || maximum<=CATALOG_CHUNK) return catalog_get(url,output,maximum,0);
    char part[360],range[48];
    if (snprintf(part,sizeof(part),"%s.part",output)>=(int)sizeof(part)) return -1;
    FILE *out=fopen(output,"wb");
    if (!out) { printf("Cannot create %s.\n",output); return -1; }
    int failed=0;
    for (unsigned long start=0;!failed && start<maximum;start+=CATALOG_CHUNK) {
        unsigned long length=maximum-start<CATALOG_CHUNK ? maximum-start : CATALOG_CHUNK;
        snprintf(range,sizeof(range),"%lu-%lu",start,start+length-1);
        printf("  %lu%%\r",start/(maximum/100));
        if (catalog_get(url,part,maximum,range)) { failed=1; break; }
        FILE *in=fopen(part,"rb"); unsigned char block[8192]; size_t n; unsigned long copied=0;
        if (!in) { failed=1; break; }
        while ((n=fread(block,1,sizeof(block),in))) {
            if (fwrite(block,1,n,out)!=n) { failed=1; break; }
            copied+=(unsigned long)n;
        }
        if (ferror(in)) failed=1;
        fclose(in);
        /* A server without Range support sends the whole package at once. */
        if (!failed && !start && copied==maximum) break;
        if (!failed && copied!=length) { puts("The package catalog sent an incomplete download."); failed=1; }
    }
    remove(part);
    if (fclose(out)) failed=1;
    if (failed) remove(output);
    return failed ? -1 : 0;
}

static int save_download(const char *source, const char *target) {
    int fd=open(target,O_WRONLY|O_CREAT|O_EXCL,0666);
    if (fd<0) { printf("Cannot create %s (it may already exist).\n",target); return 1; }
    FILE *out=fdopen(fd,"wb"),*in=fopen(source,"rb");
    int failed=!out || !in;
    if (!out) close(fd);
    unsigned char block[8192]; size_t n;
    while (!failed && (n=fread(block,1,sizeof(block),in))) if (fwrite(block,1,n,out)!=n) failed=1;
    if (in) { if (ferror(in)) failed=1; fclose(in); }
    if (out && fclose(out)) failed=1;
    if (failed) { remove(target); printf("Cannot save %s.\n",target); }
    return failed;
}

static int online_command(const char *command, const char *argument, const char *output, int force) {
    char work[300],file[340];
    const char *temp=getenv("TMPDIR");
    if (!temp || !*temp) temp=getenv("TEMP");
    if (!temp || !*temp) temp="\\TEMP";
    if (snprintf(work,sizeof(work),"%s/pkg-XXXXXX",temp)>=(int)sizeof(work) || !mkdtemp(work)) {
        puts("Cannot create a temporary directory for the download."); return 1;
    }
    struct repo repo; int result=1;
    const char *allow=getenv("PKG_ALLOW_HTTP");
    if (repo_init(&repo,getenv("PKG_REPOSITORY"),allow && !strcmp(allow,"1"),system_bits(),work,catalog_transfer,0)) {
        puts("PKG_REPOSITORY must be an HTTPS origin, such as " PKG_REPOSITORY ".");
        rmdir(work); return 1;
    }
    if (same(command,"SEARCH") || same(command,"CATALOG")) {
        struct repo_entry *entries=malloc(REPO_MAX_PACKAGES*sizeof(*entries));
        int count=entries ? repo_search(&repo,argument,entries,REPO_MAX_PACKAGES) : -1;
        if (count>=0) {
            if (same(command,"CATALOG")) result=repo_write_index(output,entries,count)!=0;
            else {
                for (int i=0;i<count;++i) printf("%-24s %-12s %2d-bit  %s\n",entries[i].info.name,entries[i].info.version,entries[i].info.arch,entries[i].info.description);
                printf("%d package(s).\n",count); result=0;
            }
        }
        free(entries);
    } else {
        struct repo_entry entry;
        if (!repo_resolve(&repo,argument,&entry)) {
            if (same(command,"SHOW")) { show_info(&entry.info); result=0; }
            else {
                printf("Downloading %s %s (%lu bytes)...\n",entry.info.name,entry.info.version,entry.size);
                if (!repo_fetch(&repo,&entry,file,sizeof(file))) {
                    if (same(command,"FETCH")) {
                        char destination[160];
                        snprintf(destination,sizeof(destination),"%s-%s-%d.dsp",entry.info.name,entry.info.version,entry.info.arch);
                        result=save_download(file,output ? output : destination);
                        if (!result) printf("Saved %s (SHA-256 verified).\n",output ? output : destination);
                    } else result=install_command(file,same(command,"UPDATE"),force);
                    remove(file);
                }
            }
        }
    }
    rmdir(work); return result;
}

static int local_package(const char *name) {
    size_t n=strlen(name);
    return strchr(name,'/') || strchr(name,'\\') || strchr(name,':') || (n>=4 && !strcasecmp(name+n-4,".dsp"));
}

static void help(void) {
    puts("DOS-XC Package Manager\n\n"
         "  PKG LIST                        Lists installed packages\n"
         "  PKG INFO file.dsp | name        Shows package details\n"
         "  PKG FILES file.dsp | name       Lists the files in a package\n"
         "  PKG SEARCH [text]               Searches the online catalog\n"
         "  PKG SHOW name                   Shows online package details\n"
         "  PKG FETCH name [out.dsp]        Downloads and verifies a package\n"
         "  PKG INSTALL file.dsp|name [/Y] [/F]  Installs a local or online package\n"
         "  PKG UPDATE file.dsp|name [/Y] [/F]   Replaces an installed package with a newer version;\n"
         "                                  the system package is put in place at the next start\n"
         "  PKG REMOVE name [/Y] [/F]       Uninstalls a package\n"
         "  PKG PACK folder out.dsp [/N:name] [/V:version] [/A:author] [/D:text]\n"
         "           [/B:16|32|64] [/P:folders-for-PATH] [/S:program] [/E:NAME=value]\n"
         "           [/R:A.DRV;B.DRV] [/O:roles] [/U:name>=version;...] [/Q:1] [/Z:1]\n"
         "                                  Makes a package; /S:program runs 16-bit\n"
         "                                  programs in their own folder; /R:files\n"
         "                                  are device drivers for \\SYSTEM\\DRIVERS;\n"
         "                                  /O: lists roles it provides (desktop);\n"
         "                                  /U: lists packages or roles it needs;\n"
         "                                  /Q:1 marks it required (never removed);\n"
         "                                  /Z:1 packs the older ZIP format.  It is\n"
         "                                  compressed with XZ when the xz package\n"
         "                                  is installed\n\n"
         "/Y answers yes to questions.  /F installs an older version, or goes ahead\n"
         "although other packages need what is removed or replaced.\n"
         "Online commands require curl and use " PKG_REPOSITORY ".\n"
         "SET PKG_REPOSITORY=https://host selects another catalog.");
}

int main(int argc, char **argv) {
    struct dosxc_sysinfo sys;
    dosxc_sys_sysinfo(&sys);
    snprintf(programs, sizeof(programs), "%c:\\Programs", sys.drive);
    snprintf(system_dir, sizeof(system_dir), "%c:\\SYSTEM", sys.drive);
    snprintf(drivers, sizeof(drivers), "%c:\\SYSTEM\\DRIVERS", sys.drive);
    live = sys.live;
    setvbuf(stdout, 0, _IOLBF, 0);
    char *args[64];
    int count = 0, force = 0;
    for (int i = 1; i < argc && count < 64; ++i) {
        if (same(argv[i], "/Y")) assume_yes = 1;
        else if (same(argv[i], "/F")) force = 1;
        else args[count++] = argv[i];
    }
    if (!count || same(args[0], "HELP") || !strcmp(args[0], "/?")) { help(); return !count; }
    const char *command = args[0];
    if (same(command, "LIST")) return list_packages();
    if (same(command, "PACK")) return pack_command(count - 1, args + 1);
    if (same(command,"SEARCH") && count<=2) return online_command(command,count==2 ? args[1] : "",0,force);
    if (same(command,"FETCH") && (count==2 || count==3)) return online_command(command,args[1],count==3 ? args[2] : 0,force);
    if (same(command,"CATALOG") && count==2) return online_command(command,"",args[1],force);
    if (count != 2) { help(); return 1; }
    if (same(command, "INFO")) return info_command(args[1], 0);
    if (same(command, "FILES")) return info_command(args[1], 1);
    if (same(command,"SHOW")) return online_command(command,args[1],0,force);
    if ((same(command,"INSTALL") || same(command,"UPDATE")) && !local_package(args[1])) return online_command(command,args[1],0,force);
    if (same(command, "INSTALL")) return install_command(args[1], 0, force);
    if (same(command, "UPDATE")) return install_command(args[1], 1, force);
    if (same(command, "REMOVE") || same(command, "UNINSTALL")) return remove_command(args[1], force);
    printf("Unknown command %s.\n", command);
    help();
    return 1;
}
