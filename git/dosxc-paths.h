/*
 * DOS-XC /git/dosxc-paths.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef DOSXC_PATHS_H
#define DOSXC_PATHS_H
#include <stddef.h>
#include <string.h>
#define PATH_SEP ';'
int win32_has_dos_drive_prefix(const char *path);
#define has_dos_drive_prefix win32_has_dos_drive_prefix
int win32_skip_dos_drive_prefix(char **path);
#define skip_dos_drive_prefix win32_skip_dos_drive_prefix
static inline int dosxc_is_dir_sep(int c) { return c == '/' || c == '\\'; }
#define is_dir_sep dosxc_is_dir_sep
static inline char *dosxc_find_last_dir_sep(const char *path) {
    char *last = NULL;
    for (; *path; ++path)
        if (dosxc_is_dir_sep(*path)) last = (char *)path;
    return last;
}
#define find_last_dir_sep dosxc_find_last_dir_sep
static inline int dosxc_has_dir_sep(const char *path) { return !!path[strcspn(path, "/\\")]; }
#define has_dir_sep(path) dosxc_has_dir_sep(path)
int win32_offset_1st_component(const char *path);
#define offset_1st_component win32_offset_1st_component
#endif
