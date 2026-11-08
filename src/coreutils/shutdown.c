/* DOS-XC coreutils - SHUTDOWN.  Copyright (c) 2026 Ben Daws - see LICENSE.txt */
#include "common.h"
int main(int argc, char **argv) {
    int yes = 0, restart = 0, button = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "/?")) {
            puts("SHUTDOWN [/Y] [/R]   Writes back open files and switches the computer off.\n"
                 "                     /Y  does not ask first\n"
                 "                     /R  restarts instead (as RESTART)\n"
                 "The computer's power button runs SHUTDOWN too.");
            return 0;
        }
        if (same(argv[i], "/Y")) yes = 1;
        else if (same(argv[i], "/R")) restart = 1;
        else if (same(argv[i], "/B")) button = yes = 1;   /* started by the power button */
        else { printf("SHUTDOWN: unknown option %s\n", argv[i]); return 1; }
    }
    if (!yes && !ask(restart ? "Restart the computer (Y/N)? " : "Switch the computer off (Y/N)? ")) return 1;
    if (button) puts("\nThe power button was pressed.");
    puts(restart ? "Restarting..." : "Shutting down...");
    fflush(stdout);
    if (restart) dosxc_sys_reboot();
    else dosxc_sys_power(DOSXC_POWER_OFF);
    puts("The computer could not be switched off.");
    return 1;
}
