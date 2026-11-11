/*
 * DOS-XC /coreutils/ver.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "common.h"
int main(void) {
    struct dosxc_sysinfo info;
    dosxc_sys_sysinfo(&info);
    printf("%s (%d-bit, %s boot%s)\nSystem folder: %c:\\SYSTEM\n", info.version, (int)info.bits,
           info.firmware == 'U' ? "UEFI" : "BIOS", info.live ? ", live media" : "", info.drive);
    return 0;
}
