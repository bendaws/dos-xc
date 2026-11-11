/*
 * DOS-XC /coreutils/md.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "common.h"
/* Creates every missing folder along the path, as cmd.exe does. */
static int make(char *path) {
    if (is_dir(path)) return 0;
    for (char *p = path + 1; *p; ++p) {
        if ((*p != '\\' && *p != '/') || p[-1] == ':') continue;
        char saved = *p;
        *p = 0;
        if (!is_dir(path) && mkdir(path, 0777)) { *p = saved; return -1; }
        *p = saved;
    }
    return mkdir(path, 0777);
}
int main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "/?")) { puts("MD folder [folder ...]   Creates folders, including missing parents."); return argc < 2; }
    int status = 0;
    for (int i = 1; i < argc; ++i) {
        if (exists(argv[i])) { printf("A folder or file %s already exists.\n", argv[i]); status = 1; continue; }
        if (make(argv[i])) { printf("%s: %s\n", argv[i], strerror(errno)); status = 1; }
    }
    return status;
}
