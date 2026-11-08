/*
 * DOS-XC C library - termcap without a terminal database (see termcap.h)
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#include <termcap.h>
#include <stdio.h>
#include <ctype.h>

char PC, *UP, *BC;
short ospeed = 15;

int tgetent(char *buffer, const char *name) { (void)buffer; (void)name; return 0; }
int tgetflag(const char *id) { (void)id; return 0; }
int tgetnum(const char *id) { (void)id; return -1; }
char *tgetstr(const char *id, char **area) { (void)id; (void)area; return 0; }

/* Cursor addressing with the classic termcap % escapes. */
char *tgoto(const char *cap, int column, int row) {
    static char out[64];
    int args[2] = {row, column}, which = 0;
    size_t n = 0;
    if (!cap) return "OOPS";
    for (const char *p = cap; *p && n + 8 < sizeof(out); ++p) {
        if (*p != '%') { out[n++] = *p; continue; }
        switch (*++p) {
        case 'd': n += (size_t)snprintf(out + n, sizeof(out) - n, "%d", args[which++ & 1]); break;
        case '2': n += (size_t)snprintf(out + n, sizeof(out) - n, "%02d", args[which++ & 1]); break;
        case '3': n += (size_t)snprintf(out + n, sizeof(out) - n, "%03d", args[which++ & 1]); break;
        case '.': out[n++] = (char)args[which++ & 1]; break;
        case 'i': ++args[0]; ++args[1]; break;
        case 'r': { int t = args[0]; args[0] = args[1]; args[1] = t; break; }
        case '%': out[n++] = '%'; break;
        case '+': if (p[1]) { out[n++] = (char)(args[which++ & 1] + *++p); } break;
        default: if (!*p) --p; break;
        }
    }
    out[n] = 0;
    return out;
}

/* Output a capability; a leading padding count is skipped. */
int tputs(const char *s, int lines, int (*put)(int)) {
    (void)lines;
    if (!s) return -1;
    while (isdigit((unsigned char)*s) || *s == '.') ++s;
    if (*s == '*') ++s;
    for (; *s; ++s) put((unsigned char)*s);
    return 0;
}
