/*
 * DOS-XC /os/libc/src/glob.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <glob.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int fold(int c, int flags) { return (flags & FNM_CASEFOLD) ? tolower(c) : c; }
static int separator(char c) { return c == '/' || c == '\\'; }

/* A bracket expression at P (after '['): 1 or 0 for C, -1 if it is not one. */
static int bracket(const char **p, int c, int flags) {
    const char *s = *p;
    int negate = *s == '!' || *s == '^', found = 0;
    if (negate) ++s;
    const char *first = s;
    while (*s && (*s != ']' || s == first)) {
        int low = (unsigned char)*s;
        if (low == '\\' && !(flags & FNM_NOESCAPE) && s[1]) low = (unsigned char)*++s;
        int high = low;
        if (s[1] == '-' && s[2] && s[2] != ']') {
            s += 2;
            high = (unsigned char)*s;
            if (high == '\\' && !(flags & FNM_NOESCAPE) && s[1]) high = (unsigned char)*++s;
        }
        if (fold(c, flags) >= fold(low, flags) && fold(c, flags) <= fold(high, flags)) found = 1;
        ++s;
    }
    if (*s != ']') return -1;
    *p = s + 1;
    return found != negate;
}

static int match(const char *p, const char *s, int flags, const char *start) {
    for (;;) {
        char c = *p++;
        if (!c) return *s && !((flags & FNM_LEADING_DIR) && separator(*s)) ? FNM_NOMATCH : 0;
        if ((flags & FNM_PERIOD) && *s == '.' && (c == '*' || c == '?' || c == '[') &&
            (s == start || ((flags & FNM_PATHNAME) && separator(s[-1]))))
            return FNM_NOMATCH;
        if (c == '?') {
            if (!*s || ((flags & FNM_PATHNAME) && separator(*s))) return FNM_NOMATCH;
            ++s;
        } else if (c == '*') {
            while (*p == '*') ++p;
            if (!*p) return (flags & FNM_PATHNAME) && !(flags & FNM_LEADING_DIR) && strpbrk(s, "/\\") ? FNM_NOMATCH : 0;
            for (; *s; ++s) {
                if (!match(p, s, flags & ~FNM_PERIOD, start)) return 0;
                if ((flags & FNM_PATHNAME) && separator(*s)) return FNM_NOMATCH;
            }
            return match(p, s, flags & ~FNM_PERIOD, start);
        } else if (c == '[') {
            if (!*s || ((flags & FNM_PATHNAME) && separator(*s))) return FNM_NOMATCH;
            const char *after = p;
            int r = bracket(&after, (unsigned char)*s, flags);
            if (r < 0) {
                if (*s != '[') return FNM_NOMATCH;
            } else if (!r) {
                return FNM_NOMATCH;
            } else {
                p = after;
            }
            ++s;
        } else {
            if (c == '\\' && !(flags & FNM_NOESCAPE) && *p) c = *p++;
            if ((flags & FNM_PATHNAME) && separator(c) && separator(*s)) { ++s; continue; }
            if (fold((unsigned char)c, flags) != fold((unsigned char)*s, flags)) return FNM_NOMATCH;
            ++s;
        }
    }
}

int fnmatch(const char *pattern, const char *string, int flags) { return match(pattern, string, flags, string); }

static int magic(const char *s, int flags) {
    for (; *s; ++s) {
        if (*s == '*' || *s == '?' || *s == '[') return 1;
        if (*s == '\\' && !(flags & GLOB_NOESCAPE) && s[1]) ++s;
    }
    return 0;
}

static int add(glob_t *g, const char *path, int mark) {
    size_t n = g->gl_offs + g->gl_pathc;
    if ((int)n + 2 > g->__capacity) {
        int capacity = g->__capacity ? g->__capacity * 2 : 32;
        while (capacity < (int)n + 2) capacity *= 2;
        char **bigger = realloc(g->gl_pathv, sizeof(char *) * (size_t)capacity);
        if (!bigger) return GLOB_NOSPACE;
        for (int i = g->__capacity; i < capacity; ++i) bigger[i] = 0;
        g->gl_pathv = bigger;
        g->__capacity = capacity;
    }
    size_t length = strlen(path);
    char *copy = malloc(length + 2);
    if (!copy) return GLOB_NOSPACE;
    memcpy(copy, path, length + 1);
    if (mark) { copy[length] = '/'; copy[length + 1] = 0; }
    g->gl_pathv[n] = copy;
    g->gl_pathv[n + 1] = 0;
    ++g->gl_pathc;
    return 0;
}

static void unescape(char *s) {
    char *out = s;
    for (; *s; ++s) {
        if (*s == '\\' && s[1]) ++s;
        *out++ = *s;
    }
    *out = 0;
}

/* Expand PATTERN's components from REST on, below the directory PREFIX. */
static int expand(const char *prefix, const char *rest, int flags, int (*error)(const char *, int), glob_t *g) {
    while (separator(*rest)) ++rest;
    const char *end = rest;
    while (*end && !separator(*end)) ++end;
    size_t length = (size_t)(end - rest), prefix_length = strlen(prefix);
    char *component = malloc(length + 1), *path = malloc(prefix_length + length + 512);
    if (!component || !path) { free(component); free(path); return GLOB_NOSPACE; }
    memcpy(component, rest, length);
    component[length] = 0;
    int r = 0;
    if (!magic(component, flags)) {
        if (!(flags & GLOB_NOESCAPE)) unescape(component);
        strcpy(path, prefix);
        if (prefix_length && !separator(prefix[prefix_length - 1])) strcat(path, "/");
        strcat(path, component);
        struct stat s;
        if (!stat(path, &s)) {
            if (*end) r = S_ISDIR(s.st_mode) ? expand(path, end, flags, error, g) : 0;
            else if (!(flags & GLOB_ONLYDIR) || S_ISDIR(s.st_mode)) r = add(g, path, (flags & GLOB_MARK) && S_ISDIR(s.st_mode));
        }
    } else {
        DIR *d = opendir(prefix_length ? prefix : ".");
        if (!d) {
            if ((error && error(prefix_length ? prefix : ".", errno)) || (flags & GLOB_ERR)) r = GLOB_ABORTED;
        } else {
            int match_flags = FNM_PERIOD * !(flags & GLOB_PERIOD) | (flags & GLOB_NOESCAPE ? FNM_NOESCAPE : 0) | FNM_CASEFOLD;
            for (struct dirent *e; !r && (e = readdir(d));) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                if (fnmatch(component, e->d_name, match_flags)) continue;
                if (prefix_length + strlen(e->d_name) + 2 > prefix_length + length + 512) continue;
                strcpy(path, prefix);
                if (prefix_length && !separator(prefix[prefix_length - 1])) strcat(path, "/");
                strcat(path, e->d_name);
                int directory = e->d_type == DT_DIR;
                if (*end) {
                    if (directory) r = expand(path, end, flags, error, g);
                } else if (!(flags & GLOB_ONLYDIR) || directory) {
                    r = add(g, path, (flags & GLOB_MARK) && directory);
                }
            }
            closedir(d);
        }
    }
    free(component);
    free(path);
    return r;
}

static int compare(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int glob(const char *pattern, int flags, int (*error)(const char *, int), glob_t *g) {
    if (!(flags & GLOB_APPEND)) {
        g->gl_pathc = 0;
        g->gl_pathv = 0;
        g->__capacity = 0;
        if (!(flags & GLOB_DOOFFS)) g->gl_offs = 0;
    }
    size_t before = g->gl_pathc;
    char prefix[4] = "";
    const char *rest = pattern;
    if (pattern[0] && pattern[1] == ':') {
        prefix[0] = pattern[0];
        prefix[1] = ':';
        rest += 2;
    }
    if (separator(*rest)) strcat(prefix, "/");
    int r = expand(prefix, rest, flags, error, g);
    if (r) return r;
    if (g->gl_pathc == before) {
        if (!(flags & GLOB_NOCHECK)) return GLOB_NOMATCH;
        if ((r = add(g, pattern, 0))) return r;
    }
    if (!(flags & GLOB_NOSORT))
        qsort(g->gl_pathv + g->gl_offs + before, g->gl_pathc - before, sizeof(char *), compare);
    return 0;
}

void globfree(glob_t *g) {
    for (size_t i = 0; i < g->gl_pathc; ++i) free(g->gl_pathv[g->gl_offs + i]);
    free(g->gl_pathv);
    g->gl_pathv = 0;
    g->gl_pathc = 0;
    g->__capacity = 0;
}
