/*
 * DOS-XC /tests/programs/arctool.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "archive.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    char error[400];
    if (argc >= 4 && !strcmp(argv[1], "x")) {
        if (arc_extract(argv[2], argv[3], 0, error, sizeof(error))) { printf("ERROR: %s\n", error); return 1; }
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "c")) {
        if (arc_create(argv[3], atoi(argv[2]), (const char *const *)argv + 4, argc - 4, 0, error, sizeof(error))) {
            printf("ERROR: %s\n", error);
            return 1;
        }
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "k")) {
        FILE *f = fopen(argv[2], "rb");
        if (!f) { printf("cannot open %s\n", argv[2]); return 1; }
        static unsigned char buffer[65536];
        unsigned long crc = 0xffffffffu, total = 0;
        size_t n;
        while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) {
            total += n;
            for (size_t i = 0; i < n; ++i) {
                crc ^= buffer[i];
                for (int k = 0; k < 8; ++k) crc = crc >> 1 ^ (0xedb88320u & (0u - (crc & 1)));
            }
        }
        fclose(f);
        printf("%08lx %lu %s\n", ~crc & 0xffffffffu, total, argv[2]);
        return 0;
    }
    puts("arctool x ARCHIVE FOLDER | c FORMAT ARCHIVE SOURCE... | k FILE (CRC-32)\n"
         "FORMAT: 1 zip, 2 7z, 4 tar, 5 tar.gz, 6 tar.xz");
    return 2;
}
