/*
 * DOS-XC /coreutils/color.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "common.h"
static int digit(char c) {
    c = (char)toupper((unsigned char)c);
    return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
int main(int argc, char **argv) {
    int attribute = 7;
    if (argc > 1) {
        const char *a = argv[1];
        if (strlen(a) != 2 || digit(a[0]) < 0 || digit(a[1]) < 0 || a[0] == a[1]) {
            puts("COLOR [bf]   b = background, f = foreground (hex digits).  Example: COLOR 1F\n"
                 "  0 Black  1 Blue  2 Green  3 Aqua  4 Red  5 Purple  6 Yellow  7 White\n"
                 "  8 Gray   9 Light Blue  A Light Green  B Light Aqua  C Light Red\n"
                 "  D Light Purple  E Light Yellow  F Bright White");
            return strcmp(a, "/?") != 0;
        }
        attribute = digit(a[0]) << 4 | digit(a[1]);
    }
    fflush(stdout);
    dosxc_sys_console(DOSXC_CONSOLE_COLOR, attribute | 0x100, 0);
    return 0;
}
