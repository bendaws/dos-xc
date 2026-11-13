/*
 * DOS-XC /openwork/zip.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "office.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "../os/pkg/dsp.h"

static unsigned get16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned long get32(const unsigned char *p) { return get16(p) | (unsigned long)get16(p + 2) << 16; }
static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, unsigned long v) { put16(p, (unsigned)(v & 0xffff)); put16(p + 2, (unsigned)(v >> 16)); }

char *read_whole(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = n >= 0 ? malloc((size_t)n + 1) : 0;
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); data = 0; }
    fclose(f);
    if (data) { data[n] = 0; if (size) *size = (size_t)n; }
    return data;
}

int write_whole(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(data, 1, size, f) == size;
    return fclose(f) || !ok ? -1 : 0;
}

const char *extension_of(const char *path) {
    const char *dot = strrchr(path, '.'), *slash = strrchr(path, '\\');
    return dot && (!slash || dot > slash) ? dot + 1 : "";
}

int zip_open(struct zip *z, const char *path) {
    memset(z, 0, sizeof(*z));
    z->data = (unsigned char *)read_whole(path, &z->size);
    if (!z->data || z->size < 22) { zip_close(z); return -1; }
    size_t end = z->size - 22;
    while (end > 0 && get32(z->data + end) != 0x06054b50) --end;
    if (get32(z->data + end) != 0x06054b50) { zip_close(z); return -1; }
    int count = (int)get16(z->data + end + 10);
    size_t at = get32(z->data + end + 16);
    z->entries = calloc((size_t)count + 1, sizeof(*z->entries));
    for (int i = 0; i < count && at + 46 <= z->size && get32(z->data + at) == 0x02014b50; ++i) {
        struct zip_entry *e = &z->entries[z->count];
        unsigned name_size = get16(z->data + at + 28), extra = get16(z->data + at + 30), comment = get16(z->data + at + 32);
        e->method = (int)get16(z->data + at + 10);
        e->packed = get32(z->data + at + 20);
        e->size = get32(z->data + at + 24);
        size_t local = get32(z->data + at + 42);
        unsigned n = name_size < sizeof(e->name) - 1 ? name_size : (unsigned)sizeof(e->name) - 1;
        memcpy(e->name, z->data + at + 46, n);
        e->name[n] = 0;
        if (local + 30 <= z->size) e->offset = local + 30 + get16(z->data + local + 26) + get16(z->data + local + 28);
        at += 46 + name_size + extra + comment;
        ++z->count;
    }
    return 0;
}

void zip_close(struct zip *z) {
    free(z->data);
    free(z->entries);
    memset(z, 0, sizeof(*z));
}

char *zip_read(const struct zip *z, const char *name, size_t *size) {
    for (int i = 0; i < z->count; ++i) {
        const struct zip_entry *e = &z->entries[i];
        if (strcasecmp(e->name, name) || e->offset + e->packed > z->size) continue;
        char *out = malloc(e->size + 1);
        if (!out) return 0;
        if (e->method == 0) memcpy(out, z->data + e->offset, e->size);
        else if (e->method != 8 || dsp_inflate(z->data + e->offset, e->packed, (unsigned char *)out, e->size) != (int)e->size) {
            free(out);
            return 0;
        }
        out[e->size] = 0;
        if (size) *size = e->size;
        return out;
    }
    return 0;
}

void zip_begin(struct zip_writer *w) { memset(w, 0, sizeof(*w)); }

static int reserve(struct zip_writer *w, size_t more) {
    if (w->used + more <= w->capacity_bytes) return 0;
    size_t capacity = w->capacity_bytes ? w->capacity_bytes : 65536;
    while (capacity < w->used + more) capacity *= 2;
    unsigned char *bigger = realloc(w->out, capacity);
    if (!bigger) return -1;
    w->out = bigger;
    w->capacity_bytes = capacity;
    return 0;
}

int zip_add(struct zip_writer *w, const char *name, const void *data, size_t size, int compress) {
    if (w->count == w->capacity) {
        int capacity = w->capacity ? w->capacity * 2 : 16;
        struct zip_entry *e = realloc(w->entries, sizeof(*e) * (size_t)capacity);
        unsigned long *c = realloc(w->crcs, sizeof(*c) * (size_t)capacity);
        if (!e || !c) return -1;
        w->entries = e;
        w->crcs = c;
        w->capacity = capacity;
    }
    unsigned char *packed = 0;
    size_t packed_size = size;
    if (compress && size > 64) {
        packed = malloc(size + 64);
        packed_size = packed ? dsp_deflate(data, size, packed, size + 64) : 0;
        if (!packed_size || packed_size >= size) { free(packed); packed = 0; packed_size = size; }
    }
    size_t n = strlen(name);
    if (reserve(w, 30 + n + packed_size)) { free(packed); return -1; }
    struct zip_entry *e = &w->entries[w->count];
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->method = packed ? 8 : 0;
    e->offset = w->used;
    e->packed = packed_size;
    e->size = size;
    w->crcs[w->count] = dsp_crc32(0, data, size);
    unsigned char *h = w->out + w->used;
    memset(h, 0, 30);
    put32(h, 0x04034b50);
    put16(h + 4, 20);
    put16(h + 8, (unsigned)e->method);
    put16(h + 12, 0x5821);
    put32(h + 14, w->crcs[w->count]);
    put32(h + 18, (unsigned long)packed_size);
    put32(h + 22, (unsigned long)size);
    put16(h + 26, (unsigned)n);
    memcpy(h + 30, name, n);
    memcpy(h + 30 + n, packed ? packed : data, packed_size);
    w->used += 30 + n + packed_size;
    free(packed);
    ++w->count;
    return 0;
}

int zip_finish(struct zip_writer *w, const char *path) {
    size_t directory = w->used;
    int r = 0;
    for (int i = 0; i < w->count && !r; ++i) {
        struct zip_entry *e = &w->entries[i];
        size_t n = strlen(e->name);
        if (reserve(w, 46 + n)) { r = -1; break; }
        unsigned char *h = w->out + w->used;
        memset(h, 0, 46);
        put32(h, 0x02014b50);
        put16(h + 4, 20);
        put16(h + 6, 20);
        put16(h + 10, (unsigned)e->method);
        put16(h + 14, 0x5821);
        put32(h + 16, w->crcs[i]);
        put32(h + 20, (unsigned long)e->packed);
        put32(h + 24, (unsigned long)e->size);
        put16(h + 28, (unsigned)n);
        put32(h + 42, (unsigned long)e->offset);
        memcpy(h + 46, e->name, n);
        w->used += 46 + n;
    }
    if (!r && !reserve(w, 22)) {
        unsigned char *h = w->out + w->used;
        memset(h, 0, 22);
        put32(h, 0x06054b50);
        put16(h + 8, (unsigned)w->count);
        put16(h + 10, (unsigned)w->count);
        put32(h + 12, (unsigned long)(w->used - directory));
        put32(h + 16, (unsigned long)directory);
        w->used += 22;
        r = write_whole(path, w->out, w->used);
    } else r = -1;
    free(w->out);
    free(w->entries);
    free(w->crcs);
    memset(w, 0, sizeof(*w));
    return r;
}

void text_add(struct text *t, const char *s, size_t n) {
    if (t->size + n + 1 > t->capacity) {
        size_t capacity = t->capacity ? t->capacity : 4096;
        while (capacity < t->size + n + 1) capacity *= 2;
        char *bigger = realloc(t->data, capacity);
        if (!bigger) return;
        t->data = bigger;
        t->capacity = capacity;
    }
    memcpy(t->data + t->size, s, n);
    t->size += n;
    t->data[t->size] = 0;
}

void text_put(struct text *t, const char *s) { text_add(t, s, strlen(s)); }

void text_printf(struct text *t, const char *format, ...) {
    char buffer[2048];
    va_list list;
    va_start(list, format);
    int n = vsnprintf(buffer, sizeof(buffer), format, list);
    va_end(list);
    if (n > 0) text_add(t, buffer, (size_t)n < sizeof(buffer) ? (size_t)n : sizeof(buffer) - 1);
}

void text_xml(struct text *t, const char *s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        switch (s[i]) {
        case '<': text_put(t, "&lt;"); break;
        case '>': text_put(t, "&gt;"); break;
        case '&': text_put(t, "&amp;"); break;
        case '"': text_put(t, "&quot;"); break;
        default: text_add(t, s + i, 1);
        }
    }
}

void text_utf8(struct text *t, const char *s, size_t n, int xml) {
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x80) {
            char two[2] = {(char)(0xc0 | c >> 6), (char)(0x80 | (c & 63))};
            text_add(t, two, 2);
        } else if (xml) text_xml(t, s + i, 1);
        else text_add(t, s + i, 1);
    }
}

void text_latin1(struct text *t, const char *s, size_t n) {
    const unsigned char *u = (const unsigned char *)s, *end = u + n;
    while (u < end) {
        unsigned code = *u++;
        if (code >= 0xc0) {
            int extra = code >= 0xf0 ? 3 : code >= 0xe0 ? 2 : 1;
            code &= 0x3f >> extra;
            for (int k = 0; k < extra && u < end && (*u & 0xc0) == 0x80; ++k) code = code << 6 | (*u++ & 0x3f);
        }
        if (code == 0x2018 || code == 0x2019) code = '\'';
        else if (code == 0x201c || code == 0x201d) code = '"';
        else if (code == 0x2013 || code == 0x2014) code = '-';
        else if (code == 0x2022) code = 0xb7;
        else if (code == 0x2026) code = '.';
        else if (code == 0x20ac) code = 'E';
        else if (code == 0xfeff || code == 0x200b) continue;
        char c = (char)(code < 256 ? code : '?');
        text_add(t, &c, 1);
    }
}
