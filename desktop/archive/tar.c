/*
 * DOS-XC /desktop/archive/tar.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "arc.h"
#include <stdlib.h>
#include <string.h>

struct crc_sink { struct sink sink, *next; u32 crc, size; };

static int crc_write(struct sink *s, const u8 *data, size_t n) {
    struct crc_sink *c = (struct crc_sink *)s;
    c->crc = crc32_update(c->crc, data, n);
    c->size += (u32)n;
    return c->next->write(c->next, data, n);
}

static int gunzip(struct src *in, struct sink *out, struct job *job, long long *mtime) {
    for (int member = 0;; ++member) {
        int b = src_byte(in);
        if (b < 0 && member) return in->error ? -1 : 0;
        while (b == 0 && member) b = src_byte(in);
        if (b < 0 && member) return 0;
        u8 h[10];
        h[0] = (u8)b;
        if (b != 0x1f || src_read(in, h + 1, 9) != 9 || h[1] != 0x8b || h[2] != 8) {
            if (member) return 0;
            return job_fail(job, "This is not gzip data.");
        }
        if (mtime && !member) *mtime = get32(h + 4);
        int flags = h[3];
        if (flags & 4) {
            u8 x[2];
            if (src_read(in, x, 2) != 2 || src_skip(in, get16(x))) return job_fail(job, "The gzip header is damaged.");
        }
        for (int field = 8; field <= 16; field <<= 1)
            if (flags & field) {
                int c;
                while ((c = src_byte(in)) > 0) {}
                if (c < 0) return job_fail(job, "The gzip header is damaged.");
            }
        if ((flags & 2) && src_skip(in, 2)) return job_fail(job, "The gzip header is damaged.");
        struct crc_sink c = {{crc_write}, out, 0, 0};
        u8 tail[8];
        int have;
        if (inflate_stream(in, &c.sink, job, tail, &have)) return -1;
        if (src_read(in, tail + have, (size_t)(8 - have)) != (size_t)(8 - have)) return job_fail(job, "The gzip data ends too soon.");
        if (get32(tail) != c.crc || get32(tail + 4) != c.size) return job_fail(job, "The gzip data is damaged (its check does not match).");
    }
}

enum { T_HEADER, T_DATA, T_SKIP, T_LONGNAME, T_PAX, T_DONE };

struct tar {
    struct sink sink;
    struct job *job;
    u8 header[512];
    size_t have;
    int mode, writing, bad;
    u64 left, pad;
    char *text;                       /* a long name or pax records being read */
    size_t text_n, text_cap;
    char long_name[1024], pax_name[1024];
    u64 pax_size;
    int pax_has_size;
};

static u64 number(const u8 *p, int n) {
    if (p[0] & 0x80) {
        u64 v = p[0] & 0x3f;
        for (int i = 1; i < n; ++i) v = v << 8 | p[i];
        return v;
    }
    u64 v = 0;
    int i = 0;
    while (i < n && (p[i] == ' ' || !p[i])) ++i;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; ++i) v = v << 3 | (u64)(p[i] - '0');
    return v;
}

static int checksum_ok(const u8 *h) {
    u32 unsigned_sum = 0;
    int signed_sum = 0;
    for (int i = 0; i < 512; ++i) {
        u8 c = i >= 148 && i < 156 ? ' ' : h[i];
        unsigned_sum += c;
        signed_sum += (signed char)c;
    }
    u64 stored = number(h + 148, 8);
    return stored == unsigned_sum || (long long)stored == signed_sum;
}

static void parse_pax(struct tar *t) {
    char *p = t->text, *end = t->text + t->text_n;
    while (p < end) {
        char *space = memchr(p, ' ', (size_t)(end - p));
        if (!space) break;
        long len = strtol(p, 0, 10);
        if (len <= 0 || p + len > end) break;
        char *key = space + 1, *record_end = p + len - 1;
        char *eq = memchr(key, '=', (size_t)(record_end - key));
        if (eq) {
            size_t klen = (size_t)(eq - key), vlen = (size_t)(record_end - eq - 1);
            if (klen == 4 && !memcmp(key, "path", 4) && vlen < sizeof(t->pax_name)) {
                memcpy(t->pax_name, eq + 1, vlen);
                t->pax_name[vlen] = 0;
            } else if (klen == 4 && !memcmp(key, "size", 4)) {
                t->pax_size = strtoull(eq + 1, 0, 10);
                t->pax_has_size = 1;
            }
        }
        p += len;
    }
}

static int header(struct tar *t) {
    const u8 *h = t->header;
    int empty = 1;
    for (int i = 0; i < 512 && empty; ++i) empty = !h[i];
    if (empty) { t->mode = T_DONE; return 0; }
    if (!checksum_ok(h)) return job_fail(t->job, "The tar data is damaged.");
    u64 size = number(h + 124, 12);
    char type = (char)h[156];
    if (type == 'x' || type == 'g' || type == 'L' || type == 'K') {
        t->left = size;
        t->pad = (512 - size % 512) % 512;
        t->text_n = 0;
        int keep = type == 'x' || type == 'L';
        if (keep && size > 65536) keep = 0;
        if (keep && size + 1 > t->text_cap) {
            char *grown = realloc(t->text, (size_t)size + 1);
            if (!grown) return job_fail(t->job, "Out of memory.");
            t->text = grown;
            t->text_cap = (size_t)size + 1;
        }
        t->mode = !keep ? T_SKIP : type == 'L' ? T_LONGNAME : T_PAX;
        if (!t->left) t->mode = T_SKIP;
        return 0;
    }
    char name[1024];
    if (t->pax_name[0]) snprintf(name, sizeof(name), "%s", t->pax_name);
    else if (t->long_name[0]) snprintf(name, sizeof(name), "%s", t->long_name);
    else if (!memcmp(h + 257, "ustar", 5) && h[345]) snprintf(name, sizeof(name), "%.155s/%.100s", (const char *)h + 345, (const char *)h);
    else snprintf(name, sizeof(name), "%.100s", (const char *)h);
    if (t->pax_has_size) size = t->pax_size;
    t->pax_name[0] = t->long_name[0] = 0;
    t->pax_has_size = 0;
    t->left = size;
    t->pad = (512 - size % 512) % 512;
    long long mtime = (long long)number(h + 136, 12);
    size_t len = strlen(name);
    int directory = type == '5' || (len && name[len - 1] == '/' && (type == '0' || !type));
    t->writing = 0;
    if (directory) {
        if (out_begin(t->job, name, 1, mtime) < 0) return -1;
        t->mode = T_SKIP;
    } else if (type == '0' || type == 0 || type == '7') {
        int r = out_begin(t->job, name, 0, mtime);
        if (r < 0) return -1;
        t->writing = !r;
        t->mode = T_DATA;
    } else {
        t->mode = T_SKIP;
    }
    if (!t->left && t->mode == T_DATA) {
        if (t->writing && out_end(t->job)) return -1;
        t->writing = 0;
        t->mode = T_SKIP;
    }
    return 0;
}

static int tar_write(struct sink *s, const u8 *data, size_t n) {
    struct tar *t = (struct tar *)s;
    while (n) {
        if (t->bad) return -1;
        switch (t->mode) {
        case T_DONE:
            return 0;
        case T_HEADER: {
            size_t k = 512 - t->have < n ? 512 - t->have : n;
            memcpy(t->header + t->have, data, k);
            t->have += k;
            data += k;
            n -= k;
            if (t->have == 512) {
                t->have = 0;
                if (header(t)) { t->bad = 1; return -1; }
            }
            break;
        }
        default: {
            if (!t->left) {
                if (!t->pad) {
                    if (t->mode == T_LONGNAME) {
                        t->text[t->text_n] = 0;
                        snprintf(t->long_name, sizeof(t->long_name), "%s", t->text);
                    } else if (t->mode == T_PAX) parse_pax(t);
                    t->mode = T_HEADER;
                    break;
                }
                size_t k = t->pad < n ? (size_t)t->pad : n;
                t->pad -= k;
                data += k;
                n -= k;
                break;
            }
            size_t k = t->left < n ? (size_t)t->left : n;
            if (t->mode == T_DATA && t->writing && out_write(t->job, data, k)) { t->bad = 1; return -1; }
            if (t->mode == T_LONGNAME || t->mode == T_PAX) { memcpy(t->text + t->text_n, data, k); t->text_n += k; }
            t->left -= k;
            data += k;
            n -= k;
            if (!t->left && t->mode == T_DATA) {
                if (t->writing && out_end(t->job)) { t->bad = 1; return -1; }
                t->writing = 0;
                t->mode = T_SKIP;
            }
        }
        }
    }
    if (t->mode != T_HEADER && t->mode != T_DONE && !t->left && !t->pad) {
        if (t->mode == T_LONGNAME) { t->text[t->text_n] = 0; snprintf(t->long_name, sizeof(t->long_name), "%s", t->text); }
        else if (t->mode == T_PAX) parse_pax(t);
        t->mode = T_HEADER;
    }
    return 0;
}

int tar_extract(struct job *job, FILE *f, int format) {
    struct tar *t = calloc(1, sizeof(*t));
    struct src *in = malloc(sizeof(*in));
    if (!t || !in) { free(t); free(in); return job_fail(job, "Out of memory."); }
    t->sink.write = tar_write;
    t->job = job;
    src_open(in, f, 0, ~0ull, job);
    int r;
    if (format == ARC_TAR_GZ) r = gunzip(in, &t->sink, job, 0);
    else if (format == ARC_TAR_XZ) r = xz_decode(in, &t->sink, job);
    else {
        r = 0;
        while (!r && src_fill(in)) r = tar_write(&t->sink, in->buf, in->len);
        if (in->error) r = -1;
    }
    if (!r && t->mode != T_DONE && (t->mode != T_HEADER || t->have)) r = job_fail(job, "The tar data ends too soon.");
    if (!r && t->writing) r = out_end(job);
    free(t->text);
    free(t);
    free(in);
    return r;
}

int gz_extract(struct job *job, FILE *f, const char *archive, int format) {
    char name[300];
    const char *base = archive;
    for (const char *p = archive; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    snprintf(name, sizeof(name), "%s", base);
    char *dot = strrchr(name, '.');
    if (dot && dot != name) *dot = 0;
    struct src *in = malloc(sizeof(*in));
    if (!in) return job_fail(job, "Out of memory.");
    src_open(in, f, 0, ~0ull, job);
    int r = out_begin(job, name, 0, 0);
    if (!r) {
        r = format == ARC_GZ ? gunzip(in, out_sink(job), job, 0) : xz_decode(in, out_sink(job), job);
        if (!r) r = out_end(job);
    } else if (r > 0) r = 0;
    free(in);
    return r;
}

struct tar_out {
    int format, error;
    struct fsink file;
    struct deflater *deflate;
    struct xz_enc *xz;
    u32 crc, size;
};

static int put_out(void *context, const u8 *data, size_t n) {
    struct tar_out *o = context;
    if (o->error) return -1;
    int r;
    if (o->format == ARC_TAR_GZ) {
        o->crc = crc32_update(o->crc, data, n);
        o->size += (u32)n;
        r = deflate_write(o->deflate, data, n);
    } else if (o->format == ARC_TAR_XZ) r = xz_enc_write(o->xz, data, n);
    else r = o->file.sink.write(&o->file.sink, data, n);
    if (r) o->error = 1;
    return r;
}

static void octal(u8 *field, int width, u64 value) {
    field[width - 1] = 0;
    for (int i = width - 2; i >= 0; --i) { field[i] = (u8)('0' + (value & 7)); value >>= 3; }
}

static int put_header(struct tar_out *o, const char *name, char type, u64 size, long long mtime, int directory) {
    u8 h[512];
    memset(h, 0, sizeof(h));
    size_t len = strlen(name);
    size_t split = 0;
    if (len > 100) {
        for (size_t i = len - 1; i > 0; --i)
            if (name[i] == '/' && i <= 155 && len - i - 1 <= 100 && len - i - 1 > 0) { split = i; break; }
    }
    if (len <= 100) memcpy(h, name, len);
    else if (split) { memcpy(h + 345, name, split); memcpy(h, name + split + 1, len - split - 1); }
    else memcpy(h, name, 100);
    octal(h + 100, 8, directory ? 0755 : 0644);
    octal(h + 108, 8, 0);
    octal(h + 116, 8, 0);
    octal(h + 124, 12, size);
    octal(h + 136, 12, mtime > 0 ? (u64)mtime : 0);
    memset(h + 148, ' ', 8);
    h[156] = (u8)type;
    memcpy(h + 257, "ustar", 6);
    h[263] = h[264] = '0';
    u32 sum = 0;
    for (int i = 0; i < 512; ++i) sum += h[i];
    octal(h + 148, 7, sum);
    h[155] = ' ';
    return put_out(o, h, 512);
}

static int put_padding(struct tar_out *o, u64 size) {
    static const u8 zero[512];
    size_t pad = (size_t)((512 - size % 512) % 512);
    return pad ? put_out(o, zero, pad) : 0;
}

static int put_entry(struct tar_out *o, const struct arc_item *it, struct job *job) {
    char name[700];
    snprintf(name, sizeof(name), "%s%s", it->name, it->directory ? "/" : "");
    size_t len = strlen(name);
    int fits = len <= 100;
    for (size_t i = 0; !fits && i < len; ++i)
        if (name[i] == '/' && i <= 155 && len - i - 1 <= 100 && len - i - 1 > 0) fits = 1;
    if (!fits) {
        if (put_header(o, "././@LongLink", 'L', len + 1, 0, 0) || put_out(o, (const u8 *)name, len + 1) || put_padding(o, len + 1)) return -1;
    }
    if (put_header(o, name, it->directory ? '5' : '0', it->directory ? 0 : it->size, it->mtime, it->directory)) return -1;
    if (it->directory) return 0;
    if (arc_feed(job, it, put_out, o, 0)) return -1;
    return put_padding(o, it->size);
}

int tar_create(struct job *job, FILE *f, int format, struct arc_item *items, int n) {
    struct tar_out o;
    memset(&o, 0, sizeof(o));
    o.format = format;
    fsink_init(&o.file, f);
    if (format == ARC_TAR_GZ) {
        static const u8 gz[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3};
        if (o.file.sink.write(&o.file.sink, gz, 10) || !(o.deflate = deflate_new(&o.file.sink))) return job_fail(job, "Out of memory.");
    } else if (format == ARC_TAR_XZ && !(o.xz = xz_enc_new(&o.file.sink))) return job_fail(job, "Out of memory.");
    int r = 0;
    for (int i = 0; i < n && !r; ++i) r = put_entry(&o, &items[i], job);
    if (!r) {
        static const u8 zero[1024];
        r = put_out(&o, zero, sizeof(zero));
    }
    if (format == ARC_TAR_GZ) {
        if (r) deflate_free(o.deflate);
        else if (deflate_finish(o.deflate)) r = -1;
        else {
            u8 tail[8];
            put32(tail, o.crc);
            put32(tail + 4, o.size);
            r = o.file.sink.write(&o.file.sink, tail, 8);
        }
    } else if (format == ARC_TAR_XZ) {
        if (r) xz_enc_free(o.xz);
        else r = xz_enc_finish(o.xz);
    }
    if (r || o.file.error) return job_fail(job, "The archive could not be written (is the disk full?).");
    return 0;
}
