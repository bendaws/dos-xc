/* DOS-XC coreutils - SPEED.  Copyright (c) 2026 Ben Daws - see LICENSE.txt */
#include "common.h"
int main(int argc, char **argv) {
    if (argc < 2) { printf("16-bit CPU duty: %d%%\n", dosxc_sys_shell(DOSXC_SHELL_SPEED, 0, 0)); return 0; }
    long v = atol(argv[1]);
    if (v < 1 || v > 100) { puts("SPEED [1..100]   CPU duty percent for 16-bit programs; 100 runs them unthrottled."); return strcmp(argv[1], "/?") != 0; }
    dosxc_sys_shell(DOSXC_SHELL_SPEED, (void *)v, 0);
    return 0;
}
