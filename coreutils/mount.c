/*
 * DOS-XC /coreutils/mount.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "common.h"
int main(int argc, char **argv) {
    if (argc != 2 || !strcmp(argv[1], "/?")) {
        puts("MOUNT file\n\n"
             "Mounts a disk image as a drive: an .ISO (CD or DVD image) or an .IMG\n"
             "(a FAT disk or floppy image).  EJECT drive: unmounts it again.");
        return argc == 2 && !strcmp(argv[1], "/?") ? 0 : 1;
    }
    int r = dosxc_sys_drive(DOSXC_DRIVE_MOUNT, 0, argv[1], strlen(argv[1]) + 1);
    if (r == -DOSXC_ENOENT) { printf("MOUNT: %s was not found.\n", argv[1]); return 1; }
    if (r == -DOSXC_EINVAL) { printf("MOUNT: %s holds no ISO 9660 or FAT volume.\n", argv[1]); return 1; }
    if (r == -DOSXC_EXDEV) { printf("MOUNT: copy %s to a disk drive first.\n", argv[1]); return 1; }
    if (r < 0) { printf("MOUNT: %s could not be mounted.\n", argv[1]); return 1; }
    char text[100];
    int said = 0;
    while (dosxc_sys_drive(DOSXC_DRIVE_NOTICE, 0, text, sizeof(text)) > 0) { puts(text); said = 1; }
    if (!said) printf("%s is mounted as %c:\n", argv[1], r);
    return 0;
}
