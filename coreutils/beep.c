/*
 * DOS-XC /coreutils/beep.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "common.h"
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "/?")) { puts("BEEP [frequency-hz] [duration-ms]"); return 0; }
    long hz = argc > 1 ? atol(argv[1]) : 880, ms = argc > 2 ? atol(argv[2]) : 100;
    if (hz < 20 || hz > 20000 || ms < 0 || ms > 3000) { puts("BEEP: frequency 20..20000 Hz; duration 0..3000 ms."); return 1; }
    dosxc_sys_console(DOSXC_CONSOLE_BEEP, hz, ms);
    return 0;
}
