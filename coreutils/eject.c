/*
 * DOS-XC /coreutils/eject.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "common.h"
int main(int argc, char **argv) {
    if (argc != 2 || !strcmp(argv[1], "/?") || !isalpha((unsigned char)argv[1][0]) ||
        (argv[1][1] && (argv[1][1] != ':' || argv[1][2]))) {
        puts("EJECT drive:\n\n"
             "Writes back everything waiting for the drive, unmounts it (and every other\n"
             "drive on the same disk) and tells a USB disk it is about to be removed.\n"
             "Drives come back when the disk is plugged in again.");
        return argc == 2 && !strcmp(argv[1], "/?") ? 0 : 1;
    }
    int letter = toupper((unsigned char)argv[1][0]);
    int r = dosxc_sys_drive(DOSXC_DRIVE_EJECT, letter, 0, 0);
    if (r == -DOSXC_EBUSY) { printf("EJECT: %c: is the system drive.\n", letter); return 1; }
    if (r < 0) { printf("EJECT: there is no drive %c:.\n", letter); return 1; }
    char text[100];
    while (dosxc_sys_drive(DOSXC_DRIVE_NOTICE, 0, text, sizeof(text)) > 0) puts(text);
    return 0;
}
