/*
 * DOS-XC C library - wide characters (UTF-8 multibyte encoding)
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#include "libc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

static mbstate_t internal_state;

size_t mbrtowc(wchar_t *out, const char *s, size_t n, mbstate_t *state) {
    if (!state) state = &internal_state;
    if (!s) {
        state->__count = state->__value = 0;
        return 0;
    }
    if (!n) return (size_t)-2;
    unsigned value = state->__value, need = state->__count;
    size_t used = 0;
    if (!need) {
        unsigned char c = (unsigned char)s[0];
        used = 1;
        if (c < 0x80) {
            if (out) *out = (wchar_t)c;
            return c ? 1 : 0;
        }
        if (c >= 0xc2 && c <= 0xdf) { need = 1; value = c & 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { need = 2; value = c & 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { need = 3; value = c & 0x07; }
        else { errno = EILSEQ; return (size_t)-1; }
    }
    while (need && used < n) {
        unsigned char c = (unsigned char)s[used++];
        if ((c & 0xc0) != 0x80) {
            state->__count = state->__value = 0;
            errno = EILSEQ;
            return (size_t)-1;
        }
        value = (value << 6) | (c & 0x3f);
        --need;
    }
    if (need) {
        state->__count = need;
        state->__value = value;
        return (size_t)-2;
    }
    state->__count = state->__value = 0;
    if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    if (out) *out = (wchar_t)value;
    return value ? used : 0;
}

size_t wcrtomb(char *s, wchar_t wc, mbstate_t *state) {
    (void)state;
    char local[4];
    if (!s) s = local, wc = 0;
    unsigned v = (unsigned)wc;
    if (v < 0x80) { s[0] = (char)v; return 1; }
    if (v < 0x800) { s[0] = (char)(0xc0 | (v >> 6)); s[1] = (char)(0x80 | (v & 0x3f)); return 2; }
    if (v >= 0xd800 && v <= 0xdfff) { errno = EILSEQ; return (size_t)-1; }
    if (v < 0x10000) {
        s[0] = (char)(0xe0 | (v >> 12));
        s[1] = (char)(0x80 | ((v >> 6) & 0x3f));
        s[2] = (char)(0x80 | (v & 0x3f));
        return 3;
    }
    if (v < 0x110000) {
        s[0] = (char)(0xf0 | (v >> 18));
        s[1] = (char)(0x80 | ((v >> 12) & 0x3f));
        s[2] = (char)(0x80 | ((v >> 6) & 0x3f));
        s[3] = (char)(0x80 | (v & 0x3f));
        return 4;
    }
    errno = EILSEQ;
    return (size_t)-1;
}

int mbsinit(const mbstate_t *state) { return !state || !state->__count; }
size_t mbrlen(const char *s, size_t n, mbstate_t *state) {
    static mbstate_t own;
    return mbrtowc(0, s, n, state ? state : &own);
}
wint_t btowc(int c) { return c >= 0 && c < 128 ? (wint_t)c : WEOF; }
int wctob(wint_t c) { return c < 128 ? (int)c : EOF; }

int mblen(const char *s, size_t n) {
    static mbstate_t state;
    if (!s) return 0;
    size_t r = mbrtowc(0, s, n, &state);
    if (r == (size_t)-2) { memset(&state, 0, sizeof(state)); errno = EILSEQ; return -1; }
    return (int)r;
}
int mbtowc(wchar_t *out, const char *s, size_t n) {
    mbstate_t state = {0};
    if (!s) return 0;
    size_t r = mbrtowc(out, s, n, &state);
    return r == (size_t)-2 ? (errno = EILSEQ, -1) : (int)r;
}
int wctomb(char *s, wchar_t wc) { return s ? (int)wcrtomb(s, wc, 0) : 0; }

size_t mbsnrtowcs(wchar_t *out, const char **source, size_t limit, size_t n, mbstate_t *state) {
    const char *s = *source;
    size_t count = 0;
    if (!state) state = &internal_state;
    while (!out || count < n) {
        wchar_t wc;
        size_t r = mbrtowc(&wc, s, limit, state);
        if (r == (size_t)-1) { *source = s; return r; }
        if (r == (size_t)-2) break;
        if (out) out[count] = wc;
        if (!r) { if (out) *source = 0; return count; }
        s += r;
        limit -= r;
        ++count;
    }
    if (out) *source = s;
    return count;
}
size_t mbsrtowcs(wchar_t *out, const char **source, size_t n, mbstate_t *state) {
    return mbsnrtowcs(out, source, (size_t)-1, n, state);
}
size_t mbstowcs(wchar_t *out, const char *s, size_t n) {
    mbstate_t state = {0};
    return mbsrtowcs(out, &s, n, &state);
}

size_t wcsnrtombs(char *out, const wchar_t **source, size_t limit, size_t n, mbstate_t *state) {
    (void)state;
    const wchar_t *w = *source;
    size_t used = 0;
    char buffer[4];
    for (; limit; --limit, ++w) {
        size_t k = wcrtomb(buffer, *w, 0);
        if (k == (size_t)-1) { if (out) *source = w; return k; }
        if (out && used + k > n) break;
        if (out) memcpy(out + used, buffer, k);
        if (!*w) { if (out) *source = 0; return used; }
        used += k;
    }
    if (out) *source = w;
    return used;
}
size_t wcsrtombs(char *out, const wchar_t **source, size_t n, mbstate_t *state) {
    return wcsnrtombs(out, source, (size_t)-1, n, state);
}
size_t wcstombs(char *out, const wchar_t *w, size_t n) { return wcsrtombs(out, &w, n, 0); }

size_t wcslen(const wchar_t *s) { size_t n = 0; while (s[n]) ++n; return n; }
size_t wcsnlen(const wchar_t *s, size_t max) { size_t n = 0; while (n < max && s[n]) ++n; return n; }
int wcscmp(const wchar_t *a, const wchar_t *b) {
    while (*a && *a == *b) ++a, ++b;
    return *a < *b ? -1 : *a > *b;
}
int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n) {
    for (; n; --n, ++a, ++b) {
        if (*a != *b) return *a < *b ? -1 : 1;
        if (!*a) return 0;
    }
    return 0;
}
int wcscoll(const wchar_t *a, const wchar_t *b) { return wcscmp(a, b); }
size_t wcsxfrm(wchar_t *d, const wchar_t *s, size_t n) {
    size_t length = wcslen(s);
    if (length < n) wmemcpy(d, s, length + 1);
    return length;
}
wchar_t *wcscpy(wchar_t *d, const wchar_t *s) { wchar_t *r = d; while ((*d++ = *s++)) {} return r; }
wchar_t *wcsncpy(wchar_t *d, const wchar_t *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; ++i) d[i] = s[i];
    for (; i < n; ++i) d[i] = 0;
    return d;
}
wchar_t *wcscat(wchar_t *d, const wchar_t *s) { wcscpy(d + wcslen(d), s); return d; }
wchar_t *wcsncat(wchar_t *d, const wchar_t *s, size_t n) {
    wchar_t *e = d + wcslen(d);
    size_t i = 0;
    for (; i < n && s[i]; ++i) e[i] = s[i];
    e[i] = 0;
    return d;
}
wchar_t *wcschr(const wchar_t *s, wchar_t c) {
    for (;; ++s) {
        if (*s == c) return (wchar_t *)s;
        if (!*s) return 0;
    }
}
wchar_t *wcsrchr(const wchar_t *s, wchar_t c) {
    const wchar_t *found = 0;
    for (;; ++s) {
        if (*s == c) found = s;
        if (!*s) return (wchar_t *)found;
    }
}
size_t wcsspn(const wchar_t *s, const wchar_t *accept) { size_t n = 0; while (s[n] && wcschr(accept, s[n])) ++n; return n; }
size_t wcscspn(const wchar_t *s, const wchar_t *reject) { size_t n = 0; while (s[n] && !wcschr(reject, s[n])) ++n; return n; }
wchar_t *wcspbrk(const wchar_t *s, const wchar_t *accept) { s += wcscspn(s, accept); return *s ? (wchar_t *)s : 0; }
wchar_t *wcsstr(const wchar_t *h, const wchar_t *n) {
    size_t length = wcslen(n);
    if (!length) return (wchar_t *)h;
    for (; *h; ++h)
        if (!wcsncmp(h, n, length)) return (wchar_t *)h;
    return 0;
}
wchar_t *wcstok(wchar_t *s, const wchar_t *delimiters, wchar_t **state) {
    if (!s) s = *state;
    if (!s) return 0;
    s += wcsspn(s, delimiters);
    if (!*s) { *state = 0; return 0; }
    wchar_t *end = s + wcscspn(s, delimiters);
    if (*end) *end++ = 0;
    else end = 0;
    *state = end;
    return s;
}
wchar_t *wcsdup(const wchar_t *s) {
    size_t n = wcslen(s) + 1;
    wchar_t *d = malloc(n * sizeof(wchar_t));
    return d ? wmemcpy(d, s, n) : 0;
}
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n) {
    for (; n; --n, ++s)
        if (*s == c) return (wchar_t *)s;
    return 0;
}
int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n) {
    for (; n; --n, ++a, ++b)
        if (*a != *b) return *a < *b ? -1 : 1;
    return 0;
}
wchar_t *wmemcpy(wchar_t *d, const wchar_t *s, size_t n) { return memcpy(d, s, n * sizeof(wchar_t)); }
wchar_t *wmemmove(wchar_t *d, const wchar_t *s, size_t n) { return memmove(d, s, n * sizeof(wchar_t)); }
wchar_t *wmemset(wchar_t *d, wchar_t c, size_t n) {
    for (size_t i = 0; i < n; ++i) d[i] = c;
    return d;
}

/* Numeric conversions: narrow the ASCII prefix and reuse the char versions. */
static size_t narrow(const wchar_t *w, char *out, size_t size) {
    size_t n = 0;
    while (n + 1 < size && w[n] > 0 && w[n] < 128) out[n] = (char)w[n], ++n;
    out[n] = 0;
    return n;
}
#define WIDE_CONVERT(type, name, call)                                \
    type name(const wchar_t *w, wchar_t **end, int base) {            \
        char text[128], *stop;                                        \
        narrow(w, text, sizeof(text));                                \
        type v = call(text, &stop, base);                             \
        if (end) *end = (wchar_t *)w + (stop - text);                 \
        return v;                                                     \
    }
WIDE_CONVERT(long, wcstol, strtol)
WIDE_CONVERT(unsigned long, wcstoul, strtoul)
WIDE_CONVERT(long long, wcstoll, strtoll)
WIDE_CONVERT(unsigned long long, wcstoull, strtoull)
#define WIDE_FLOAT(type, name, call)                                  \
    type name(const wchar_t *w, wchar_t **end) {                      \
        char text[512], *stop;                                        \
        narrow(w, text, sizeof(text));                                \
        type v = call(text, &stop);                                   \
        if (end) *end = (wchar_t *)w + (stop - text);                 \
        return v;                                                     \
    }
WIDE_FLOAT(double, wcstod, strtod)
WIDE_FLOAT(float, wcstof, strtof)
WIDE_FLOAT(long double, wcstold, strtold)

int wcwidth(wchar_t c) {
    if (!c) return 0;
    if (c < 32 || (c >= 0x7f && c < 0xa0)) return -1;
    if ((c >= 0x300 && c < 0x370) || c == 0x200b) return 0;
    if ((c >= 0x1100 && c <= 0x115f) || (c >= 0x2e80 && c <= 0xa4cf) || (c >= 0xac00 && c <= 0xd7a3) ||
        (c >= 0xf900 && c <= 0xfaff) || (c >= 0xfe30 && c <= 0xfe4f) || (c >= 0xff00 && c <= 0xff60) ||
        (c >= 0xffe0 && c <= 0xffe6) || (c >= 0x1f300 && c <= 0x1faff) || (c >= 0x20000 && c <= 0x3fffd))
        return 2;
    return 1;
}
int wcswidth(const wchar_t *s, size_t n) {
    int total = 0;
    for (; n && *s; --n, ++s) {
        int w = wcwidth(*s);
        if (w < 0) return -1;
        total += w;
    }
    return total;
}

size_t wcsftime(wchar_t *out, size_t size, const wchar_t *format, const struct tm *tm) {
    char narrow_format[256], text[512];
    size_t i = 0;
    for (; format[i] && i + 1 < sizeof(narrow_format); ++i) narrow_format[i] = format[i] < 128 ? (char)format[i] : '?';
    narrow_format[i] = 0;
    size_t n = strftime(text, sizeof(text), narrow_format, tm);
    if (!n || n >= size) return 0;
    for (i = 0; i <= n; ++i) out[i] = (unsigned char)text[i];
    return n;
}

/* Wide stream I/O in UTF-8. */
wint_t fgetwc(FILE *f) {
    mbstate_t state = {0};
    for (;;) {
        int c = fgetc(f);
        if (c == EOF) return WEOF;
        char byte = (char)c;
        wchar_t wc;
        size_t r = mbrtowc(&wc, &byte, 1, &state);
        if (r == (size_t)-1) return WEOF;
        if (r != (size_t)-2) return (wint_t)wc;
    }
}
wint_t getwc(FILE *f) { return fgetwc(f); }
wint_t getwchar(void) { return fgetwc(stdin); }
wint_t fputwc(wchar_t wc, FILE *f) {
    char buffer[4];
    size_t n = wcrtomb(buffer, wc, 0);
    if (n == (size_t)-1 || fwrite(buffer, 1, n, f) != n) return WEOF;
    return (wint_t)wc;
}
wint_t putwc(wchar_t wc, FILE *f) { return fputwc(wc, f); }
wint_t putwchar(wchar_t wc) { return fputwc(wc, stdout); }
wint_t ungetwc(wint_t wc, FILE *f) {
    char buffer[4];
    size_t n = wcrtomb(buffer, (wchar_t)wc, 0);
    if (wc == WEOF || n != 1) return WEOF;
    return ungetc((unsigned char)buffer[0], f) == EOF ? WEOF : wc;
}
wchar_t *fgetws(wchar_t *s, int n, FILE *f) {
    int i = 0;
    while (i < n - 1) {
        wint_t c = fgetwc(f);
        if (c == WEOF) break;
        s[i++] = (wchar_t)c;
        if (c == '\n') break;
    }
    if (!i) return 0;
    s[i] = 0;
    return s;
}
int fputws(const wchar_t *s, FILE *f) {
    for (; *s; ++s)
        if (fputwc(*s, f) == WEOF) return -1;
    return 0;
}
int fwide(FILE *f, int mode) { (void)f; return mode; }
