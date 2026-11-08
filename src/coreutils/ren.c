/* DOS-XC coreutils - REN / RENAME.  Copyright (c) 2026 Ben Daws - see LICENSE.txt */
#include "common.h"
int main(int argc, char **argv) {
    if (argc != 3) { puts("REN source new-name   Renames a file or folder (the new name has no path)."); return argc != 2 || strcmp(argv[1], "/?"); }
    if (strchr(argv[2], '\\') || strchr(argv[2], '/') || strchr(argv[2], ':')) { puts("The new name cannot contain a path."); return 1; }
    char folder[300], target[600];
    const char *name;
    split_path(argv[1], folder, sizeof(folder), &name);
    join(target, sizeof(target), folder, argv[2]);
    if (!exists(argv[1])) { printf("%s: file not found\n", argv[1]); return 1; }
    if (exists(target) && strcasecmp(name, argv[2])) { puts("A file with that name already exists."); return 1; }
    if (rename(argv[1], target)) { printf("%s: %s\n", argv[1], strerror(errno)); return 1; }
    return 0;
}
