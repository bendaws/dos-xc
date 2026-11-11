/*
 * DOS-XC /coreutils/cls.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "common.h"
int main(int argc, char **argv) {
    if (argc > 1) { puts("CLS   Clears the screen."); return !!strcmp(argv[1], "/?"); }
    fflush(stdout);
    dosxc_sys_console(DOSXC_CONSOLE_CLEAR, 0, 0);
    return 0;
}
