/* DOS-XC coreutils - MORE.  Copyright (c) 2026 Ben Daws - see LICENSE.txt */
#include "common.h"
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "/?")) { puts("MORE [file]   or   command | MORE   Shows text one screen at a time."); return 0; }
    FILE *f = argc > 1 ? fopen(argv[1], "r") : stdin;
    if (!f) { printf("%s: file not found\n", argv[1]); return 1; }
    int c, row = 0, column = 0;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\r') continue;
        putchar(c);
        if (c == '\n' || ++column == 80) {
            column = 0;
            if (++row == 24) {
                fputs("-- More --", stdout);
                fflush(stdout);
                int k = dosxc_sys_console(DOSXC_CONSOLE_GETKEY, 1, 0);
                fputs("\r          \r", stdout);
                if (k == 'q' || k == 'Q' || k == 27 || k == 3) break;
                row = k == '\n' ? 23 : 0;
            }
        }
    }
    return 0;
}
