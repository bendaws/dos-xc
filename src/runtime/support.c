/*
 * DOS-XC /runtime/support.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * Freestanding helpers for the program runtime.
 */

#include "runtime.h"

void *memcpy(void *destination, const void *source, __SIZE_TYPE__ size) {
    void *result = destination;
    __asm__ volatile("rep movsb" : "+D"(destination), "+S"(source), "+c"(size) : : "memory");
    return result;
}

void *memset(void *destination, int value, __SIZE_TYPE__ size) {
    void *result = destination;
    __asm__ volatile("rep stosb" : "+D"(destination), "+c"(size) : "a"(value) : "memory");
    return result;
}

void *memmove(void *destination, const void *source, __SIZE_TYPE__ size) {
    unsigned char *d = destination;
    const unsigned char *s = source;
    if (d <= s || d >= s + size) return memcpy(destination, source, size);
    while (size--) d[size] = s[size];
    return destination;
}

int memcmp(const void *left, const void *right, __SIZE_TYPE__ size) {
    const unsigned char *a = left, *b = right;
    for (__SIZE_TYPE__ i = 0; i < size; ++i)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

uptr rt_strlen(const char *text) {
    uptr n = 0;
    while (text[n]) ++n;
    return n;
}

void rt_strcpy(char *destination, const char *source) {
    while ((*destination++ = *source++)) {
    }
}

int rt_upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

int rt_equal_nocase(const char *a, const char *b) {
    while (*a && *b)
        if (rt_upper((unsigned char)*a++) != rt_upper((unsigned char)*b++)) return 0;
    return !*a && !*b;
}

void rt_print(const char *text) {
    if (current && current->vc) terminal_write((const u8 *)text, rt_strlen(text));
    else ctx->write(text);
}

void rt_print_hex(u64 value) {
    char buffer[19];
    buffer[0] = '0';
    buffer[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        unsigned digit = (unsigned)(value >> ((15 - i) * 4)) & 15;
        buffer[2 + i] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
    }
    buffer[18] = 0;
    rt_print(buffer);
}

void rt_print_decimal(u64 value) {
    char buffer[24];
    int at = 23;
    buffer[at] = 0;
    do {
        u64 quotient = value / 10;
        buffer[--at] = (char)('0' + (value - quotient * 10));
        value = quotient;
    } while (value);
    rt_print(buffer + at);
}

/* Seconds since 1970-01-01 for a civil date (no time zone). */
i64 rt_epoch(unsigned year, unsigned month, unsigned day, unsigned hour, unsigned minute, unsigned second) {
    static const unsigned before[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31) return 0;
    i64 days = (i64)(year - 1970) * 365 + (year - 1969) / 4 - (year - 1901) / 100 + (year - 1601) / 400;
    days += before[month - 1] + day - 1;
    if (month > 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ++days;
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

#if __SIZEOF_POINTER__ == 4
/* 64-bit division for the i386 build (normally supplied by libgcc). */
static u64 divide(u64 numerator, u64 denominator, u64 *remainder) {
    u64 quotient = 0, rest = 0;
    if (!denominator) return 0;
    for (int bit = 63; bit >= 0; --bit) {
        rest = (rest << 1) | ((numerator >> bit) & 1);
        if (rest >= denominator) {
            rest -= denominator;
            quotient |= 1ULL << bit;
        }
    }
    if (remainder) *remainder = rest;
    return quotient;
}
u64 __udivdi3(u64 a, u64 b) { return divide(a, b, 0); }
u64 __umoddi3(u64 a, u64 b) { u64 r = 0; divide(a, b, &r); return r; }
i64 __divdi3(i64 a, i64 b) {
    int negative = (a < 0) != (b < 0);
    u64 q = divide(a < 0 ? -(u64)a : (u64)a, b < 0 ? -(u64)b : (u64)b, 0);
    return negative ? -(i64)q : (i64)q;
}
i64 __moddi3(i64 a, i64 b) {
    u64 r = 0;
    divide(a < 0 ? -(u64)a : (u64)a, b < 0 ? -(u64)b : (u64)b, &r);
    return a < 0 ? -(i64)r : (i64)r;
}
#endif

int rt_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) ++a, ++b;
    return (int)(u8)*a - (int)(u8)*b;
}
