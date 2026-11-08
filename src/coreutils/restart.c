/* DOS-XC coreutils - RESTART (also REBOOT).  Copyright (c) 2026 Ben Daws - see LICENSE.txt */
#include "common.h"
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "/?")) {
        puts("RESTART [/Y]   Writes back open files and restarts the computer.\n"
             "               /Y  does not ask first.  REBOOT is the same command.");
        return 0;
    }
    if (!(argc > 1 && same(argv[1], "/Y")) && !ask("Restart the computer (Y/N)? ")) return 1;
    puts("Restarting...");
    fflush(stdout);
    dosxc_sys_reboot();
    puts("The computer could not be restarted.");
    return 1;
}
