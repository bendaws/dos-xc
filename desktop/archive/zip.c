/*
 * DOS-XC /desktop/archive/zip.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "arc.h"
#include <stdlib.h>
#include <string.h>

static int find_end(struct job *job, FILE *f, u64 size, u64 *cd_offset, u64 *cd_size, u64 *count, u64 *adjust) {
    size_t tail = size < 65557 ? (size_t)size : 65557;
    u8 *buffer = malloc(tail);
    if (!buffer) return job_fail(job, "Out of memory.");
    if (arc_seek(f, size - tail) || fread(buffer, 1, tail, f) != tail) { free(buffer); return job_fail(job, "The archive could not be read."); }
    long at = -1;
    for (long i = (long)tail - 22; i >= 0; --i)
        if (!memcmp(buffer + i, "PK\5\6", 4)) { at = i; break; }
    if (at < 0) { free(buffer); return job_fail(job, "The ZIP archive is damaged (its directory is missing)."); }
    const u8 *e = buffer + at;
    u64 end_pos = size - tail + (u64)at;
    *count = get16(e + 10);
    *cd_size = get32(e + 12);
    *cd_offset = get32(e + 16);
    if (at >= 20 && !memcmp(e - 20, "PK\6\7", 4)) {
        u64 z = get64(e - 20 + 8);
        u8 z64[56];
        if (!arc_seek(f, z) && fread(z64, 1, 56, f) == 56 && !memcmp(z64, "PK\6\6", 4)) {
            *count = get64(z64 + 32);
            *cd_size = get64(z64 + 40);
            *cd_offset = get64(z64 + 48);
            end_pos = z;
        }
    }
    free(buffer);
    *adjust = end_pos > *cd_offset + *cd_size ? end_pos - (*cd_offset + *cd_size) : 0;
    if (*cd_offset + *adjust + *cd_size > size) return job_fail(job, "The ZIP archive is damaged.");
    return 0;
}

static const char *method_name(unsigned method) {
    switch (method) {
    case 1: return "Shrink";
    case 6: return "Implode";
    case 9: return "Deflate64";
    case 12: return "BZip2";
    case 93: return "Zstandard";
    case 98: return "PPMd";
    }
    return 0;
}

static int extract_entry(struct job *job, FILE *f, const u8 *c, const char *name, u64 adjust, u64 file_size) {
    unsigned flags = get16(c + 8), method = get16(c + 10);
    u32 crc = get32(c + 16);
    u64 packed = get32(c + 20), size = get32(c + 24), offset = get32(c + 42);
    unsigned name_len = get16(c + 28), extra_len = get16(c + 30);
    u32 attributes = get32(c + 38);
    const u8 *x = c + 46 + name_len, *x_end = x + extra_len;
    while (x + 4 <= x_end) {
        unsigned id = get16(x), n = get16(x + 2);
        const u8 *v = x + 4;
        if (id == 1) {
            if (size == 0xffffffffu && v + 8 <= x + 4 + n) { size = get64(v); v += 8; }
            if (packed == 0xffffffffu && v + 8 <= x + 4 + n) { packed = get64(v); v += 8; }
            if (offset == 0xffffffffu && v + 8 <= x + 4 + n) { offset = get64(v); v += 8; }
        }
        x += 4 + n;
    }
    size_t len = strlen(name);
    int directory = (len && (name[len - 1] == '/' || name[len - 1] == '\\')) || (attributes & 0x10 && !size);
    long long mtime = from_dos_time(get16(c + 14), get16(c + 12));
    if (directory) return out_begin(job, name, 1, mtime) < 0 ? -1 : 0;
    if (flags & 1 || method == 99) return job_fail(job, "\"%s\" is protected with a password, which DOS-XC cannot open yet.", name);
    if (method != 0 && method != 8 && method != 14 && method != 95) {
        const char *m = method_name(method);
        return m ? job_fail(job, "\"%s\" uses %s compression, which DOS-XC cannot unpack.", name, m)
                 : job_fail(job, "\"%s\" uses compression method %u, which DOS-XC cannot unpack.", name, method);
    }
    u8 local[30];
    if (arc_seek(f, offset + adjust) || fread(local, 1, 30, f) != 30 || memcmp(local, "PK\3\4", 4)) return job_fail(job, "The ZIP archive is damaged at \"%s\".", name);
    u64 data = offset + adjust + 30 + get16(local + 26) + get16(local + 28);
    if (data + packed > file_size) return job_fail(job, "The ZIP archive is damaged at \"%s\".", name);
    int r = out_begin(job, name, 0, mtime);
    if (r) return r < 0 ? -1 : 0;
    struct src *in = malloc(sizeof(*in));
    if (!in) return job_fail(job, "Out of memory.");
    src_open(in, f, data, packed, job);
    struct sink *out = out_sink(job);
    if (method == 0) {
        while (!r && src_fill(in)) r = out->write(out, in->buf, in->len);
        if (in->error) r = -1;
    } else if (method == 8) {
        r = inflate_stream(in, out, job, 0, 0);
    } else if (method == 95) {
        r = xz_decode(in, out, job);
    } else {
        u8 head[9];
        if (src_read(in, head, 9) != 9 || get16(head + 2) != 5) r = job_fail(job, "The LZMA data of \"%s\" is damaged.", name);
        else r = lzma_decode(in, head + 4, size, out, job);
    }
    free(in);
    if (r) return -1;
    if (job->written != size || job->crc != crc) return job_fail(job, "\"%s\" is damaged (its check does not match).", name);
    return out_end(job);
}

int zip_extract(struct job *job, FILE *f) {
    u64 size = arc_file_size(f), cd_offset = 0, cd_size = 0, count = 0, adjust = 0;
    if (find_end(job, f, size, &cd_offset, &cd_size, &count, &adjust)) return -1;
    if (cd_size > (64u << 20)) return job_fail(job, "The ZIP archive's directory is too large.");
    u8 *cd = malloc((size_t)cd_size + 1);
    if (!cd) return job_fail(job, "Out of memory.");
    if (arc_seek(f, cd_offset + adjust) || fread(cd, 1, (size_t)cd_size, f) != cd_size) { free(cd); return job_fail(job, "The archive could not be read."); }
    int r = 0;
    size_t at = 0;
    for (u64 i = 0; i < count && !r; ++i) {
        if (at + 46 > cd_size || memcmp(cd + at, "PK\1\2", 4)) { r = job_fail(job, "The ZIP archive's directory is damaged."); break; }
        const u8 *c = cd + at;
        size_t name_len = get16(c + 28), entry = 46 + name_len + get16(c + 30) + get16(c + 32);
        if (at + entry > cd_size) { r = job_fail(job, "The ZIP archive's directory is damaged."); break; }
        char name[1024];
        size_t n = name_len < sizeof(name) - 1 ? name_len : sizeof(name) - 1;
        memcpy(name, c + 46, n);
        name[n] = 0;
        r = extract_entry(job, f, c, name, adjust, size);
        at += entry;
    }
    free(cd);
    return r;
}

struct zip_entry { u64 offset, packed, size; u32 crc; u16 date, time, method; int directory; };

static int deflate_feed(void *context, const u8 *data, size_t n) { return deflate_write(context, data, n); }

static int ascii(const char *s) {
    for (; *s; ++s)
        if ((unsigned char)*s >= 128) return 0;
    return 1;
}

int zip_create(struct job *job, FILE *f, struct arc_item *items, int n) {
    struct zip_entry *e = calloc((size_t)n + 1, sizeof(*e));
    if (!e) return job_fail(job, "Out of memory.");
    struct fsink file;
    fsink_init(&file, f);
    int r = 0;
    for (int i = 0; i < n && !r; ++i) {
        struct arc_item *it = &items[i];
        char name[700];
        snprintf(name, sizeof(name), "%s%s", it->name, it->directory ? "/" : "");
        size_t len = strlen(name);
        e[i].offset = file.count;
        e[i].directory = it->directory;
        e[i].method = it->directory || !it->size ? 0 : 8;
        e[i].size = it->directory ? 0 : it->size;
        dos_time(it->mtime, &e[i].date, &e[i].time);
        u8 h[30];
        memset(h, 0, sizeof(h));
        memcpy(h, "PK\3\4", 4);
        put16(h + 4, 20);
        put16(h + 6, ascii(name) ? 0 : 0x800);
        put16(h + 8, e[i].method);
        put16(h + 10, e[i].time);
        put16(h + 12, e[i].date);
        put16(h + 26, (u32)len);
        if (file.sink.write(&file.sink, h, 30) || file.sink.write(&file.sink, (const u8 *)name, len)) { r = -1; break; }
        if (!e[i].method) continue;
        u64 data_start = file.count;
        struct deflater *d = deflate_new(&file.sink);
        if (!d) { r = job_fail(job, "Out of memory."); break; }
        if (arc_feed(job, it, deflate_feed, d, &e[i].crc)) { deflate_free(d); r = -1; break; }
        if (deflate_finish(d)) { r = -1; break; }
        e[i].packed = file.count - data_start;
        if (e[i].packed > 0xfffffffeu || e[i].size > 0xfffffffeu || file.count > 0xfffffffeu) { r = job_fail(job, "It is too large for a ZIP archive; try 7z."); break; }
        u8 patch[12];
        put32(patch, e[i].crc);
        put32(patch + 4, (u32)e[i].packed);
        put32(patch + 8, (u32)e[i].size);
        if (arc_seek(f, e[i].offset + 14) || fwrite(patch, 1, 12, f) != 12 || fseek(f, 0, SEEK_END)) { r = -1; break; }
    }
    u64 cd_start = file.count;
    for (int i = 0; i < n && !r; ++i) {
        char name[700];
        snprintf(name, sizeof(name), "%s%s", items[i].name, items[i].directory ? "/" : "");
        size_t len = strlen(name);
        u8 c[46];
        memset(c, 0, sizeof(c));
        memcpy(c, "PK\1\2", 4);
        put16(c + 4, 20);
        put16(c + 6, 20);
        put16(c + 8, ascii(name) ? 0 : 0x800);
        put16(c + 10, e[i].method);
        put16(c + 12, e[i].time);
        put16(c + 14, e[i].date);
        put32(c + 16, e[i].crc);
        put32(c + 20, (u32)e[i].packed);
        put32(c + 24, (u32)e[i].size);
        put16(c + 28, (u32)len);
        put32(c + 38, e[i].directory ? 0x10 : 0x20);
        put32(c + 42, (u32)e[i].offset);
        if (file.sink.write(&file.sink, c, 46) || file.sink.write(&file.sink, (const u8 *)name, len)) r = -1;
    }
    if (!r) {
        if (n > 0xfffe || file.count > 0xfffffffeu) r = job_fail(job, "It is too large for a ZIP archive; try 7z.");
        u8 end[22];
        memset(end, 0, sizeof(end));
        memcpy(end, "PK\5\6", 4);
        put16(end + 8, (u32)n);
        put16(end + 10, (u32)n);
        put32(end + 12, (u32)(file.count - cd_start));
        put32(end + 16, (u32)cd_start);
        if (!r && file.sink.write(&file.sink, end, 22)) r = -1;
    }
    free(e);
    if (r || file.error) return job_fail(job, "The archive could not be written (is the disk full?).");
    return 0;
}
