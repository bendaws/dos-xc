/*
 * DOS-XC /coreutils/type.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "common.h"
int main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "/?")) { puts("TYPE file [file ...]   Shows the contents of text files."); return argc < 2; }
    int status = 0;
    for (int i = 1; i < argc; ++i) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) { printf("%s: %s\n", argv[i], is_dir(argv[i]) ? "is a folder" : "file not found"); status = 1; continue; }
        char buffer[8192];
        size_t n;
        while ((n = fread(buffer, 1, sizeof(buffer), f))) {
            for (size_t k = 0; k < n; ++k) if (buffer[k] == 26) { n = k; break; }
            fwrite(buffer, 1, n, stdout);
        }
        fclose(f);
    }
    return status;
}
