/*
 * DOS-XC /os/libc/src/stdlib.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "libc.h"
#include <ctype.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <langinfo.h>
#include <libgen.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- integers ---- */
static unsigned long long parse_unsigned(const char *s, char **end, int base, int *negative, int *overflow,
                                         unsigned long long limit) {
    const char *start = s;
    while (isspace((unsigned char)*s)) ++s;
    *negative = 0;
    *overflow = 0;
    if (*s == '+' || *s == '-') *negative = *s++ == '-';
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] | 32) == 'x' && isxdigit((unsigned char)s[2])) {
        s += 2;
        base = 16;
    } else if ((base == 0 || base == 2) && s[0] == '0' && (s[1] | 32) == 'b' && (s[2] == '0' || s[2] == '1')) {
        s += 2;
        base = 2;
    } else if (base == 0) {
        base = *s == '0' ? 8 : 10;
    }
    if (base < 2 || base > 36) {
        errno = EINVAL;
        if (end) *end = (char *)start;
        return 0;
    }
    unsigned long long value = 0;
    int any = 0;
    for (;; ++s) {
        int digit;
        if (isdigit((unsigned char)*s)) digit = *s - '0';
        else if (isalpha((unsigned char)*s)) digit = (*s | 32) - 'a' + 10;
        else break;
        if (digit >= base) break;
        any = 1;
        if (value > (limit - (unsigned)digit) / (unsigned)base) *overflow = 1;
        else value = value * (unsigned)base + (unsigned)digit;
    }
    if (end) *end = (char *)(any ? s : start);
    return value;
}

unsigned long long strtoull(const char *s, char **end, int base) {
    int negative, overflow;
    unsigned long long v = parse_unsigned(s, end, base, &negative, &overflow, ULLONG_MAX);
    if (overflow) {
        errno = ERANGE;
        return ULLONG_MAX;
    }
    return negative ? -v : v;
}

long long strtoll(const char *s, char **end, int base) {
    int negative, overflow;
    unsigned long long v = parse_unsigned(s, end, base, &negative, &overflow, ULLONG_MAX);
    if (overflow || v > (unsigned long long)LLONG_MAX + negative) {
        errno = ERANGE;
        return negative ? LLONG_MIN : LLONG_MAX;
    }
    return negative ? (long long)-v : (long long)v;
}

unsigned long strtoul(const char *s, char **end, int base) {
    int negative, overflow;
    unsigned long long v = parse_unsigned(s, end, base, &negative, &overflow, ULONG_MAX);
    if (overflow) {
        errno = ERANGE;
        return ULONG_MAX;
    }
    return (unsigned long)(negative ? -v : v);
}

long strtol(const char *s, char **end, int base) {
    int negative, overflow;
    unsigned long long v = parse_unsigned(s, end, base, &negative, &overflow, ULONG_MAX);
    if (overflow || v > (unsigned long long)LONG_MAX + negative) {
        errno = ERANGE;
        return negative ? LONG_MIN : LONG_MAX;
    }
    return negative ? (long)-v : (long)v;
}

intmax_t strtoimax(const char *s, char **end, int base) { return strtoll(s, end, base); }
uintmax_t strtoumax(const char *s, char **end, int base) { return strtoull(s, end, base); }
int atoi(const char *s) { return (int)strtol(s, 0, 10); }
long atol(const char *s) { return strtol(s, 0, 10); }
long long atoll(const char *s) { return strtoll(s, 0, 10); }

int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }
long long llabs(long long v) { return v < 0 ? -v : v; }
intmax_t imaxabs(intmax_t v) { return v < 0 ? -v : v; }
div_t div(int a, int b) { return (div_t){a / b, a % b}; }
ldiv_t ldiv(long a, long b) { return (ldiv_t){a / b, a % b}; }
lldiv_t lldiv(long long a, long long b) { return (lldiv_t){a / b, a % b}; }
imaxdiv_t imaxdiv(intmax_t a, intmax_t b) { return (imaxdiv_t){a / b, a % b}; }

/* ---- floating point ---- */
static long double power10(int e) {
    long double result = 1, base = 10;
    unsigned n = (unsigned)(e < 0 ? -e : e);
    while (n) {
        if (n & 1) result *= base;
        base *= base;
        n >>= 1;
    }
    return e < 0 ? 1 / result : result;
}

static int match_word(const char **s, const char *word) {
    size_t n = strlen(word);
    if (strncasecmp(*s, word, n)) return 0;
    *s += n;
    return 1;
}

long double strtold(const char *s, char **end) {
    const char *start = s;
    while (isspace((unsigned char)*s)) ++s;
    int negative = 0;
    if (*s == '+' || *s == '-') negative = *s++ == '-';
    long double value;
    if (match_word(&s, "infinity") || match_word(&s, "inf")) {
        value = __builtin_infl();
    } else if (match_word(&s, "nan")) {
        if (*s == '(') {
            const char *p = s + 1;
            while (isalnum((unsigned char)*p) || *p == '_') ++p;
            if (*p == ')') s = p + 1;
        }
        value = __builtin_nanl("");
    } else if (s[0] == '0' && (s[1] | 32) == 'x' && (isxdigit((unsigned char)s[2]) || (s[2] == '.' && isxdigit((unsigned char)s[3])))) {
        s += 2;
        unsigned long long mantissa = 0;
        long exponent = 0;
        int digits = 0, point = 0, sticky = 0;
        for (;; ++s) {
            if (*s == '.' && !point) { point = 1; continue; }
            if (!isxdigit((unsigned char)*s)) break;
            int d = isdigit((unsigned char)*s) ? *s - '0' : (*s | 32) - 'a' + 10;
            if (digits < 16) {
                mantissa = mantissa * 16 + (unsigned)d;
                if (mantissa) ++digits;
                if (point) exponent -= 4;
            } else {
                if (d) sticky = 1;
                if (!point) exponent += 4;
            }
        }
        if ((*s | 32) == 'p') {
            const char *p = s + 1;
            int en = 0;
            if (*p == '+' || *p == '-') en = *p++ == '-';
            if (isdigit((unsigned char)*p)) {
                long e = 0;
                while (isdigit((unsigned char)*p)) { if (e < 100000) e = e * 10 + (*p - '0'); ++p; }
                exponent += en ? -e : e;
                s = p;
            }
        }
        if (sticky) mantissa |= 1;
        value = ldexpl((long double)mantissa, (int)(exponent > 100000 ? 100000 : exponent < -100000 ? -100000 : exponent));
    } else {
        unsigned long long mantissa = 0;
        long exponent = 0;
        int digits = 0, point = 0, any = 0;
        for (;; ++s) {
            if (*s == '.' && !point) { point = 1; continue; }
            if (!isdigit((unsigned char)*s)) break;
            any = 1;
            if (digits < 19) {
                mantissa = mantissa * 10 + (unsigned)(*s - '0');
                if (mantissa) ++digits;
                if (point) --exponent;
            } else if (!point) {
                ++exponent;
            }
        }
        if (!any) {
            if (end) *end = (char *)start;
            return 0;
        }
        if ((*s | 32) == 'e') {
            const char *p = s + 1;
            int en = 0;
            if (*p == '+' || *p == '-') en = *p++ == '-';
            if (isdigit((unsigned char)*p)) {
                long e = 0;
                while (isdigit((unsigned char)*p)) { if (e < 100000) e = e * 10 + (*p - '0'); ++p; }
                exponent += en ? -e : e;
                s = p;
            }
        }
        if (!mantissa) value = 0;
        else if (exponent > 5000) value = __builtin_infl();
        else if (exponent < -5000) value = 0;
        else if (exponent < -4900) value = (long double)mantissa * power10(-4900) * power10((int)exponent + 4900);
        else value = exponent < 0 ? (long double)mantissa / power10((int)-exponent) : (long double)mantissa * power10((int)exponent);
        if (mantissa && (value == 0 || __builtin_isinf(value))) errno = ERANGE;
    }
    if (end) *end = (char *)s;
    return negative ? -value : value;
}

double strtod(const char *s, char **end) {
    long double v = strtold(s, end);
    double d = (double)v;
    if ((__builtin_isinf(d) && !__builtin_isinf(v)) || (d == 0 && v != 0)) errno = ERANGE;
    return d;
}
float strtof(const char *s, char **end) {
    long double v = strtold(s, end);
    float f = (float)v;
    if ((__builtin_isinf(f) && !__builtin_isinf(v)) || (f == 0 && v != 0)) errno = ERANGE;
    return f;
}
double atof(const char *s) { return strtod(s, 0); }

/* ---- sorting ---- */
static void swap(char *a, char *b, size_t size) {
    while (size--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

typedef int (*compare3)(const void *, const void *, void *);
struct plain { int (*compare)(const void *, const void *); };
static int call_plain(const void *a, const void *b, void *context) {
    return ((struct plain *)context)->compare(a, b);
}

static void sift(char *base, size_t start, size_t count, size_t size, compare3 cmp, void *context) {
    size_t root = start;
    while (2 * root + 1 < count) {
        size_t child = 2 * root + 1;
        if (child + 1 < count && cmp(base + child * size, base + (child + 1) * size, context) < 0) ++child;
        if (cmp(base + root * size, base + child * size, context) >= 0) return;
        swap(base + root * size, base + child * size, size);
        root = child;
    }
}

static void introsort(char *base, size_t count, size_t size, compare3 cmp, void *context, int depth) {
    while (count > 12) {
        if (!depth--) {
            for (size_t i = count / 2; i-- > 0;) sift(base, i, count, size, cmp, context);
            for (size_t end = count - 1; end > 0; --end) {
                swap(base, base + end * size, size);
                sift(base, 0, end, size, cmp, context);
            }
            return;
        }
        char *first = base, *middle = base + (count / 2) * size, *last = base + (count - 1) * size;
        if (cmp(middle, first, context) < 0) swap(middle, first, size);
        if (cmp(last, middle, context) < 0) {
            swap(last, middle, size);
            if (cmp(middle, first, context) < 0) swap(middle, first, size);
        }
        swap(middle, last - size, size);
        char *pivot = last - size, *i = first, *j = pivot;
        for (;;) {
            while (cmp((i += size), pivot, context) < 0) {
            }
            while (j > first && cmp((j -= size), pivot, context) > 0) {
            }
            if (i >= j) break;
            swap(i, j, size);
        }
        swap(i, pivot, size);
        size_t left = (size_t)(i - base) / size, right = count - left - 1;
        if (left < right) {
            introsort(base, left, size, cmp, context, depth);
            base = i + size;
            count = right;
        } else {
            introsort(i + size, right, size, cmp, context, depth);
            count = left;
        }
    }
    for (size_t k = 1; k < count; ++k)
        for (size_t m = k; m > 0 && cmp(base + (m - 1) * size, base + m * size, context) > 0; --m)
            swap(base + (m - 1) * size, base + m * size, size);
}

void qsort_r(void *base, size_t count, size_t size, int (*cmp)(const void *, const void *, void *), void *context) {
    int depth = 0;
    for (size_t n = count; n; n >>= 1) depth += 2;
    if (count > 1 && size) introsort(base, count, size, cmp, context, depth);
}

void qsort(void *base, size_t count, size_t size, int (*cmp)(const void *, const void *)) {
    struct plain p = {cmp};
    qsort_r(base, count, size, call_plain, &p);
}

void *bsearch(const void *key, const void *base, size_t count, size_t size, int (*cmp)(const void *, const void *)) {
    const char *b = base;
    while (count) {
        const char *middle = b + (count / 2) * size;
        int r = cmp(key, middle);
        if (!r) return (void *)middle;
        if (r > 0) {
            b = middle + size;
            count -= count / 2 + 1;
        } else {
            count /= 2;
        }
    }
    return 0;
}

static unsigned long long random_state = 0x853c49e6748fea9bull;
static unsigned next_random(unsigned long long *state) {
    *state = *state * 6364136223846793005ull + 1442695040888963407ull;
    unsigned x = (unsigned)(((*state >> 18) ^ *state) >> 27), r = (unsigned)(*state >> 59);
    return (x >> r) | (x << ((-r) & 31));
}
int rand(void) { return (int)(next_random(&random_state) & RAND_MAX); }
void srand(unsigned seed) { random_state = seed * 0x9e3779b97f4a7c15ull + 1; }
int rand_r(unsigned *seed) {
    unsigned long long state = *seed;
    int r = (int)(next_random(&state) & RAND_MAX);
    *seed = (unsigned)state;
    return r;
}
long random(void) { return rand(); }
void srandom(unsigned seed) { srand(seed); }
static unsigned long long rand48_step(unsigned short x[3]) {
    unsigned long long v = (unsigned long long)x[0] | (unsigned long long)x[1] << 16 | (unsigned long long)x[2] << 32;
    v = (v * 0x5deece66dull + 0xb) & 0xffffffffffffull;
    x[0] = (unsigned short)v; x[1] = (unsigned short)(v >> 16); x[2] = (unsigned short)(v >> 32);
    return v;
}
long jrand48(unsigned short x[3]) { return (long)(int)(rand48_step(x) >> 16); }
long nrand48(unsigned short x[3]) { return (long)(rand48_step(x) >> 17); }
double erand48(unsigned short x[3]) { return (double)rand48_step(x) / 281474976710656.0; }
long mrand48(void) { return (long)(int)((unsigned)rand() << 1 ^ (unsigned)rand()); }
long lrand48(void) { return rand(); }
void srand48(long seed) { srand((unsigned)seed); }
double drand48(void) { return (double)rand() / ((double)RAND_MAX + 1); }

static char **owned_environment;
static size_t owned_capacity;

static size_t name_length(const char *s) { return strcspn(s, "="); }

static char *env_lookup(const char *name) {
    size_t n = strlen(name);
    if (!environ || !n || strchr(name, '=')) return 0;
    for (char **e = environ; *e; ++e)
        if (!strncmp(*e, name, n) && (*e)[n] == '=') return *e + n + 1;
    for (char **e = environ; *e; ++e)
        if (!strncasecmp(*e, name, n) && (*e)[n] == '=') return *e + n + 1;
    return 0;
}
/* Programs may replace getenv and hold a lock around these. */
char *getenv(const char *name) { return env_lookup(name); }
char *secure_getenv(const char *name) { return env_lookup(name); }
char *__secure_getenv(const char *name) { return env_lookup(name); }

static int own_environment(size_t extra) {
    size_t count = 0;
    while (environ && environ[count]) ++count;
    if (environ == owned_environment && count + extra + 1 <= owned_capacity) return 0;
    size_t capacity = (count + extra + 1) * 2;
    char **copy = malloc(capacity * sizeof(char *));
    if (!copy) return -1;
    if (count) memcpy(copy, environ, count * sizeof(char *));
    copy[count] = 0;
    if (owned_environment == environ) free(owned_environment);
    environ = owned_environment = copy;
    owned_capacity = capacity;
    return 0;
}

static void remove_variable(const char *name, size_t n) {
    if (!environ) return;
    char **out = environ;
    for (char **e = environ; *e; ++e)
        if (!(name_length(*e) == n && !strncmp(*e, name, n))) *out++ = *e;
    *out = 0;
}

static int env_put(char *string) {
    size_t n = name_length(string);
    if (!string[n]) {
        remove_variable(string, n);
        return 0;
    }
    if (own_environment(1)) return -1;
    for (char **e = environ; *e; ++e)
        if (name_length(*e) == n && !strncmp(*e, string, n)) {
            *e = string;
            return 0;
        }
    size_t count = 0;
    while (environ[count]) ++count;
    environ[count] = string;
    environ[count + 1] = 0;
    return 0;
}

int putenv(char *entry) { return env_put(entry); }

int setenv(const char *name, const char *value, int overwrite) {
    size_t n = strlen(name);
    if (!n || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    if (!overwrite && env_lookup(name)) return 0;
    char *entry = malloc(n + strlen(value) + 2);
    if (!entry) return -1;
    memcpy(entry, name, n);
    entry[n] = '=';
    strcpy(entry + n + 1, value);
    return env_put(entry);
}

int unsetenv(const char *name) {
    size_t n = strlen(name);
    if (!n || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    remove_variable(name, n);
    return 0;
}

int clearenv(void) {
    if (own_environment(0)) return -1;
    environ[0] = 0;
    return 0;
}

static unsigned long long temp_seed;
static void fill_template(char *x, size_t n) {
    static const char letters[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    if (!temp_seed) temp_seed = dosxc_sys_nanoseconds() ^ ((unsigned long long)dosxc_sys_getpid() << 32);
    for (size_t i = 0; i < n; ++i) x[i] = letters[next_random(&temp_seed) % 62];
}

static char *template_letters(char *template, int suffix) {
    size_t length = strlen(template);
    if (suffix < 0 || length < (size_t)suffix + 6 || memcmp(template + length - suffix - 6, "XXXXXX", 6)) {
        errno = EINVAL;
        return 0;
    }
    return template + length - suffix - 6;
}

int mkostemps(char *template, int suffix, int flags) {
    char *x = template_letters(template, suffix);
    if (!x) return -1;
    for (int attempt = 0; attempt < 1000; ++attempt) {
        fill_template(x, 6);
        int fd = open(template, O_RDWR | O_CREAT | O_EXCL | (flags & O_CLOEXEC), 0600);
        if (fd >= 0 || errno != EEXIST) return fd;
    }
    errno = EEXIST;
    return -1;
}
int mkstemps(char *template, int suffix) { return mkostemps(template, suffix, 0); }
int mkstemp(char *template) { return mkostemps(template, 0, 0); }
int mkostemp(char *template, int flags) { return mkostemps(template, 0, flags); }

char *mkdtemp(char *template) {
    char *x = template_letters(template, 0);
    if (!x) return 0;
    for (int attempt = 0; attempt < 1000; ++attempt) {
        fill_template(x, 6);
        if (!mkdir(template, 0700)) return template;
        if (errno != EEXIST) return 0;
    }
    return 0;
}

char *mktemp(char *template) {
    char *x = template_letters(template, 0);
    struct stat s;
    if (!x) {
        template[0] = 0;
        return template;
    }
    for (int attempt = 0; attempt < 1000; ++attempt) {
        fill_template(x, 6);
        if (stat(template, &s)) return template;
    }
    template[0] = 0;
    return template;
}

static const char *temporary_directory(void) {
    const char *d = getenv("TMPDIR");
    if (!d || !*d) d = getenv("TEMP");
    if (!d || !*d) d = getenv("TMP");
    return d && *d ? d : P_tmpdir;
}

char *tempnam(const char *directory, const char *prefix) {
    struct stat s;
    if (!directory || stat(directory, &s) || !S_ISDIR(s.st_mode)) directory = temporary_directory();
    if (!prefix) prefix = "tmp";
    size_t n = strlen(directory) + strlen(prefix) + 16;
    char *name = malloc(n);
    if (!name) return 0;
    snprintf(name, n, "%s/%.5sXXXXXX", directory, prefix);
    return *mktemp(name) ? name : (free(name), (char *)0);
}

char *tmpnam(char *buffer) {
    static char internal[L_tmpnam];
    char *out = buffer ? buffer : internal;
    snprintf(out, L_tmpnam, "%s/tmpXXXXXX", temporary_directory());
    return *mktemp(out) ? out : 0;
}

/* ---- paths ---- */
char *basename(char *path) {
    if (!path || !*path) return ".";
    size_t n = strlen(path);
    while (n > 1 && (path[n - 1] == '/' || path[n - 1] == '\\')) path[--n] = 0;
    char *slash = 0;
    for (char *p = path; *p; ++p)
        if (*p == '/' || *p == '\\') slash = p;
    return slash && slash[1] ? slash + 1 : path;
}

char *dirname(char *path) {
    if (!path || !*path) return ".";
    size_t n = strlen(path);
    while (n > 1 && (path[n - 1] == '/' || path[n - 1] == '\\')) --n;
    while (n && path[n - 1] != '/' && path[n - 1] != '\\') --n;
    if (!n) return ".";
    while (n > 1 && (path[n - 1] == '/' || path[n - 1] == '\\')) --n;
    path[n] = 0;
    return path;
}

static struct lconv c_lconv = {".", "", "", "", "", "", "", "", "", "",
                               CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX,
                               CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX};
static struct __dosxc_locale { int unused; } c_locale_object;

char *setlocale(int category, const char *name) {
    if (category < 0 || category > LC_ALL) return 0;
    if (!name || !*name || !strcmp(name, "C") || !strcmp(name, "POSIX") || !strcmp(name, "C.UTF-8") ||
        !strcmp(name, "C.utf8"))
        return "C";
    return 0;
}
struct lconv *localeconv(void) { return &c_lconv; }
locale_t newlocale(int mask, const char *name, locale_t base) {
    (void)mask;
    (void)base;
    if (!setlocale(LC_ALL, name)) {
        errno = ENOENT;
        return 0;
    }
    return &c_locale_object;
}
locale_t duplocale(locale_t l) { return l; }
void freelocale(locale_t l) { (void)l; }
locale_t uselocale(locale_t l) { (void)l; return &c_locale_object; }

char *nl_langinfo(nl_item item) {
    static char *const names[] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat",
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
        "January", "February", "March", "April", "May", "June", "July", "August", "September", "October",
        "November", "December",
    };
    if (item >= ABDAY_1 && item <= MON_12) return names[item - ABDAY_1];
    switch (item) {
    case CODESET: return "ANSI_X3.4-1968";
    case RADIXCHAR: return ".";
    case THOUSEP: return "";
    case D_T_FMT: return "%a %b %e %H:%M:%S %Y";
    case D_FMT: return "%m/%d/%y";
    case T_FMT: return "%H:%M:%S";
    case T_FMT_AMPM: return "%I:%M:%S %p";
    case AM_STR: return "AM";
    case PM_STR: return "PM";
    case YESEXPR: return "^[yY]";
    case NOEXPR: return "^[nN]";
    case CRNCYSTR: return "-";
    }
    return "";
}
char *nl_langinfo_l(nl_item item, locale_t l) { (void)l; return nl_langinfo(item); }
size_t __dosxc_mb_cur_max(void) { return 4; }

char *getpass(const char *prompt) {
    static char buffer[128];
    fputs(prompt, stderr);
    fflush(stderr);
    int old = dosxc_sys_console(DOSXC_CONSOLE_MODE, DOSXC_CONSOLE_RAW, 0);
    size_t n = 0;
    for (;;) {
        char c;
        if (read(0, &c, 1) != 1 || c == '\n' || c == '\r') break;
        if (c == '\b') { if (n) --n; continue; }
        if (n + 1 < sizeof(buffer)) buffer[n++] = c;
    }
    dosxc_sys_console(DOSXC_CONSOLE_MODE, old, 0);
    buffer[n] = 0;
    fputs("\n", stderr);
    return buffer;
}
