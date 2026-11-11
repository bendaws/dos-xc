/*
 * DOS-XC /coreutils/rd.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "common.h"
static int remove_tree(const char *path) {
    struct dosxc_dirent *entries;
    int count = list(path, &entries), failed = 0;
    for (int i = 0; i < count; ++i) {
        char child[600];
        join(child, sizeof(child), path, entries[i].name);
        if (entries[i].attributes & DOSXC_ATTR_DIRECTORY) failed |= remove_tree(child);
        else if (unlink(child)) failed = 1;
    }
    if (count > 0) free(entries);
    return rmdir(path) ? 1 : failed;
}
int main(int argc, char **argv) {
    int tree = 0, quiet = 0, status = 0, any = 0;
    for (int i = 1; i < argc; ++i) {
        if (same(argv[i], "/S")) { tree = 1; continue; }
        if (same(argv[i], "/Q")) { quiet = 1; continue; }
        if (argv[i][0] == '/') { puts("RD [/S] [/Q] folder   Removes an empty folder; /S removes everything in it too."); return strcmp(argv[i], "/?") != 0; }
        any = 1;
        if (!is_dir(argv[i])) { printf("%s: folder not found\n", argv[i]); status = 1; continue; }
        if (tree) {
            char question[400];
            snprintf(question, sizeof(question), "%s, Are you sure (Y/N)? ", argv[i]);
            if (!quiet && !ask(question)) continue;
            if (remove_tree(argv[i])) { printf("%s: some files could not be removed.\n", argv[i]); status = 1; }
        } else if (rmdir(argv[i])) {
            printf("%s: %s\n", argv[i], errno == ENOTEMPTY ? "the folder is not empty" : strerror(errno));
            status = 1;
        }
    }
    if (!any) { puts("RD [/S] [/Q] folder"); return 1; }
    return status;
}
