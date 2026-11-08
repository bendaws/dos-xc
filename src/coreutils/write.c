/* DOS-XC coreutils - WRITE.  Copyright (c) 2026 Ben Daws - see LICENSE.txt */
#include "common.h"
int main(int argc, char **argv) {
    int i = 1, add = 0;
    if (i < argc && same(argv[i], "/A")) { add = 1; ++i; }
    if (i >= argc || !strcmp(argv[i], "/?")) { puts("WRITE [/A] file text   Writes a line of text to a file; /A appends."); return i >= argc; }
    FILE *f = fopen(argv[i], add ? "ab" : "wb");
    if (!f) { printf("%s: %s\n", argv[i], strerror(errno)); return 1; }
    for (int k = i + 1; k < argc; ++k) fprintf(f, k > i + 1 ? " %s" : "%s", argv[k]);
    fputs("\r\n", f);
    if (fclose(f)) { puts("Unable to write the file."); return 1; }
    return 0;
}
