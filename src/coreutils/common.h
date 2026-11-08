/*
 * DOS-XC coreutils - shared helpers
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#pragma once
#include <ctype.h>
#include <dosxc/syscall.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static inline int same(const char *a, const char *b) { return !strcasecmp(a, b); }
static inline int starts(const char *a, const char *b) { return !strncasecmp(a, b, strlen(b)); }

/* DOS wildcard match: * and ?, case-insensitive; "*.*" matches everything. */
static inline int wildcard(const char *pattern, const char *text) {
    if (!strcmp(pattern, "*.*")) pattern = "*";
    while (*pattern) {
        if (*pattern == '*') {
            do if (wildcard(pattern + 1, text)) return 1; while (*text++);
            return 0;
        }
        if (!*text || (*pattern != '?' && tolower((unsigned char)*pattern) != tolower((unsigned char)*text))) return 0;
        ++pattern;
        ++text;
    }
    return !*text;
}

static inline int has_wildcards(const char *s) { return strchr(s, '*') || strchr(s, '?'); }

static inline int is_dir(const char *path) {
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}

static inline int exists(const char *path) {
    struct stat st;
    return !stat(path, &st);
}

/* Split "DIR\PATTERN" into its folder ("." when none) and last part. */
static inline void split_path(const char *path, char *folder, size_t size, const char **last) {
    const char *cut = 0;
    for (const char *p = path; *p; ++p)
        if (*p == '\\' || *p == '/' || (*p == ':' && p == path + 1)) cut = p;
    if (!cut) {
        snprintf(folder, size, ".");
        *last = path;
        return;
    }
    size_t n = (size_t)(cut - path) + 1;
    if (*cut != ':' && n > 1 && !(n == 3 && path[1] == ':')) --n;     /* keep "\" and "C:\" */
    if (n >= size) n = size - 1;
    memcpy(folder, path, n);
    folder[n] = 0;
    *last = cut + 1;
}

static inline void join(char *out, size_t size, const char *folder, const char *name) {
    size_t n = strlen(folder);
    if (!strcmp(folder, ".")) snprintf(out, size, "%s", name);
    else snprintf(out, size, "%s%s%s", folder, n && (folder[n - 1] == '\\' || folder[n - 1] == '/' || folder[n - 1] == ':') ? "" : "\\", name);
}

static inline int ask(const char *question) {
    fputs(question, stdout);
    fflush(stdout);
    for (;;) {
        int k = dosxc_sys_console(DOSXC_CONSOLE_GETKEY, 1, 0);
        if (k == 'y' || k == 'Y') { puts("Y"); return 1; }
        if (k == 'n' || k == 'N' || k == 27) { puts("N"); return 0; }
    }
}

/* List FOLDER (sorted by the caller if needed); returns the entry count. */
static inline int list(const char *folder, struct dosxc_dirent **entries) {
    int capacity = 256;
    for (;;) {
        *entries = malloc(sizeof(**entries) * (size_t)capacity);
        if (!*entries) return -1;
        int n = (int)dosxc_sys_listdir(folder, *entries, (size_t)capacity);
        if (n < 0) { free(*entries); *entries = 0; return -1; }
        if (n <= capacity) return n;
        free(*entries);
        capacity = n + 16;
    }
}
