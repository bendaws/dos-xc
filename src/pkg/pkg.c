/*
 * DOS-XC /pkg/pkg.c - PKG.EXE, the DOS-XC Package Manager
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 *   PKG LIST                         installed packages
 *   PKG INFO file.dsp | name         package details
 *   PKG FILES file.dsp | name        files in a package
 *   PKG INSTALL file.dsp [/Y]        install to \Programs\<name>
 *   PKG UPDATE file.dsp [/Y] [/F]    replace an installed package with a newer one
 *   PKG REMOVE name [/Y]             uninstall
 *   PKG PACK folder out.dsp [/N:name] [/V:version] [/A:author]
 *            [/D:description] [/B:16|32|64] [/P:path] [/S:program] [/R:drivers]
 *
 * Installed packages live in \Programs\<name> on the system drive; each
 * keeps its metadata in PACKAGE.INF there, which the shell reads at start
 * to build PATH.  PKG updates PATH right away too.
 */

#include <ctype.h>
#include <dosxc/syscall.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dsp.h"

static char programs[16];
static char drivers[24];          /* "C:\\SYSTEM\\DRIVERS" */
static int assume_yes;

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

static void package_folder(const char *name, char *out, size_t size) { snprintf(out, size, "%s\\%s", programs, name); }

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
    for (const char *p = info->env; *p;) {
        size_t n = strcspn(p, "\n");
        printf("Env:         %.*s\n", (int)n, p);
        p += n;
        if (*p) ++p;
    }
}

static int list_packages(void) {
    struct dosxc_dirent *entries = malloc(sizeof(*entries) * 512);
    int count = entries ? (int)dosxc_sys_listdir(programs, entries, 512) : -1, shown = 0;
    if (count < 0) { printf("%s does not exist; no packages are installed.\n", programs); free(entries); return 0; }
    printf("%-16s %-14s %-11s %s\n", "Package", "Version", "Arch", "Description");
    for (int i = 0; i < count && i < 512; ++i) {
        struct dsp_info info;
        char folder[300];
        if (!(entries[i].attributes & DOSXC_ATTR_DIRECTORY) || installed(entries[i].name, &info, folder, sizeof(folder))) continue;
        printf("%-16s %-14s %-11s %.36s\n", info.name, info.version, arch_name(info.arch), info.description);
        ++shown;
    }
    free(entries);
    printf("%d package(s) installed.\n", shown);
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
    char inf[400], text[1024];
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

/* Move the driver from the package folder to \\SYSTEM\\DRIVERS and start it. */
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
    char question[300];
    if (present) snprintf(question, sizeof(question), "Replace %s %s with %s (Y/N)? ", old.name, old.version, info->version);
    else snprintf(question, sizeof(question), "Install %s %s (%s) to %s\\%s (Y/N)? ", info->name, info->version,
                  info->description, programs, info->name);
    if (!ask(question)) { dsp_close(&package); return 1; }
    if (present && uninstall(&old, folder)) { dsp_close(&package); return 1; }
    package_folder(info->name, folder, sizeof(folder));
    if (info->driver[0]) {                    /* another package's driver must not be replaced */
        char list[256];
        snprintf(list, sizeof(list), "%s", info->driver);
        for (char *name = strtok(list, ";"); name; name = strtok(0, ";"))
            if (driver_file_exists(name)) {
                printf("%s\\%s already exists; it belongs to another package.\n", drivers, name);
                dsp_close(&package);
                return 1;
            }
    }
    if (make_dirs(programs) || (info->driver[0] && make_dirs(drivers)) || extract(&package, folder)) {
        printf("Installation failed; removing what was copied.\n");
        remove_tree(folder);
        dsp_close(&package);
        return 1;
    }
    install_folder = folder;
    driver_failed = 0;
    each_driver(info, driver_install);
    if (driver_failed) {
        printf("Installation failed; removing what was copied.\n");
        remove_tree(folder);
        dsp_close(&package);
        return 1;
    }
    each_path(info, path_add);
    each_env(info, 1);
    printf("%s %s is installed in %s.\n", info->name, info->version, folder);
    dsp_close(&package);
    return 0;
}

static int remove_command(const char *name) {
    struct dsp_info info;
    char folder[300];
    if (installed(name, &info, folder, sizeof(folder))) { printf("Package %s is not installed.\n", name); return 1; }
    char question[300];
    if (same(info.name, "coreutils"))
        snprintf(question, sizeof(question), "coreutils provides DIR, COPY, DEL and the other basic commands.  Remove it (Y/N)? ");
    else snprintf(question, sizeof(question), "Remove %s %s and everything in %s (Y/N)? ", info.name, info.version, folder);
    if (!ask(question)) return 1;
    if (uninstall(&info, folder)) return 1;
    printf("%s has been removed.\n", info.name);
    return 0;
}

/* Collect every file under FOLDER (relative names with '/'). */
static int pack_tree(struct dsp_writer *w, const char *folder, const char *relative, unsigned *count) {
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
            if (pack_tree(w, folder, name, count)) { free(entries); return -1; }
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
        int r = dsp_add(w, name, data, (size_t)entries[i].size);
        free(data);
        if (r) { free(entries); puts("Cannot write the package (is the disk full?)."); return -1; }
        printf("  %s\n", name);
        ++*count;
    }
    free(entries);
    return 0;
}

static int pack_command(int argc, char **argv) {
    if (argc < 2) { puts("PKG PACK folder out.dsp [/N:name] [/V:version] [/A:author] [/D:description] [/B:bits] [/P:path] [/S:program]"); return 1; }
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
    struct dsp_writer *w = dsp_create(out, &info);
    if (!w) { printf("Cannot create %s.\n", out); return 1; }
    unsigned count = 0;
    int failed = pack_tree(w, folder, "", &count);
    if (dsp_finish(w) || failed) { remove(out); puts("Packing failed."); return 1; }
    printf("Packed %u file(s) into %s (%s %s).\n", count, out, info.name, info.version);
    return 0;
}

static void help(void) {
    puts("DOS-XC Package Manager\n\n"
         "  PKG LIST                        Lists installed packages\n"
         "  PKG INFO file.dsp | name        Shows package details\n"
         "  PKG FILES file.dsp | name       Lists the files in a package\n"
         "  PKG INSTALL file.dsp [/Y]       Installs to \\Programs\\<name>\n"
         "  PKG UPDATE file.dsp [/Y] [/F]   Replaces an installed package with a newer version\n"
         "  PKG REMOVE name [/Y]            Uninstalls a package\n"
         "  PKG PACK folder out.dsp [/N:name] [/V:version] [/A:author] [/D:text]\n"
         "           [/B:16|32|64] [/P:folders-for-PATH] [/S:program] [/E:NAME=value]\n"
         "           [/R:A.DRV;B.DRV]\n"
         "                                  Makes a package; /S:program runs 16-bit\n"
         "                                  programs in their own folder; /R:files\n"
         "                                  are device drivers for \\SYSTEM\\DRIVERS\n\n"
         "/Y answers yes to questions.");
}

int main(int argc, char **argv) {
    struct dosxc_sysinfo sys;
    dosxc_sys_sysinfo(&sys);
    snprintf(programs, sizeof(programs), "%c:\\Programs", sys.drive);
    snprintf(drivers, sizeof(drivers), "%c:\\SYSTEM\\DRIVERS", sys.drive);
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
    if (count != 2) { help(); return 1; }
    if (same(command, "INFO")) return info_command(args[1], 0);
    if (same(command, "FILES")) return info_command(args[1], 1);
    if (same(command, "INSTALL")) return install_command(args[1], 0, force);
    if (same(command, "UPDATE")) return install_command(args[1], 1, force);
    if (same(command, "REMOVE") || same(command, "UNINSTALL")) return remove_command(args[1]);
    printf("Unknown command %s.\n", command);
    help();
    return 1;
}
