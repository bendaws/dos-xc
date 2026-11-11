/*
 * DOS-XC /coreutils/dir.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "common.h"
#include <time.h>

static int bare, wide, paging, recursive, attr, order, reverse, lines;
static unsigned long long files, dirs, bytes;

static void newline(void) {
    putchar('\n');
    if (paging && ++lines == 23) {
        fputs("Press any key to continue . . .", stdout);
        fflush(stdout);
        dosxc_sys_console(DOSXC_CONSOLE_GETKEY, 1, 0);
        putchar('\n');
        lines = 0;
    }
}

static int compare(const void *x, const void *y) {
    const struct dosxc_dirent *a = x, *b = y;
    int r;
    if (order == 1 && a->size != b->size) r = a->size > b->size ? 1 : -1;
    else r = strcasecmp(a->name, b->name);
    return reverse ? -r : r;
}

static void sized(unsigned long long value, int width) {
    char digits[40], out[48];
    int n = snprintf(digits, sizeof(digits), "%llu", value), o = 0;
    for (int i = 0; i < n; ++i) {
        if (i && (n - i) % 3 == 0) out[o++] = ',';
        out[o++] = digits[i];
    }
    out[o] = 0;
    printf("%*s", width, out);
}

static int show(const char *folder, const char *pattern) {
    struct dosxc_dirent *entries;
    int count = list(folder, &entries);
    if (count < 0) { puts("File Not Found"); return 1; }
    qsort(entries, (size_t)count, sizeof(*entries), compare);
    char full[300];
    dosxc_sys_fullpath(folder, full, sizeof(full));
    if (!bare) { printf(" Directory of %s", full); newline(); newline(); }
    int column = 0;
    for (int i = 0; i < count; ++i) {
        struct dosxc_dirent *f = &entries[i];
        int directory = (f->attributes & DOSXC_ATTR_DIRECTORY) != 0;
        if (!wildcard(pattern, f->name) || (attr == 1 && !directory) || (attr == 2 && directory)) continue;
        if (directory) ++dirs; else { ++files; bytes += f->size; }
        if (bare) {
            if (recursive) printf("%s%s", full, strlen(full) > 3 ? "\\" : "");
            fputs(f->name, stdout);
            newline();
            continue;
        }
        if (wide) {
            char name[300];
            snprintf(name, sizeof(name), directory ? "[%s]" : "%s", f->name);
            printf("%-19s", name);
            if (++column == 4) { newline(); column = 0; }
            continue;
        }
        time_t t = (time_t)f->mtime;
        struct tm *tm = gmtime(&t);
        if (f->mtime && tm) printf("%02d/%02d/%04d  %02d:%02d", tm->tm_mon + 1, tm->tm_mday, tm->tm_year + 1900, tm->tm_hour, tm->tm_min);
        else fputs("--/--/----  --:--", stdout);
        fputs("  ", stdout);
        if (directory) fputs("       <DIR>", stdout); else sized(f->size, 12);
        printf("  %c%c%c%c  %s", f->attributes & 1 ? 'R' : '-', f->attributes & 2 ? 'H' : '-',
               f->attributes & 4 ? 'S' : '-', f->attributes & 32 ? 'A' : '-', f->name);
        newline();
    }
    if (wide && column) newline();
    if (recursive) {
        for (int i = 0; i < count; ++i) {
            if (!(entries[i].attributes & DOSXC_ATTR_DIRECTORY)) continue;
            char sub[600];
            join(sub, sizeof(sub), folder, entries[i].name);
            if (!bare) newline();
            show(sub, pattern);
        }
    }
    free(entries);
    return 0;
}

int main(int argc, char **argv) {
    const char *target = 0;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (a[0] != '/') { target = a; continue; }
        if (!strcmp(a, "/?")) {
            puts("DIR [path][pattern] [/B] [/W] [/P] [/S] [/A[:D|-D]] [/O:N|S|-N|-S]\n"
                 "  /B bare names  /W wide  /P pause  /S subfolders  /A:D folders only\n"
                 "  /A:-D files only  /O sort by name (N) or size (S), - reverses");
            return 0;
        }
        if (same(a, "/B")) bare = 1;
        else if (same(a, "/W")) wide = 1;
        else if (same(a, "/P")) paging = 1;
        else if (same(a, "/S")) recursive = 1;
        else if (same(a, "/A") || same(a, "/A:A")) attr = 0;
        else if (same(a, "/A:D") || same(a, "/AD")) attr = 1;
        else if (same(a, "/A:-D") || same(a, "/A-D")) attr = 2;
        else if (starts(a, "/O")) {
            const char *o = a + 2;
            if (*o == ':') ++o;
            if (*o == '-') { reverse = 1; ++o; }
            if (same(o, "S")) order = 1;
            else if (*o && !same(o, "N")) { puts("Unsupported DIR sort order."); return 1; }
        } else { printf("Invalid switch - %s\n", a); return 1; }
    }
    char folder[300] = ".";
    const char *pattern = "*";
    if (target) {
        if (is_dir(target) || (strlen(target) == 2 && target[1] == ':')) {
            snprintf(folder, sizeof(folder), "%s%s", target, strlen(target) == 2 && target[1] == ':' ? "\\" : "");
        } else split_path(target, folder, sizeof(folder), &pattern);
        if (!*pattern) pattern = "*";
    }
    if (show(folder, pattern)) return 1;
    if (!bare) {
        sized(files, 8); fputs(" File(s) ", stdout); sized(bytes, 14); fputs(" bytes", stdout); newline();
        struct dosxc_volume volumes[26];
        char full[300];
        dosxc_sys_fullpath(folder, full, sizeof(full));
        unsigned long long free_bytes = 0;
        int count = dosxc_sys_volumes(volumes, 26);
        for (int i = 0; i < count && i < 26; ++i)
            if (volumes[i].letter == toupper((unsigned char)full[0])) free_bytes = volumes[i].free_bytes;
        sized(dirs, 8); fputs(" Dir(s)  ", stdout); sized(free_bytes, 14); fputs(" bytes free", stdout); newline();
    }
    return files || dirs ? 0 : (puts("File Not Found"), 1);
}
