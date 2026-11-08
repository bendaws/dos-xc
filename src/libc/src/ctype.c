/*
 * DOS-XC C library - character classes ("C" locale)
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#include <ctype.h>
#include <wctype.h>
#include <string.h>

int isascii(int c) { return (unsigned)c < 128; }
int toascii(int c) { return c & 0x7f; }
int isdigit(int c) { return (unsigned)c - '0' < 10; }
int islower(int c) { return (unsigned)c - 'a' < 26; }
int isupper(int c) { return (unsigned)c - 'A' < 26; }
int isalpha(int c) { return ((unsigned)c | 32) - 'a' < 26; }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || ((unsigned)c | 32) - 'a' < 6; }
int isblank(int c) { return c == ' ' || c == '\t'; }
int isspace(int c) { return c == ' ' || (unsigned)c - '\t' < 5; }
int iscntrl(int c) { return (unsigned)c < 32 || c == 127; }
int isprint(int c) { return (unsigned)c - 32 < 95; }
int isgraph(int c) { return (unsigned)c - 33 < 94; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int tolower(int c) { return isupper(c) ? c | 32 : c; }
int toupper(int c) { return islower(c) ? c & 0x5f : c; }

/* Wide classes: ASCII rules, other code points are letters if above Latin-1 controls. */
int iswdigit(wint_t c) { return (unsigned)c - '0' < 10; }
int iswlower(wint_t c) { return c < 128 ? islower((int)c) : (c >= 0xdf && c <= 0xff && c != 0xf7); }
int iswupper(wint_t c) { return c < 128 ? isupper((int)c) : (c >= 0xc0 && c <= 0xde && c != 0xd7); }
int iswalpha(wint_t c) { return c < 128 ? isalpha((int)c) : c >= 0xaa && c != 0xd7 && c != 0xf7 && c != WEOF; }
int iswalnum(wint_t c) { return iswalpha(c) || iswdigit(c); }
int iswxdigit(wint_t c) { return c < 128 && isxdigit((int)c); }
int iswblank(wint_t c) { return c == ' ' || c == '\t'; }
int iswspace(wint_t c) { return c < 128 ? isspace((int)c) : c == 0x85 || c == 0x2028 || c == 0x2029 || c == 0x3000; }
int iswcntrl(wint_t c) { return (unsigned)c < 32 || (c >= 127 && c < 160) || c == 0x2028 || c == 0x2029; }
int iswprint(wint_t c) { return c != WEOF && !iswcntrl(c) && c < 0x110000; }
int iswgraph(wint_t c) { return iswprint(c) && !iswspace(c) && c != ' '; }
int iswpunct(wint_t c) { return iswgraph(c) && !iswalnum(c); }
wint_t towlower(wint_t c) { return c < 128 ? (wint_t)tolower((int)c) : iswupper(c) ? c + 32 : c; }
wint_t towupper(wint_t c) { return c < 128 ? (wint_t)toupper((int)c) : (iswlower(c) && c != 0xdf && c != 0xff) ? c - 32 : c; }

static const char *const class_names[] = {"alnum", "alpha", "blank", "cntrl", "digit", "graph",
                                          "lower", "print", "punct", "space", "upper", "xdigit"};
wctype_t wctype(const char *name) {
    for (unsigned i = 0; i < sizeof(class_names) / sizeof(*class_names); ++i)
        if (!strcmp(name, class_names[i])) return i + 1;
    return 0;
}
int iswctype(wint_t c, wctype_t type) {
    switch (type) {
    case 1: return iswalnum(c);
    case 2: return iswalpha(c);
    case 3: return iswblank(c);
    case 4: return iswcntrl(c);
    case 5: return iswdigit(c);
    case 6: return iswgraph(c);
    case 7: return iswlower(c);
    case 8: return iswprint(c);
    case 9: return iswpunct(c);
    case 10: return iswspace(c);
    case 11: return iswupper(c);
    case 12: return iswxdigit(c);
    }
    return 0;
}
static const int lower_map = 1, upper_map = 2;
wctrans_t wctrans(const char *name) {
    if (!strcmp(name, "tolower")) return &lower_map;
    if (!strcmp(name, "toupper")) return &upper_map;
    return 0;
}
wint_t towctrans(wint_t c, wctrans_t t) {
    return t == &lower_map ? towlower(c) : t == &upper_map ? towupper(c) : c;
}
