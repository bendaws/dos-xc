/*
 * DOS-XC /coreutils/del.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "common.h"
int main(int argc, char **argv) {
    int prompt = 0, quiet = 0;
    const char *target = 0;
    for (int i = 1; i < argc; ++i) {
        if (same(argv[i], "/P")) prompt = 1;
        else if (same(argv[i], "/Q")) quiet = 1;
        else if (argv[i][0] == '/') { puts("DEL [/P] [/Q] file-or-pattern   /P asks for each file; /Q never asks."); return strcmp(argv[i], "/?") != 0; }
        else target = argv[i];
    }
    if (!target) { puts("DEL [/P] [/Q] file-or-pattern"); return 1; }
    char folder[300];
    const char *pattern;
    if (is_dir(target)) { snprintf(folder, sizeof(folder), "%s", target); pattern = "*"; }
    else split_path(target, folder, sizeof(folder), &pattern);
    if (!quiet && !prompt && (!strcmp(pattern, "*") || !strcmp(pattern, "*.*"))) {
        char full[300], question[400];
        dosxc_sys_fullpath(folder, full, sizeof(full));
        snprintf(question, sizeof(question), "All files in %s will be deleted!  Are you sure (Y/N)? ", full);
        if (!ask(question)) return 1;
    }
    struct dosxc_dirent *entries;
    int count = list(folder, &entries), deleted = 0, failed = 0;
    for (int i = 0; i < count; ++i) {
        if ((entries[i].attributes & DOSXC_ATTR_DIRECTORY) || !wildcard(pattern, entries[i].name)) continue;
        char path[600];
        join(path, sizeof(path), folder, entries[i].name);
        if (prompt) {
            char question[700];
            snprintf(question, sizeof(question), "%s, Delete (Y/N)? ", path);
            if (!ask(question)) continue;
        }
        if (unlink(path)) { printf("%s: %s\n", path, strerror(errno)); failed = 1; }
        else ++deleted;
    }
    if (count > 0) free(entries);
    if (!deleted && !failed) { puts("Could Not Find the file."); return 1; }
    return failed;
}
