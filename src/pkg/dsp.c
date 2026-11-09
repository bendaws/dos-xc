/*
 * DOS-XC /pkg/dsp.c - reading and writing DOS-XC packages
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#include "dsp.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static unsigned get16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned long get32(const unsigned char *p) {
    return p[0] | (unsigned long)p[1] << 8 | (unsigned long)p[2] << 16 | (unsigned long)p[3] << 24;
}
static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, unsigned long v) { for (int i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (i * 8)); }

unsigned long dsp_crc32(unsigned long crc, const void *data, size_t size) {
    static unsigned long table[256];
    if (!table[1])
        for (unsigned long n = 0; n < 256; ++n) {
            unsigned long c = n;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320ul ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
    const unsigned char *p = data;
    crc ^= 0xfffffffful;
    while (size--) crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return crc ^ 0xfffffffful;
}

/* ---- metadata ---------------------------------------------------------- */

int dsp_valid_name(const char *name) {
    if (!*name || strlen(name) > 40) return 0;
    for (const char *p = name; *p; ++p)
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.' && *p != ' ') return 0;
    return *name != '.' && *name != ' ' && name[strlen(name) - 1] != ' ';
}

static void field(char *out, size_t size, const char *value, size_t length) {
    if (length >= size) length = size - 1;
    memcpy(out, value, length);
    out[length] = 0;
}

int dsp_parse_info(const char *text, size_t length, struct dsp_info *info) {
    memset(info, 0, sizeof(*info));
    size_t at = 0;
    while (at < length) {
        size_t end = at;
        while (end < length && text[end] != '\n') ++end;
        size_t line_end = end;
        if (line_end > at && text[line_end - 1] == '\r') --line_end;
        const char *line = text + at;
        size_t n = line_end - at;
        const char *equals = memchr(line, '=', n);
        if (equals && *line != ';' && *line != '#') {
            size_t key = (size_t)(equals - line);
            const char *value = equals + 1;
            size_t value_length = n - key - 1;
#define KEY(k) (key == strlen(k) && !strncasecmp(line, k, key))
            if (KEY("Name")) field(info->name, sizeof(info->name), value, value_length);
            else if (KEY("Version")) field(info->version, sizeof(info->version), value, value_length);
            else if (KEY("Author")) field(info->author, sizeof(info->author), value, value_length);
            else if (KEY("Description")) field(info->description, sizeof(info->description), value, value_length);
            else if (KEY("Path")) field(info->path, sizeof(info->path), value, value_length);
            else if (KEY("StartIn")) field(info->start_in, sizeof(info->start_in), value, value_length);
            else if (KEY("Driver")) field(info->driver, sizeof(info->driver), value, value_length);
            else if (KEY("Env")) {
                size_t used = strlen(info->env);
                if (used + value_length + 2 < sizeof(info->env)) {
                    if (used) info->env[used++] = '\n';
                    field(info->env + used, sizeof(info->env) - used, value, value_length);
                }
            }
            else if (KEY("Arch")) {
                char number[8];
                field(number, sizeof(number), value, value_length);
                info->arch = atoi(number);
            }
#undef KEY
        }
        at = end + 1;
    }
    if (!dsp_valid_name(info->name) || !info->version[0] || (info->arch != 16 && info->arch != 32 && info->arch != 64))
        return -1;
    return 0;
}

int dsp_format_info(const struct dsp_info *info, char *out, size_t size) {
    int n = snprintf(out, size, "Name=%s\r\nVersion=%s\r\nAuthor=%s\r\nDescription=%s\r\nArch=%d\r\nPath=%s\r\n%s%s%s",
                     info->name, info->version, info->author, info->description, info->arch, info->path,
                     info->start_in[0] ? "StartIn=" : "", info->start_in, info->start_in[0] ? "\r\n" : "");
    if (n < 0 || (size_t)n >= size) return -1;
    if (info->driver[0]) {
        int m = snprintf(out + n, size - (size_t)n, "Driver=%s\r\n", info->driver);
        if (m < 0 || (size_t)(n + m) >= size) return -1;
        n += m;
    }
    for (const char *p = info->env; *p;) {
        size_t length = strcspn(p, "\n");
        int m = snprintf(out + n, size - (size_t)n, "Env=%.*s\r\n", (int)length, p);
        if (m < 0 || (size_t)(n + m) >= size) return -1;
        n += m;
        p += length;
        if (*p) ++p;
    }
    return n;
}

/* Numeric parts compare as numbers: 1.10 > 1.9.  Returns <0, 0 or >0. */
int dsp_compare_versions(const char *a, const char *b) {
    while (*a || *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            unsigned long x = strtoul(a, (char **)&a, 10), y = strtoul(b, (char **)&b, 10);
            if (x != y) return x < y ? -1 : 1;
            continue;
        }
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y) return x < y ? -1 : 1;
        if (*a) ++a;
        if (*b) ++b;
    }
    return 0;
}

/* ---- reading ------------------------------------------------------------ */

static int fail(char *error, size_t size, const char *message) {
    if (error && size) snprintf(error, size, "%s", message);
    return -1;
}

static int read_header(FILE *f, struct dsp_info *info, unsigned long *zip_start, char *error, size_t error_size) {
    unsigned char head[12];
    if (fread(head, 1, 12, f) != 12 || memcmp(head, DSP_MAGIC, 4)) return fail(error, error_size, "not a DOS-XC package (.dsp)");
    if (get16(head + 4) != DSP_VERSION) return fail(error, error_size, "made for a newer package format");
    unsigned long length = get32(head + 8);
    if (length > 65536) return fail(error, error_size, "damaged package header");
    char *text = malloc(length + 1);
    if (!text) return fail(error, error_size, "out of memory");
    if (fread(text, 1, length, f) != length) { free(text); return fail(error, error_size, "damaged package header"); }
    int r = dsp_parse_info(text, length, info);
    free(text);
    if (r) return fail(error, error_size, "the package information is incomplete (Name, Version and Arch are required)");
    *zip_start = 12 + length;
    return 0;
}

int dsp_read_info(const char *path, struct dsp_info *info, char *error, size_t error_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return fail(error, error_size, "cannot open the file");
    unsigned long start;
    int r = read_header(f, info, &start, error, error_size);
    fclose(f);
    return r;
}

int dsp_open(struct dsp *package, const char *path, char *error, size_t error_size) {
    memset(package, 0, sizeof(*package));
    FILE *f = fopen(path, "rb");
    if (!f) return fail(error, error_size, "cannot open the file");
    unsigned long zip_start;
    if (read_header(f, &package->info, &zip_start, error, error_size)) { fclose(f); return -1; }
    /* The end-of-central-directory record is in the last 64 KiB + 22 bytes. */
    if (fseek(f, 0, SEEK_END)) { fclose(f); return fail(error, error_size, "cannot read the file"); }
    long end = ftell(f);
    long tail_size = end < 65558 ? end : 65558;
    unsigned char *tail = malloc((size_t)tail_size);
    if (!tail || fseek(f, end - tail_size, SEEK_SET) || fread(tail, 1, (size_t)tail_size, f) != (size_t)tail_size) {
        free(tail);
        fclose(f);
        return fail(error, error_size, "cannot read the file");
    }
    long eocd = -1;
    for (long i = tail_size - 22; i >= 0; --i)
        if (get32(tail + i) == 0x06054b50ul) { eocd = i; break; }
    if (eocd < 0) { free(tail); fclose(f); return fail(error, error_size, "the package archive is damaged"); }
    unsigned count = get16(tail + eocd + 10);
    unsigned long directory_size = get32(tail + eocd + 12), directory = get32(tail + eocd + 16);
    free(tail);
    unsigned char probe[4];
    unsigned long base = 0;                       /* offsets are absolute; accept relative ones too */
    if (fseek(f, (long)directory, SEEK_SET) || fread(probe, 1, 4, f) != 4 || get32(probe) != 0x02014b50ul) base = zip_start;
    unsigned char *central = malloc(directory_size ? directory_size : 1);
    package->entries = calloc(count ? count : 1, sizeof(*package->entries));
    if (!central || !package->entries || fseek(f, (long)(base + directory), SEEK_SET) ||
        fread(central, 1, directory_size, f) != directory_size) {
        free(central);
        dsp_close(package);
        fclose(f);
        return fail(error, error_size, "the package archive is damaged");
    }
    size_t at = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (at + 46 > directory_size || get32(central + at) != 0x02014b50ul) break;
        struct dsp_entry *e = &package->entries[package->count];
        unsigned name_length = get16(central + at + 28), extra = get16(central + at + 30), comment = get16(central + at + 32);
        if (at + 46 + name_length > directory_size) break;
        e->method = get16(central + at + 10);
        e->crc = get32(central + at + 16);
        e->compressed = get32(central + at + 20);
        e->size = get32(central + at + 24);
        e->offset = base + get32(central + at + 42);
        field(e->name, sizeof(e->name), (const char *)central + at + 46, name_length);
        for (char *p = e->name; *p; ++p) if (*p == '\\') *p = '/';
        at += 46 + name_length + extra + comment;
        /* Skip unsafe names (absolute, "..", drive letters). */
        if (e->name[0] == '/' || strstr(e->name, "..") || strchr(e->name, ':')) continue;
        ++package->count;
    }
    free(central);
    package->file = f;
    return 0;
}

int dsp_read_entry(struct dsp *package, unsigned index, unsigned char **data, size_t *size) {
    struct dsp_entry *e = &package->entries[index];
    unsigned char local[30];
    *data = 0;
    *size = 0;
    if (fseek(package->file, (long)e->offset, SEEK_SET) || fread(local, 1, 30, package->file) != 30 ||
        get32(local) != 0x04034b50ul)
        return -1;
    long skip = (long)get16(local + 26) + get16(local + 28);
    if (fseek(package->file, skip, SEEK_CUR)) return -1;
    unsigned char *out = malloc(e->size ? e->size : 1);
    if (!out) return -1;
    if (e->method == 0) {
        if (e->compressed != e->size || fread(out, 1, e->size, package->file) != e->size) { free(out); return -1; }
    } else if (e->method == 8) {
        unsigned char *in = malloc(e->compressed ? e->compressed : 1);
        if (!in || fread(in, 1, e->compressed, package->file) != e->compressed ||
            dsp_inflate(in, e->compressed, out, e->size) != (int)e->size) {
            free(in);
            free(out);
            return -1;
        }
        free(in);
    } else {
        free(out);
        return -1;
    }
    if (dsp_crc32(0, out, e->size) != e->crc) { free(out); return -1; }
    *data = out;
    *size = e->size;
    return 0;
}

void dsp_close(struct dsp *package) {
    if (package->file) fclose(package->file);
    free(package->entries);
    package->file = 0;
    package->entries = 0;
    package->count = 0;
}

/* ---- writing ------------------------------------------------------------ */

struct dsp_writer {
    FILE *file;
    unsigned char *directory;
    size_t directory_size, directory_capacity;
    unsigned count;
    int failed;
};

struct dsp_writer *dsp_create(const char *path, const struct dsp_info *info) {
    char text[1024];
    int length = dsp_format_info(info, text, sizeof(text));
    if (length < 0) return 0;
    struct dsp_writer *w = calloc(1, sizeof(*w));
    if (!w) return 0;
    w->file = fopen(path, "wb");
    if (!w->file) { free(w); return 0; }
    unsigned char head[12];
    memcpy(head, DSP_MAGIC, 4);
    put16(head + 4, DSP_VERSION);
    put16(head + 6, 0);
    put32(head + 8, (unsigned long)length);
    if (fwrite(head, 1, 12, w->file) != 12 || fwrite(text, 1, (size_t)length, w->file) != (size_t)length) w->failed = 1;
    return w;
}

int dsp_add(struct dsp_writer *w, const char *name, const void *data, size_t size) {
    size_t name_length = strlen(name);
    unsigned long offset = (unsigned long)ftell(w->file), crc = dsp_crc32(0, data, size);
    unsigned char *packed = malloc(size + 64);
    size_t packed_size = packed && size ? dsp_deflate(data, size, packed, size) : 0;
    unsigned method = packed_size ? 8 : 0;
    const void *body = method ? packed : data;
    size_t body_size = method ? packed_size : size;
    unsigned char local[30] = {0};
    put32(local, 0x04034b50ul);
    put16(local + 4, 20);
    put16(local + 8, method);
    put16(local + 12, (2026 - 1980) << 9 | 1 << 5 | 1);
    put32(local + 14, crc);
    put32(local + 18, (unsigned long)body_size);
    put32(local + 22, (unsigned long)size);
    put16(local + 26, (unsigned)name_length);
    if (fwrite(local, 1, 30, w->file) != 30 || fwrite(name, 1, name_length, w->file) != name_length ||
        fwrite(body, 1, body_size, w->file) != body_size)
        w->failed = 1;
    free(packed);
    if (w->directory_size + 46 + name_length > w->directory_capacity) {
        size_t capacity = (w->directory_capacity + 46 + name_length) * 2;
        unsigned char *bigger = realloc(w->directory, capacity);
        if (!bigger) { w->failed = 1; return -1; }
        w->directory = bigger;
        w->directory_capacity = capacity;
    }
    unsigned char *c = w->directory + w->directory_size;
    memset(c, 0, 46);
    put32(c, 0x02014b50ul);
    put16(c + 4, 20);
    put16(c + 6, 20);
    put16(c + 10, method);
    put16(c + 14, (2026 - 1980) << 9 | 1 << 5 | 1);
    put32(c + 16, crc);
    put32(c + 20, (unsigned long)body_size);
    put32(c + 24, (unsigned long)size);
    put16(c + 28, (unsigned)name_length);
    put32(c + 42, offset);
    memcpy(c + 46, name, name_length);
    w->directory_size += 46 + name_length;
    ++w->count;
    return w->failed ? -1 : 0;
}

int dsp_finish(struct dsp_writer *w) {
    unsigned long start = (unsigned long)ftell(w->file);
    unsigned char end[22] = {0};
    put32(end, 0x06054b50ul);
    put16(end + 8, w->count);
    put16(end + 10, w->count);
    put32(end + 12, (unsigned long)w->directory_size);
    put32(end + 16, start);
    if (fwrite(w->directory, 1, w->directory_size, w->file) != w->directory_size || fwrite(end, 1, 22, w->file) != 22)
        w->failed = 1;
    if (fclose(w->file)) w->failed = 1;
    int failed = w->failed;
    free(w->directory);
    free(w);
    return failed ? -1 : 0;
}
