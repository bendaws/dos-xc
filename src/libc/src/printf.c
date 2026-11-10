/*
 * DOS-XC C library - formatted output
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * Floating-point conversions are exact: the binary value is expanded to
 * decimal with multi-word arithmetic and rounded half-to-even, so every
 * printed digit is correct for float, double and long double.
 */
#include "stdio_impl.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

struct sink {
    void (*put)(struct sink *, const char *, size_t);
    size_t count;
    char *buffer;
    size_t capacity, length;
    FILE *file;
    int fd, failed;
    char local[512];
    size_t local_used;
};

static void emit(struct sink *s, const char *text, size_t n) {
    if (n) s->put(s, text, n);
    s->count += n;
}

static void pad(struct sink *s, char c, int n) {
    char block[64];
    memset(block, c, sizeof(block));
    while (n > 0) {
        int k = n < 64 ? n : 64;
        emit(s, block, (size_t)k);
        n -= k;
    }
}

enum { LEFT = 1, PLUS = 2, SPACE = 4, ALT = 8, ZERO = 16, GROUP = 32 };

/* Emit PREFIX + BODY within WIDTH, zero padding between them if asked. */
static void field(struct sink *s, const char *prefix, const char *body, size_t n, int width, int flags, int zero_ok) {
    size_t p = strlen(prefix);
    int fill = width - (int)(p + n);
    if (!(flags & LEFT) && !((flags & ZERO) && zero_ok)) pad(s, ' ', fill);
    emit(s, prefix, p);
    if (!(flags & LEFT) && (flags & ZERO) && zero_ok) pad(s, '0', fill);
    emit(s, body, n);
    if (flags & LEFT) pad(s, ' ', fill);
}

/* ---- integers ---- */
static void format_integer(struct sink *s, unsigned long long value, int negative, char conversion, int flags,
                           int width, int precision) {
    char digits[72];
    int n = 0, base = conversion == 'o' ? 8 : (conversion == 'x' || conversion == 'X' || conversion == 'p') ? 16 : 10;
    const char *alphabet = conversion == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
    unsigned long long v = value;
    while (v) {
        digits[sizeof(digits) - 1 - n++] = alphabet[v % (unsigned)base];
        v /= (unsigned)base;
    }
    if (precision < 0) precision = 1;
    else flags &= ~ZERO;
    char body[600];
    int zeros = precision > n ? precision - n : 0;
    if (zeros > (int)sizeof(body) - 80) zeros = (int)sizeof(body) - 80;
    if (conversion == 'o' && (flags & ALT) && zeros == 0 && (n == 0 || digits[sizeof(digits) - n] != '0')) zeros = 1;
    memset(body, '0', (size_t)zeros);
    memcpy(body + zeros, digits + sizeof(digits) - n, (size_t)n);
    char prefix[4] = "";
    if (negative) strcpy(prefix, "-");
    else if (flags & PLUS) strcpy(prefix, "+");
    else if (flags & SPACE) strcpy(prefix, " ");
    if (((flags & ALT) && value && (conversion == 'x' || conversion == 'X')) || conversion == 'p')
        strcpy(prefix, conversion == 'X' ? "0X" : "0x");
    field(s, prefix, body, (size_t)(zeros + n), width, flags, 1);
}

/* ---- exact floating-point digits ---- */
struct number {
    char *integer;          /* integer part, decimal, most significant first */
    int count, at;
    unsigned *fraction;     /* fraction F / 2^(32 * words), little endian */
    int words, fraction_zero;
};

static int next_digit(struct number *d) {
    if (d->at < d->count) return d->integer[d->at++] - '0';
    if (d->fraction_zero) return 0;
    unsigned long long carry = 0;
    int zero = 1;
    for (int i = 0; i < d->words; ++i) {
        unsigned long long t = (unsigned long long)d->fraction[i] * 10 + carry;
        d->fraction[i] = (unsigned)t;
        carry = t >> 32;
        if (d->fraction[i]) zero = 0;
    }
    d->fraction_zero = zero;
    return (int)carry;
}

static int rest_nonzero(const struct number *d) {
    for (int i = d->at; i < d->count; ++i)
        if (d->integer[i] != '0') return 1;
    return !d->fraction_zero;
}

#define MAX_WORDS 520

/* Expand VALUE (finite, non-negative) into D.  Storage comes from WORK. */
static void expand(long double value, struct number *d, unsigned *work, char *integer_text) {
    d->integer = integer_text;
    d->count = d->at = 0;
    d->fraction = work + MAX_WORDS;
    d->words = 0;
    d->fraction_zero = 1;
    if (value == 0) return;
    int e2;
    long double m = __builtin_frexpl(value, &e2);
    unsigned long long mantissa = (unsigned long long)__builtin_ldexpl(m, 64);
    int e = e2 - 64;                       /* value = mantissa * 2^e */
    unsigned *big = work;                  /* integer part, little-endian words */
    int n = 0;
    if (e >= 0) {
        int shift = e % 32, offset = e / 32;
        for (int i = 0; i < offset; ++i) big[i] = 0;
        unsigned long long low = mantissa << shift;
        unsigned long long high = shift ? mantissa >> (64 - shift) : 0;
        big[offset] = (unsigned)low;
        big[offset + 1] = (unsigned)(low >> 32);
        big[offset + 2] = (unsigned)high;
        n = offset + 3;
    } else {
        int k = -e;
        unsigned long long integer = k < 64 ? mantissa >> k : 0;
        unsigned long long fraction = k < 64 ? mantissa & ((1ull << k) - 1) : mantissa;
        big[0] = (unsigned)integer;
        big[1] = (unsigned)(integer >> 32);
        n = 2;
        if (fraction) {
            int words = (k + 31) / 32;
            int shift = words * 32 - k;     /* fraction << shift fits in words*32 bits */
            unsigned *f = d->fraction;
            for (int i = 0; i < words; ++i) f[i] = 0;
            /* place (fraction << shift) */
            int bit = shift;
            for (int i = 0; i < 64 && i + bit < words * 32; ++i)
                if ((fraction >> i) & 1) f[(i + bit) / 32] |= 1u << ((i + bit) % 32);
            d->words = words;
            d->fraction_zero = 0;
        }
    }
    while (n && !big[n - 1]) --n;
    /* integer part to decimal: repeated division by 10^9 */
    char chunks[MAX_WORDS * 10];
    int length = 0;
    while (n) {
        unsigned long long remainder = 0;
        for (int i = n - 1; i >= 0; --i) {
            unsigned long long t = (remainder << 32) | big[i];
            big[i] = (unsigned)(t / 1000000000u);
            remainder = t % 1000000000u;
        }
        while (n && !big[n - 1]) --n;
        for (int j = 0; j < 9; ++j) {
            chunks[length++] = (char)('0' + remainder % 10);
            remainder /= 10;
        }
    }
    while (length > 1 && chunks[length - 1] == '0') --length;
    if (length == 1 && chunks[0] == '0') length = 0;
    for (int i = 0; i < length; ++i) integer_text[i] = chunks[length - 1 - i];
    d->count = length;
}

/* Round DIGITS[0..n) given the next digit and sticky bit; returns 1 on carry out. */
static int round_digits(char *digits, int n, int next, int sticky) {
    if (next < 5 || (next == 5 && !sticky && (n == 0 || ((digits[n - 1] - '0') & 1) == 0))) return 0;
    for (int i = n - 1; i >= 0; --i) {
        if (digits[i] != '9') {
            ++digits[i];
            return 0;
        }
        digits[i] = '0';
    }
    return 1;
}

struct work_area { unsigned words[2 * MAX_WORDS]; char integer[MAX_WORDS * 10]; };

/* Fixed notation: returns length of BODY (allocated or local). */
static size_t fixed(long double value, int precision, int alt, char **out, struct work_area *w) {
    struct number d;
    expand(value, &d, w->words, w->integer);
    int integer_digits = d.count ? d.count : 1;
    size_t total = (size_t)integer_digits + (size_t)precision + 2;
    char *digits = malloc(total + 2);
    if (!digits) return 0;
    int n = 0;
    if (!d.count) digits[n++] = '0';
    for (int i = 0; i < d.count; ++i) digits[n++] = (char)('0' + next_digit(&d));
    for (int i = 0; i < precision; ++i) digits[n++] = (char)('0' + next_digit(&d));
    int next = next_digit(&d), sticky = rest_nonzero(&d);
    int carry = round_digits(digits, n, next, sticky);
    char *body = malloc(total + 4);
    if (!body) {
        free(digits);
        return 0;
    }
    size_t length = 0;
    if (carry) body[length++] = '1';
    memcpy(body + length, digits, (size_t)integer_digits);
    length += (size_t)integer_digits;
    if (precision || alt) body[length++] = '.';
    memcpy(body + length, digits + integer_digits, (size_t)precision);
    length += (size_t)precision;
    free(digits);
    *out = body;
    return length;
}

/* PRECISION+1 significant digits into DIGITS, decimal exponent into *exponent. */
static void significant(long double value, int precision, char *digits, int *exponent, struct work_area *w) {
    if (value == 0) {
        memset(digits, '0', (size_t)precision + 1);
        *exponent = 0;
        return;
    }
    struct number d;
    expand(value, &d, w->words, w->integer);
    int x, first;
    if (d.count) {
        x = d.count - 1;
        first = next_digit(&d);
    } else {
        x = -1;
        while ((first = next_digit(&d)) == 0) --x;
    }
    digits[0] = (char)('0' + first);
    for (int n = 1; n <= precision; ++n) digits[n] = (char)('0' + next_digit(&d));
    int next = next_digit(&d), sticky = rest_nonzero(&d);
    if (round_digits(digits, precision + 1, next, sticky)) {
        digits[0] = '1';
        ++x;
    }
    *exponent = x;
}

static size_t exponential(long double value, int precision, int alt, char upper, char **out, struct work_area *w) {
    char *digits = malloc((size_t)precision + 2);
    char *body = malloc((size_t)precision + 16);
    if (!digits || !body) {
        free(digits);
        free(body);
        return 0;
    }
    int x;
    significant(value, precision, digits, &x, w);
    size_t n = 0;
    body[n++] = digits[0];
    if (precision || alt) body[n++] = '.';
    memcpy(body + n, digits + 1, (size_t)precision);
    n += (size_t)precision;
    free(digits);
    body[n++] = upper ? 'E' : 'e';
    body[n++] = x < 0 ? '-' : '+';
    unsigned ax = (unsigned)(x < 0 ? -x : x);
    char e[8];
    int k = 0;
    do e[k++] = (char)('0' + ax % 10);
    while (ax /= 10);
    if (k < 2) e[k++] = '0';
    while (k) body[n++] = e[--k];
    *out = body;
    return n;
}

static void strip_zeros(char *body, size_t *length) {
    char *point = memchr(body, '.', *length);
    if (!point) return;
    char *exponent = memchr(body, 'e', *length);
    if (!exponent) exponent = memchr(body, 'E', *length);
    size_t tail = exponent ? (size_t)(body + *length - exponent) : 0;
    char *end = exponent ? exponent : body + *length;
    char *p = end;
    while (p > point + 1 && p[-1] == '0') --p;
    if (p == point + 1) p = point;
    memmove(p, end, tail);
    *length = (size_t)(p - body) + tail;
}

static size_t hexadecimal(long double value, int precision, int alt, char upper, char **out) {
    char *body = malloc(64 + (size_t)(precision > 0 ? precision : 0));
    if (!body) return 0;
    const char *alphabet = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    size_t n = 0;
    body[n++] = '0';
    body[n++] = upper ? 'X' : 'x';
    int exponent = 0;
    unsigned long long fraction = 0;
    int lead = 0;
    if (value != 0) {
        int e2;
        long double m = __builtin_frexpl(value, &e2);
        unsigned long long mantissa = (unsigned long long)__builtin_ldexpl(m, 64);
        lead = 1;
        fraction = mantissa << 1;          /* 64 fraction bits after the leading 1 */
        exponent = e2 - 1;
    }
    int digits = 16;
    if (precision >= 0 && precision < 16) {
        int drop = (16 - precision) * 4;
        unsigned long long half = 1ull << (drop - 1), rest = fraction & ((half << 1) - 1);
        fraction >>= drop;
        if (rest > half || (rest == half && (fraction & 1))) {
            if (++fraction >> (precision * 4)) {
                fraction = 0;
                ++lead;
                if (lead == 2) {
                    lead = 1;
                    ++exponent;
                }
            }
        }
        fraction <<= drop;
        digits = precision;
    } else if (precision < 0) {
        digits = 16;
        while (digits && !((fraction >> (64 - digits * 4)) & 15)) --digits;
    }
    body[n++] = (char)('0' + lead);
    if (digits || alt) body[n++] = '.';
    for (int i = 0; i < digits && i < 16; ++i) body[n++] = alphabet[(fraction >> (60 - i * 4)) & 15];
    for (int i = 16; i < digits; ++i) body[n++] = '0';
    body[n++] = upper ? 'P' : 'p';
    body[n++] = exponent < 0 ? '-' : '+';
    unsigned ax = (unsigned)(exponent < 0 ? -exponent : exponent);
    char e[8];
    int k = 0;
    do e[k++] = (char)('0' + ax % 10);
    while (ax /= 10);
    while (k) body[n++] = e[--k];
    *out = body;
    return n;
}

static void format_float(struct sink *s, long double value, char conversion, int flags, int width, int precision) {
    char prefix[4] = "";
    int negative = __builtin_signbit(value) != 0;
    if (negative) value = -value;
    if (negative) strcpy(prefix, "-");
    else if (flags & PLUS) strcpy(prefix, "+");
    else if (flags & SPACE) strcpy(prefix, " ");
    int upper = conversion >= 'A' && conversion <= 'Z';
    if (__builtin_isnan(value) || __builtin_isinf(value)) {
        const char *text = __builtin_isnan(value) ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        field(s, prefix, text, 3, width, flags, 0);
        return;
    }
    static struct work_area work;          /* single-threaded: one conversion at a time */
    char *body = 0;
    size_t length = 0;
    int alt = (flags & ALT) != 0;
    char lower = (char)(conversion | 32);
    if (lower == 'a') {
        length = hexadecimal(value, precision, alt, (char)upper, &body);
    } else {
        if (precision < 0) precision = 6;
        if (precision > 100000) precision = 100000;
        if (lower == 'f') {
            length = fixed(value, precision, alt, &body, &work);
        } else if (lower == 'e') {
            length = exponential(value, precision, alt, (char)upper, &body, &work);
        } else {
            int p = precision ? precision : 1, x;
            char *digits = malloc((size_t)p + 2);
            if (!digits) return;
            significant(value, p - 1, digits, &x, &work);
            free(digits);
            if (value == 0) x = 0;
            if (p > x && x >= -4) length = fixed(value, p - 1 - x, alt, &body, &work);
            else length = exponential(value, p - 1, alt, (char)upper, &body, &work);
            if (!alt && body) strip_zeros(body, &length);
        }
    }
    if (!body) return;
    if (lower == 'a') {
        emit(s, prefix, strlen(prefix));
        width -= (int)strlen(prefix);
        int fill = width - (int)length;
        if (!(flags & LEFT) && !(flags & ZERO)) pad(s, ' ', fill), fill = 0;
        if (!(flags & LEFT) && (flags & ZERO)) {
            emit(s, body, 2);
            pad(s, '0', fill);
            emit(s, body + 2, length - 2);
        } else {
            emit(s, body, length);
            if (flags & LEFT) pad(s, ' ', fill);
        }
    } else {
        field(s, prefix, body, length, width, flags, 1);
    }
    free(body);
}

/* ---- the format interpreter ---- */
static int format(struct sink *s, const char *f, va_list ap) {
    for (; *f; ++f) {
        if (*f != '%') {
            const char *start = f;
            while (f[1] && f[1] != '%') ++f;
            emit(s, start, (size_t)(f - start + 1));
            continue;
        }
        ++f;
        int flags = 0, width = 0, precision = -1;
        for (;; ++f) {
            if (*f == '-') flags |= LEFT;
            else if (*f == '+') flags |= PLUS;
            else if (*f == ' ') flags |= SPACE;
            else if (*f == '#') flags |= ALT;
            else if (*f == '0') flags |= ZERO;
            else if (*f == '\'') flags |= GROUP;
            else break;
        }
        if (*f == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                flags |= LEFT;
                width = -width;
            }
            ++f;
        } else {
            while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        }
        if (*f == '.') {
            ++f;
            precision = 0;
            if (*f == '*') {
                precision = va_arg(ap, int);
                ++f;
            } else {
                while (*f >= '0' && *f <= '9') precision = precision * 10 + (*f++ - '0');
            }
        }
        int length = 0; /* 1 hh, 2 h, 3 l, 4 ll, 5 j, 6 z, 7 t, 8 L */
        for (;; ++f) {
            if (*f == 'h') length = length == 2 ? 1 : 2;
            else if (*f == 'l') length = length == 3 ? 4 : 3;
            else if (*f == 'q') length = 4;
            else if (*f == 'j') length = 5;
            else if (*f == 'z' || *f == 'Z') length = 6;
            else if (*f == 't') length = 7;
            else if (*f == 'L') length = 8;
            else break;
        }
        char c = *f;
        switch (c) {
        case 'd':
        case 'i': {
            long long v;
            switch (length) {
            case 1: v = (signed char)va_arg(ap, int); break;
            case 2: v = (short)va_arg(ap, int); break;
            case 3: v = va_arg(ap, long); break;
            case 4: case 8: v = va_arg(ap, long long); break;
            case 5: v = va_arg(ap, long long); break;
            case 6: case 7: v = va_arg(ap, ptrdiff_t); break;
            default: v = va_arg(ap, int);
            }
            format_integer(s, v < 0 ? -(unsigned long long)v : (unsigned long long)v, v < 0, 'd', flags, width, precision);
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            unsigned long long v;
            switch (length) {
            case 1: v = (unsigned char)va_arg(ap, unsigned); break;
            case 2: v = (unsigned short)va_arg(ap, unsigned); break;
            case 3: v = va_arg(ap, unsigned long); break;
            case 4: case 5: case 8: v = va_arg(ap, unsigned long long); break;
            case 6: case 7: v = va_arg(ap, size_t); break;
            default: v = va_arg(ap, unsigned);
            }
            format_integer(s, v, 0, c, flags & ~(PLUS | SPACE), width, precision);
            break;
        }
        case 'p': {
            void *p = va_arg(ap, void *);
            if (!p) field(s, "", "(nil)", 5, width, flags, 0);
            else format_integer(s, (uintptr_t)p, 0, 'p', flags, width, precision);
            break;
        }
        case 'c': {
            char ch;
            if (length == 3) {
                char mb[8];
                mbstate_t state = {0};
                size_t n = wcrtomb(mb, (wchar_t)va_arg(ap, wint_t), &state);
                field(s, "", mb, n == (size_t)-1 ? 0 : n, width, flags, 0);
                break;
            }
            ch = (char)va_arg(ap, int);
            field(s, "", &ch, 1, width, flags, 0);
            break;
        }
        case 's': {
            if (length == 3) {
                const wchar_t *w = va_arg(ap, const wchar_t *);
                if (!w) w = L"(null)";
                char mb[8];
                size_t total = 0;
                mbstate_t state = {0};
                for (const wchar_t *p = w; *p; ++p) {
                    size_t n = wcrtomb(mb, *p, &state);
                    if (n == (size_t)-1 || (precision >= 0 && total + n > (size_t)precision)) break;
                    total += n;
                }
                if (!(flags & LEFT)) pad(s, ' ', width - (int)total);
                size_t done = 0;
                for (const wchar_t *p = w; *p && done < total; ++p) {
                    size_t n = wcrtomb(mb, *p, &state);
                    emit(s, mb, n);
                    done += n;
                }
                if (flags & LEFT) pad(s, ' ', width - (int)total);
                break;
            }
            const char *text = va_arg(ap, const char *);
            if (!text) text = "(null)";
            size_t n = precision >= 0 ? strnlen(text, (size_t)precision) : strlen(text);
            field(s, "", text, n, width, flags, 0);
            break;
        }
        case 'm': {
            const char *text = strerror(errno);
            field(s, "", text, strlen(text), width, flags, 0);
            break;
        }
        case 'n': {
            void *p = va_arg(ap, void *);
            switch (length) {
            case 1: *(signed char *)p = (signed char)s->count; break;
            case 2: *(short *)p = (short)s->count; break;
            case 3: *(long *)p = (long)s->count; break;
            case 4: case 5: *(long long *)p = (long long)s->count; break;
            case 6: case 7: *(size_t *)p = s->count; break;
            default: *(int *)p = (int)s->count;
            }
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            long double v = length == 8 ? va_arg(ap, long double) : (long double)va_arg(ap, double);
            format_float(s, v, c, flags, width, precision);
            break;
        }
        case '%': emit(s, "%", 1); break;
        case 0: --f; break;
        default: emit(s, "%", 1); emit(s, f, 1); break;
        }
    }
    return 0;
}

/* ---- sinks ---- */
static void put_string(struct sink *s, const char *text, size_t n) {
    if (s->length < s->capacity) {
        size_t room = s->capacity - s->length;
        memcpy(s->buffer + s->length, text, n < room ? n : room);
    }
    s->length += n;
}

static void put_growing(struct sink *s, const char *text, size_t n) {
    if (s->failed) return;
    if (s->length + n + 1 > s->capacity) {
        size_t capacity = s->capacity ? s->capacity * 2 : 128;
        while (capacity < s->length + n + 1) capacity *= 2;
        char *bigger = realloc(s->buffer, capacity);
        if (!bigger) {
            s->failed = 1;
            return;
        }
        s->buffer = bigger;
        s->capacity = capacity;
    }
    memcpy(s->buffer + s->length, text, n);
    s->length += n;
}

static void drain(struct sink *s) {
    if (!s->local_used) return;
    if (s->file) {
        if (fwrite(s->local, 1, s->local_used, s->file) != s->local_used) s->failed = 1;
    } else {
        size_t done = 0;
        while (done < s->local_used) {
            ssize_t n = write(s->fd, s->local + done, s->local_used - done);
            if (n <= 0) {
                s->failed = 1;
                break;
            }
            done += (size_t)n;
        }
    }
    s->local_used = 0;
}

static void put_buffered(struct sink *s, const char *text, size_t n) {
    while (n) {
        if (s->local_used == sizeof(s->local)) drain(s);
        size_t k = sizeof(s->local) - s->local_used;
        if (k > n) k = n;
        memcpy(s->local + s->local_used, text, k);
        s->local_used += k;
        text += k;
        n -= k;
    }
}

static int finish(struct sink *s) {
    if (s->count > 0x7fffffff) {
        errno = EOVERFLOW;
        return -1;
    }
    return (int)s->count;
}

int vsnprintf(char *buffer, size_t size, const char *f, va_list ap) {
    struct sink s = {0};
    s.put = put_string;
    s.buffer = buffer;
    s.capacity = size ? size - 1 : 0;
    format(&s, f, ap);
    if (size) buffer[s.length < s.capacity ? s.length : s.capacity] = 0;
    return finish(&s);
}

int vsprintf(char *buffer, const char *f, va_list ap) { return vsnprintf(buffer, (size_t)-1 / 2, f, ap); }

int vasprintf(char **out, const char *f, va_list ap) {
    struct sink s = {0};
    s.put = put_growing;
    format(&s, f, ap);
    put_growing(&s, "", 0);
    if (s.failed || !s.buffer) {
        free(s.buffer);
        *out = 0;
        return -1;
    }
    s.buffer[s.length] = 0;
    *out = s.buffer;
    return finish(&s);
}

int vfprintf(FILE *file, const char *f, va_list ap) {
    STREAM_GUARD(file);
    struct sink s;
    s.put = put_buffered;
    s.count = 0;
    s.file = file;
    s.failed = 0;
    s.local_used = 0;
    format(&s, f, ap);
    drain(&s);
    return s.failed || ferror(file) ? -1 : finish(&s);
}

int vdprintf(int fd, const char *f, va_list ap) {
    struct sink s;
    s.put = put_buffered;
    s.count = 0;
    s.file = 0;
    s.fd = fd;
    s.failed = 0;
    s.local_used = 0;
    format(&s, f, ap);
    drain(&s);
    return s.failed ? -1 : finish(&s);
}

int vprintf(const char *f, va_list ap) { return vfprintf(stdout, f, ap); }

#define VARIADIC(call)    \
    va_list ap;           \
    va_start(ap, format_string); \
    int r = call;         \
    va_end(ap);           \
    return r;

int printf(const char *format_string, ...) { VARIADIC(vfprintf(stdout, format_string, ap)) }
int fprintf(FILE *file, const char *format_string, ...) { VARIADIC(vfprintf(file, format_string, ap)) }
int sprintf(char *buffer, const char *format_string, ...) { VARIADIC(vsprintf(buffer, format_string, ap)) }
int snprintf(char *buffer, size_t size, const char *format_string, ...) { VARIADIC(vsnprintf(buffer, size, format_string, ap)) }
int dprintf(int fd, const char *format_string, ...) { VARIADIC(vdprintf(fd, format_string, ap)) }
int asprintf(char **out, const char *format_string, ...) { VARIADIC(vasprintf(out, format_string, ap)) }
