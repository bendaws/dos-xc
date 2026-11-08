/*
 * DOS-XC C library - formatted input
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#include "stdio_impl.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

struct source {
    FILE *file;
    const char *text;
    size_t consumed;
};

static int get(struct source *s) {
    int c;
    if (s->file) c = fgetc(s->file);
    else c = *s->text ? (unsigned char)*s->text++ : EOF;
    if (c != EOF) ++s->consumed;
    return c;
}

static void unget(struct source *s, int c) {
    if (c == EOF) return;
    --s->consumed;
    if (s->file) ungetc(c, s->file);
    else --s->text;
}

static void store_integer(void *p, int length, unsigned long long v) {
    switch (length) {
    case 1: *(char *)p = (char)v; break;
    case 2: *(short *)p = (short)v; break;
    case 3: *(long *)p = (long)v; break;
    case 4: case 5: *(long long *)p = (long long)v; break;
    case 6: case 7: *(size_t *)p = (size_t)v; break;
    default: *(int *)p = (int)v;
    }
}

static int scan(struct source *s, const char *f, va_list ap) {
    int assigned = 0, c;
    char buffer[512];
    for (; *f; ++f) {
        if (isspace((unsigned char)*f)) {
            while ((c = get(s)) != EOF && isspace(c)) {
            }
            unget(s, c);
            continue;
        }
        if (*f != '%' || f[1] == '%') {
            if (*f == '%') ++f;
            c = get(s);
            if (*f == '%') while (c != EOF && isspace(c)) c = get(s);
            if (c != (unsigned char)*f) {
                unget(s, c);
                return c == EOF && !assigned ? EOF : assigned;
            }
            continue;
        }
        ++f;
        int suppress = 0, width = 0, length = 0;
        if (*f == '*') {
            suppress = 1;
            ++f;
        }
        while (isdigit((unsigned char)*f)) width = width * 10 + (*f++ - '0');
        for (;; ++f) {
            if (*f == 'h') length = length == 2 ? 1 : 2;
            else if (*f == 'l') length = length == 3 ? 4 : 3;
            else if (*f == 'q' || *f == 'L') length = 4;
            else if (*f == 'j') length = 5;
            else if (*f == 'z') length = 6;
            else if (*f == 't') length = 7;
            else break;
        }
        char conversion = *f;
        if (!conversion) break;
        if (conversion == 'n') {
            if (!suppress) store_integer(va_arg(ap, void *), length, s->consumed);
            continue;
        }
        if (conversion != 'c' && conversion != '[') {
            while ((c = get(s)) != EOF && isspace(c)) {
            }
            unget(s, c);
        }
        if (conversion == 'c') {
            if (!width) width = 1;
            char *out = suppress ? 0 : va_arg(ap, char *);
            for (int i = 0; i < width; ++i) {
                if ((c = get(s)) == EOF) return assigned ? assigned : EOF;
                if (out) out[i] = (char)c;
            }
            if (out) ++assigned;
            continue;
        }
        if (conversion == 's' || conversion == '[') {
            char set[256] = {0};
            int invert = 0;
            if (conversion == '[') {
                ++f;
                if (*f == '^') {
                    invert = 1;
                    ++f;
                }
                if (*f == ']') set[(unsigned char)*f++] = 1;
                for (; *f && *f != ']'; ++f) {
                    if (f[1] == '-' && f[2] && f[2] != ']') {
                        for (int k = (unsigned char)f[0]; k <= (unsigned char)f[2]; ++k) set[k] = 1;
                        f += 2;
                    } else {
                        set[(unsigned char)*f] = 1;
                    }
                }
                if (!*f) --f;
            }
            char *out = suppress ? 0 : va_arg(ap, char *);
            int n = 0;
            if (!width) width = -1;
            while (width < 0 || n < width) {
                c = get(s);
                if (c == EOF) break;
                int accept = conversion == 's' ? !isspace(c) : (set[c] != 0) != invert;
                if (!accept) {
                    unget(s, c);
                    break;
                }
                if (out) out[n] = (char)c;
                ++n;
            }
            if (!n) return assigned ? assigned : (c == EOF ? EOF : assigned);
            if (out) {
                out[n] = 0;
                ++assigned;
            }
            continue;
        }
        /* numbers: collect the longest plausible prefix, then convert */
        int limit = width && width < (int)sizeof(buffer) - 1 ? width : (int)sizeof(buffer) - 1, n = 0;
        int is_float = strchr("eEfFgGaA", conversion) != 0;
        int base = conversion == 'o' ? 8 : (conversion == 'x' || conversion == 'X' || conversion == 'p') ? 16
                 : conversion == 'i' ? 0 : 10;
        int seen_point = 0, seen_exponent = 0, hex = base == 16, detect = base == 0;
        while (n < limit) {
            c = get(s);
            if (c == EOF) break;
            int ok = 0;
            if ((c == '+' || c == '-') && (n == 0 || (is_float && (buffer[n - 1] | 32) == (hex ? 'p' : 'e')))) ok = 1;
            else if (isdigit(c)) ok = base != 8 || c < '8';
            else if (n && buffer[n - 1] == '0' && (c | 32) == 'x' && (detect || base == 16 || is_float) &&
                     (n == 1 || (n == 2 && (buffer[0] == '-' || buffer[0] == '+')))) {
                ok = 1;
                hex = 1;
            } else if (hex && isxdigit(c) && !seen_exponent) ok = 1;
            else if (is_float && c == '.' && !seen_point && !seen_exponent) ok = seen_point = 1;
            else if (is_float && !seen_exponent && (c | 32) == (hex ? 'p' : 'e') && n) ok = seen_exponent = 1;
            else if (is_float && strchr("infatyINFATY", c)) ok = 1;
            if (!ok) {
                unget(s, c);
                break;
            }
            if (base == 0 && n == 0 && c == '0') base = 8;
            if (base == 8 && hex) base = 16;
            buffer[n++] = (char)c;
        }
        buffer[n] = 0;
        if (!n) return assigned ? assigned : (c == EOF ? EOF : assigned);
        char *end;
        if (is_float) {
            long double v = strtold(buffer, &end);
            if (end == buffer) return assigned;
            if (!suppress) {
                void *p = va_arg(ap, void *);
                if (length == 4) *(long double *)p = v;
                else if (length == 3) *(double *)p = (double)v;
                else *(float *)p = (float)v;
                ++assigned;
            }
        } else {
            unsigned long long v = (conversion == 'd' || conversion == 'i')
                                       ? (unsigned long long)strtoll(buffer, &end, base ? base : (hex ? 16 : 10))
                                       : strtoull(buffer, &end, base ? base : 10);
            if (end == buffer) return assigned;
            if (!suppress) {
                void *p = va_arg(ap, void *);
                if (conversion == 'p') *(void **)p = (void *)(uintptr_t)v;
                else store_integer(p, length, v);
                ++assigned;
            }
        }
    }
    return assigned;
}

int vfscanf(FILE *file, const char *f, va_list ap) {
    struct source s = {file, 0, 0};
    return scan(&s, f, ap);
}
int vsscanf(const char *text, const char *f, va_list ap) {
    struct source s = {0, text, 0};
    return scan(&s, f, ap);
}
int vscanf(const char *f, va_list ap) { return vfscanf(stdin, f, ap); }

int fscanf(FILE *file, const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    int r = vfscanf(file, f, ap);
    va_end(ap);
    return r;
}
int sscanf(const char *text, const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    int r = vsscanf(text, f, ap);
    va_end(ap);
    return r;
}
int scanf(const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    int r = vfscanf(stdin, f, ap);
    va_end(ap);
    return r;
}
