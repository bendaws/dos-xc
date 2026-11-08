/* DOS-XC coreutils - COPY and MOVE (one source, built twice).
   Copyright (c) 2026 Ben Daws - see LICENSE.txt */
#include "common.h"
#include <fcntl.h>

static int overwrite;

static int copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (!in) { printf("%s: %s\n", from, strerror(errno)); return 1; }
    FILE *out = fopen(to, "wb");
    if (!out) { printf("%s: %s\n", to, strerror(errno)); fclose(in); return 1; }
    static char buffer[65536];
    size_t n;
    int failed = 0;
    while ((n = fread(buffer, 1, sizeof(buffer), in)))
        if (fwrite(buffer, 1, n, out) != n) { failed = 1; break; }
    fclose(in);
    if (fclose(out) || failed) { printf("%s: write failed (disk full?)\n", to); remove(to); return 1; }
    return 0;
}

static int one(const char *from, const char *to_spec, int move) {
    char to[600];
    const char *name;
    char folder[300];
    split_path(from, folder, sizeof(folder), &name);
    if (is_dir(to_spec) || (strlen(to_spec) == 2 && to_spec[1] == ':')) join(to, sizeof(to), to_spec, name);
    else snprintf(to, sizeof(to), "%s", to_spec);
    char a[300], b[300];
    if (dosxc_sys_fullpath(from, a, sizeof(a)) >= 0 && dosxc_sys_fullpath(to, b, sizeof(b)) >= 0 && same(a, b)) {
        puts("The file cannot be copied onto itself.");
        return 1;
    }
    if (exists(to)) {
        if (is_dir(to)) { printf("%s: a folder with that name exists.\n", to); return 1; }
        if (!overwrite) {
            char question[700];
            snprintf(question, sizeof(question), "Overwrite %s (Yes/No/All)? ", to);
            fputs(question, stdout);
            fflush(stdout);
            for (;;) {
                int k = dosxc_sys_console(DOSXC_CONSOLE_GETKEY, 1, 0);
                if (k == 'y' || k == 'Y') { puts("Yes"); break; }
                if (k == 'a' || k == 'A') { puts("All"); overwrite = 1; break; }
                if (k == 'n' || k == 'N' || k == 27) { puts("No"); return 0; }
            }
        }
        if (move && remove(to)) { printf("%s: cannot replace it.\n", to); return 1; }
    }
    if (move) {
        if (!rename(from, to)) return 0;
        if (errno != EXDEV) { printf("%s: %s\n", from, strerror(errno)); return 1; }
        if (copy_file(from, to)) return 1;                     /* another drive: copy, then delete */
        if (remove(from)) { printf("%s: copied, but the original could not be deleted.\n", from); return 1; }
        return 0;
    }
    if (is_dir(from)) { printf("%s: COPY copies files, not folders.\n", from); return 1; }
    return copy_file(from, to);
}

int main(int argc, char **argv) {
    int move = strstr(argv[0], "MOVE") || strstr(argv[0], "move");
    int i = 1;
    while (i < argc && argv[i][0] == '/') {
        if (same(argv[i], "/Y")) overwrite = 1;
        else if (same(argv[i], "/-Y")) overwrite = 0;
        else break;
        ++i;
    }
    if (argc - i != 2) {
        puts(move ? "MOVE [/Y] source destination   Moves files (wildcards allowed) or renames a folder."
                  : "COPY [/Y] source destination   Copies files; the source may use * and ?.");
        return argc - i != 2 && !(i < argc && !strcmp(argv[i], "/?"));
    }
    const char *source = argv[i], *destination = argv[i + 1];
    if (!has_wildcards(source)) {
        if (!exists(source)) { printf("%s: file not found\n", source); return 1; }
        int r = one(source, destination, move);
        if (!r && !move) puts("        1 file(s) copied.");
        return r;
    }
    if (!is_dir(destination) && !(strlen(destination) == 2 && destination[1] == ':')) {
        puts("With wildcards the destination must be a folder.");
        return 1;
    }
    char folder[300];
    const char *pattern;
    split_path(source, folder, sizeof(folder), &pattern);
    struct dosxc_dirent *entries;
    int count = list(folder, &entries), done = 0, failed = 0;
    for (int k = 0; k < count; ++k) {
        if ((entries[k].attributes & DOSXC_ATTR_DIRECTORY) || !wildcard(pattern, entries[k].name)) continue;
        char from[600];
        join(from, sizeof(from), folder, entries[k].name);
        if (one(from, destination, move)) failed = 1;
        else { ++done; if (!move) puts(from); }
    }
    if (count > 0) free(entries);
    printf("%9d file(s) %s.\n", done, move ? "moved" : "copied");
    return failed || !done;
}
