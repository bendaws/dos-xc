/*
 * DOS-XC /desktop/archive/sevenz.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "arc.h"
#include <stdlib.h>
#include <string.h>

enum {
    K_END, K_HEADER, K_ARCHIVE_PROPERTIES, K_ADDITIONAL, K_MAIN, K_FILES, K_PACK, K_UNPACK, K_SUBSTREAMS,
    K_SIZE, K_CRC, K_FOLDER, K_UNPACK_SIZE, K_NUM_UNPACK, K_EMPTY_STREAM, K_EMPTY_FILE, K_ANTI, K_NAME,
    K_CTIME, K_ATIME, K_MTIME, K_ATTRIBUTES, K_COMMENT, K_ENCODED_HEADER, K_START_POS, K_DUMMY
};

#define MAX_CODERS 4

struct coder { u8 id[16]; int id_size, streams; u8 props[16]; int props_size; };
struct folder {
    int coders, bonds, packs;
    struct coder c[MAX_CODERS];
    int bond_in[MAX_CODERS], bond_out[MAX_CODERS], pack_stream[MAX_CODERS];
    u64 unpack[MAX_CODERS];
    u64 pack_offset;                   /* in the file */
    u64 pack_size[MAX_CODERS];
    int first_pack;
    u32 crc;
    int crc_defined;
    u64 substreams;
};

struct archive {
    u64 pack_pos;
    u64 npacks;
    u64 *pack_sizes;
    int nfolders;
    struct folder *folders;
    u64 nsub;                          /* all folders' substreams */
    u64 *sub_size;
    u32 *sub_crc;
    u8 *sub_crc_defined;
    u64 nfiles;
    char **names;
    u8 *empty_stream, *empty_file;
    long long *mtime;
    u32 *attributes;
    u8 *has_attributes;
};

struct rd { const u8 *p, *end; int bad; };

static int rbyte(struct rd *r) {
    if (r->p >= r->end) { r->bad = 1; return 0; }
    return *r->p++;
}

static u64 rnum(struct rd *r) {
    int first = rbyte(r);
    u64 value = 0;
    for (int i = 0, mask = 0x80; i < 8; ++i, mask >>= 1) {
        if (!(first & mask)) return value | (u64)(first & (mask - 1)) << (8 * i);
        value |= (u64)rbyte(r) << (8 * i);
    }
    return value;
}

static u32 r32(struct rd *r) { u32 v = 0; for (int i = 0; i < 4; ++i) v |= (u32)rbyte(r) << (8 * i); return v; }
static u64 r64(struct rd *r) { u64 v = r32(r); return v | (u64)r32(r) << 32; }
static void rskip(struct rd *r, u64 n) { if ((u64)(r->end - r->p) < n) { r->bad = 1; r->p = r->end; } else r->p += n; }

static u8 *rbits(struct rd *r, u64 n) {
    u8 *v = calloc((size_t)n + 1, 1);
    if (!v) { r->bad = 1; return 0; }
    int byte = 0, mask = 0;
    for (u64 i = 0; i < n; ++i) {
        if (!mask) { byte = rbyte(r); mask = 0x80; }
        v[i] = (byte & mask) != 0;
        mask >>= 1;
    }
    return v;
}

static u8 *rdefined(struct rd *r, u64 n) {
    if (rbyte(r)) {
        u8 *v = malloc((size_t)n + 1);
        if (v) memset(v, 1, (size_t)n + 1);
        else r->bad = 1;
        return v;
    }
    return rbits(r, n);
}

static void rdigests(struct rd *r, u64 n, u8 **defined, u32 **crc) {
    *defined = rdefined(r, n);
    *crc = calloc((size_t)n + 1, sizeof(u32));
    if (!*defined || !*crc) { r->bad = 1; return; }
    for (u64 i = 0; i < n && !r->bad; ++i)
        if ((*defined)[i]) (*crc)[i] = r32(r);
}

static void read_folder(struct rd *r, struct folder *f) {
    memset(f, 0, sizeof(*f));
    u64 coders = rnum(r);
    if (!coders || coders > MAX_CODERS) { r->bad = 1; return; }
    f->coders = (int)coders;
    int in_streams = 0;
    for (int i = 0; i < f->coders && !r->bad; ++i) {
        struct coder *c = &f->c[i];
        int flags = rbyte(r);
        c->id_size = flags & 15;
        if (flags & 0x80 || c->id_size > 15) { r->bad = 1; return; }
        for (int k = 0; k < c->id_size; ++k) c->id[k] = (u8)rbyte(r);
        c->streams = 1;
        if (flags & 0x10) {
            u64 a = rnum(r), b = rnum(r);
            if (a > MAX_CODERS || b != 1) { r->bad = 1; return; }
            c->streams = (int)a;
        }
        if (flags & 0x20) {
            u64 n = rnum(r);
            if (n > sizeof(c->props)) { rskip(r, n); c->props_size = -1; }
            else { c->props_size = (int)n; for (u64 k = 0; k < n; ++k) c->props[k] = (u8)rbyte(r); }
        }
        in_streams += c->streams;
    }
    f->bonds = f->coders - 1;
    for (int i = 0; i < f->bonds; ++i) {
        f->bond_in[i] = (int)rnum(r);
        f->bond_out[i] = (int)rnum(r);
    }
    f->packs = in_streams - f->bonds;
    if (f->packs < 1 || f->packs > MAX_CODERS) { r->bad = 1; return; }
    if (f->packs == 1) {
        for (int s = 0; s < in_streams; ++s) {
            int bound = 0;
            for (int b = 0; b < f->bonds; ++b) bound |= f->bond_in[b] == s;
            if (!bound) { f->pack_stream[0] = s; break; }
        }
    } else
        for (int i = 0; i < f->packs; ++i) f->pack_stream[i] = (int)rnum(r);
}

static void read_streams(struct rd *r, struct archive *a) {
    for (;;) {
        int id = (int)rnum(r);
        if (r->bad || id == K_END) return;
        if (id == K_PACK) {
            a->pack_pos = rnum(r);
            a->npacks = rnum(r);
            if (a->npacks > (1u << 20)) { r->bad = 1; return; }
            a->pack_sizes = calloc((size_t)a->npacks + 1, sizeof(u64));
            if (!a->pack_sizes) { r->bad = 1; return; }
            for (;;) {
                int k = (int)rnum(r);
                if (r->bad || k == K_END) break;
                if (k == K_SIZE) for (u64 i = 0; i < a->npacks; ++i) a->pack_sizes[i] = rnum(r);
                else if (k == K_CRC) { u8 *d; u32 *c; rdigests(r, a->npacks, &d, &c); free(d); free(c); }
                else rskip(r, rnum(r));
            }
        } else if (id == K_UNPACK) {
            if (rnum(r) != K_FOLDER) { r->bad = 1; return; }
            u64 n = rnum(r);
            if (n > (1u << 20) || rbyte(r)) { r->bad = 1; return; }
            a->nfolders = (int)n;
            a->folders = calloc((size_t)n + 1, sizeof(struct folder));
            if (!a->folders) { r->bad = 1; return; }
            for (u64 i = 0; i < n && !r->bad; ++i) read_folder(r, &a->folders[i]);
            if (rnum(r) != K_UNPACK_SIZE) { r->bad = 1; return; }
            for (u64 i = 0; i < n; ++i)
                for (int k = 0; k < a->folders[i].coders; ++k) a->folders[i].unpack[k] = rnum(r);
            for (u64 i = 0; i < n; ++i) a->folders[i].substreams = 1;
            for (;;) {
                int k = (int)rnum(r);
                if (r->bad || k == K_END) break;
                if (k == K_CRC) {
                    u8 *d;
                    u32 *c;
                    rdigests(r, n, &d, &c);
                    for (u64 i = 0; d && c && i < n; ++i) { a->folders[i].crc_defined = d[i]; a->folders[i].crc = c[i]; }
                    free(d);
                    free(c);
                } else rskip(r, rnum(r));
            }
        } else if (id == K_SUBSTREAMS) {
            int k = (int)rnum(r);
            if (k == K_NUM_UNPACK) {
                for (int i = 0; i < a->nfolders; ++i) a->folders[i].substreams = rnum(r);
                k = (int)rnum(r);
            }
            u64 total = 0;
            for (int i = 0; i < a->nfolders; ++i) total += a->folders[i].substreams;
            if (total > (1u << 24)) { r->bad = 1; return; }
            a->nsub = total;
            a->sub_size = calloc((size_t)total + 1, sizeof(u64));
            a->sub_crc = calloc((size_t)total + 1, sizeof(u32));
            a->sub_crc_defined = calloc((size_t)total + 1, 1);
            if (!a->sub_size || !a->sub_crc || !a->sub_crc_defined) { r->bad = 1; return; }
            u64 at = 0;
            for (int i = 0; i < a->nfolders; ++i) {
                struct folder *f = &a->folders[i];
                if (!f->substreams) continue;
                u64 sum = 0;
                for (u64 s = 0; s + 1 < f->substreams; ++s) {
                    u64 v = k == K_SIZE ? rnum(r) : 0;
                    a->sub_size[at + s] = v;
                    sum += v;
                }
                u64 whole = f->unpack[0];
                for (int c = 0; c < f->coders; ++c) {
                    int bound = 0;
                    for (int b = 0; b < f->bonds; ++b) bound |= f->bond_out[b] == c;
                    if (!bound) whole = f->unpack[c];
                }
                if (sum > whole) { r->bad = 1; return; }
                a->sub_size[at + f->substreams - 1] = whole - sum;
                at += f->substreams;
            }
            if (k == K_SIZE) k = (int)rnum(r);
            u64 unknown = 0;
            for (int i = 0; i < a->nfolders; ++i)
                if (!(a->folders[i].substreams == 1 && a->folders[i].crc_defined)) unknown += a->folders[i].substreams;
            at = 0;
            for (int i = 0; i < a->nfolders; ++i) {
                struct folder *f = &a->folders[i];
                if (f->substreams == 1 && f->crc_defined) { a->sub_crc[at] = f->crc; a->sub_crc_defined[at] = 1; }
                at += f->substreams;
            }
            while (!r->bad && k != K_END) {
                if (k == K_CRC) {
                    u8 *d;
                    u32 *c;
                    rdigests(r, unknown, &d, &c);
                    u64 j = 0;
                    at = 0;
                    for (int i = 0; d && c && i < a->nfolders; ++i) {
                        struct folder *f = &a->folders[i];
                        if (f->substreams == 1 && f->crc_defined) { at += 1; continue; }
                        for (u64 s = 0; s < f->substreams; ++s, ++j) { a->sub_crc_defined[at + s] = d[j]; a->sub_crc[at + s] = c[j]; }
                        at += f->substreams;
                    }
                    free(d);
                    free(c);
                } else rskip(r, rnum(r));
                k = (int)rnum(r);
            }
        } else {
            r->bad = 1;
            return;
        }
    }
}

static char *utf16_name(struct rd *r) {
    size_t cap = 64, n = 0;
    char *out = malloc(cap);
    if (!out) { r->bad = 1; return 0; }
    for (;;) {
        u32 c = (u32)rbyte(r);
        c |= (u32)rbyte(r) << 8;
        if (r->bad || !c) break;
        if (c >= 0xd800 && c < 0xdc00) {
            u32 low = (u32)rbyte(r);
            low |= (u32)rbyte(r) << 8;
            c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00);
        }
        if (n + 5 >= cap) {
            char *grown = realloc(out, cap *= 2);
            if (!grown) { free(out); r->bad = 1; return 0; }
            out = grown;
        }
        if (c < 0x80) out[n++] = (char)c;
        else if (c < 0x800) { out[n++] = (char)(0xc0 | c >> 6); out[n++] = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { out[n++] = (char)(0xe0 | c >> 12); out[n++] = (char)(0x80 | (c >> 6 & 63)); out[n++] = (char)(0x80 | (c & 63)); }
        else { out[n++] = (char)(0xf0 | c >> 18); out[n++] = (char)(0x80 | (c >> 12 & 63)); out[n++] = (char)(0x80 | (c >> 6 & 63)); out[n++] = (char)(0x80 | (c & 63)); }
    }
    out[n] = 0;
    return out;
}

static void read_files(struct rd *r, struct archive *a) {
    a->nfiles = rnum(r);
    if (a->nfiles > (1u << 24)) { r->bad = 1; return; }
    size_t n = (size_t)a->nfiles + 1;
    a->names = calloc(n, sizeof(char *));
    a->mtime = calloc(n, sizeof(long long));
    a->attributes = calloc(n, sizeof(u32));
    a->has_attributes = calloc(n, 1);
    a->empty_stream = calloc(n, 1);
    a->empty_file = calloc(n, 1);
    if (!a->names || !a->mtime || !a->attributes || !a->has_attributes || !a->empty_stream || !a->empty_file) { r->bad = 1; return; }
    u64 empties = 0;
    for (;;) {
        int id = (int)rnum(r);
        if (r->bad || id == K_END) return;
        u64 size = rnum(r);
        struct rd sub = {r->p, r->p + (size <= (u64)(r->end - r->p) ? size : 0), 0};
        rskip(r, size);
        if (r->bad) return;
        if (id == K_EMPTY_STREAM) {
            u8 *v = rbits(&sub, a->nfiles);
            if (!v) { r->bad = 1; return; }
            memcpy(a->empty_stream, v, (size_t)a->nfiles);
            free(v);
            empties = 0;
            for (u64 i = 0; i < a->nfiles; ++i) empties += a->empty_stream[i];
        } else if (id == K_EMPTY_FILE) {
            u8 *v = rbits(&sub, empties);
            if (!v) { r->bad = 1; return; }
            for (u64 i = 0, k = 0; i < a->nfiles; ++i)
                if (a->empty_stream[i]) a->empty_file[i] = v[k++];
            free(v);
        } else if (id == K_NAME) {
            if (rbyte(&sub)) { r->bad = 1; return; }
            for (u64 i = 0; i < a->nfiles && !sub.bad; ++i) a->names[i] = utf16_name(&sub);
        } else if (id == K_MTIME) {
            u8 *d = rdefined(&sub, a->nfiles);
            if (!d) { r->bad = 1; return; }
            if (!rbyte(&sub))
                for (u64 i = 0; i < a->nfiles; ++i)
                    if (d[i]) {
                        u64 t = r64(&sub);
                        a->mtime[i] = t > 116444736000000000ull ? (long long)(t / 10000000 - 11644473600ull) : 0;
                    }
            free(d);
        } else if (id == K_ATTRIBUTES) {
            u8 *d = rdefined(&sub, a->nfiles);
            if (!d) { r->bad = 1; return; }
            if (!rbyte(&sub))
                for (u64 i = 0; i < a->nfiles; ++i)
                    if (d[i]) { a->attributes[i] = r32(&sub); a->has_attributes[i] = 1; }
            free(d);
        }
    }
}

static void free_archive(struct archive *a) {
    free(a->pack_sizes);
    free(a->folders);
    free(a->sub_size);
    free(a->sub_crc);
    free(a->sub_crc_defined);
    if (a->names)
        for (u64 i = 0; i < a->nfiles; ++i) free(a->names[i]);
    free(a->names);
    free(a->empty_stream);
    free(a->empty_file);
    free(a->mtime);
    free(a->attributes);
    free(a->has_attributes);
    memset(a, 0, sizeof(*a));
}

struct filter {
    struct sink sink, *next;
    int kind;                          /* 1 BCJ x86, 2 delta */
    u8 buf[65536 + 8];
    size_t n;
    u32 ip, state;
    u8 history[256];
    unsigned distance, at;
};

#define TEST86(b) ((((b) + 1) & 0xfe) == 0)

static size_t x86_convert(u8 *data, size_t size, u32 ip, u32 *state) {
    size_t pos = 0;
    u32 mask = *state & 7;
    if (size < 5) return 0;
    size -= 4;
    ip += 5;
    for (;;) {
        u8 *p = data + pos;
        const u8 *limit = data + size;
        for (; p < limit; ++p)
            if ((*p & 0xfe) == 0xe8) break;
        size_t d = (size_t)(p - data) - pos;
        pos = (size_t)(p - data);
        if (p >= limit) {
            *state = d > 2 ? 0 : mask >> (unsigned)d;
            return pos;
        }
        if (d > 2) mask = 0;
        else {
            mask >>= (unsigned)d;
            if (mask && (mask > 4 || mask == 3 || TEST86(p[(mask >> 1) + 1]))) {
                mask = (mask >> 1) | 4;
                ++pos;
                continue;
            }
        }
        if (TEST86(p[4])) {
            u32 v = (u32)p[4] << 24 | (u32)p[3] << 16 | (u32)p[2] << 8 | p[1];
            u32 cur = ip + (u32)pos;
            pos += 5;
            v -= cur;
            if (mask) {
                unsigned sh = (mask & 6) << 2;
                if (TEST86((u8)(v >> sh))) {
                    v ^= ((u32)0x100 << sh) - 1;
                    v -= cur;
                }
                mask = 0;
            }
            p[1] = (u8)v;
            p[2] = (u8)(v >> 8);
            p[3] = (u8)(v >> 16);
            p[4] = (u8)(0 - ((v >> 24) & 1));
        } else {
            mask = (mask >> 1) | 4;
            ++pos;
        }
    }
}

static int filter_write(struct sink *s, const u8 *data, size_t n) {
    struct filter *f = (struct filter *)s;
    if (f->kind == 2) {
        while (n) {
            size_t k = n < 65536 ? n : 65536;
            for (size_t i = 0; i < k; ++i) {
                u8 b = (u8)(data[i] + f->history[(f->at - f->distance) & 255]);
                f->history[f->at++ & 255] = b;
                f->buf[i] = b;
                f->at &= 255;
            }
            if (f->next->write(f->next, f->buf, k)) return -1;
            data += k;
            n -= k;
        }
        return 0;
    }
    while (n) {
        size_t k = 65536 - f->n < n ? 65536 - f->n : n;
        memcpy(f->buf + f->n, data, k);
        f->n += k;
        data += k;
        n -= k;
        size_t done = x86_convert(f->buf, f->n, f->ip, &f->state);
        if (done && f->next->write(f->next, f->buf, done)) return -1;
        f->ip += (u32)done;
        memmove(f->buf, f->buf + done, f->n - done);
        f->n -= done;
        if (f->n == 65536) {
            if (f->next->write(f->next, f->buf, f->n)) return -1;
            f->ip += (u32)f->n;
            f->n = 0;
        }
    }
    return 0;
}

static int filter_finish(struct filter *f) {
    if (f->kind == 1 && f->n && f->next->write(f->next, f->buf, f->n)) return -1;
    f->n = 0;
    return 0;
}

static int id_is(const struct coder *c, const char *hex) {
    size_t n = strlen(hex) / 2;
    if ((int)n != c->id_size) return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        if (c->id[i] != v) return 0;
    }
    return 1;
}

static const char *coder_name(const struct coder *c) {
    if (id_is(c, "06f10701")) return "AES encryption (a password)";
    if (id_is(c, "040202")) return "BZip2";
    if (id_is(c, "030401")) return "PPMd";
    if (id_is(c, "0303011b")) return "BCJ2";
    if (id_is(c, "040109")) return "Deflate64";
    if (c->id_size >= 1 && c->id[0] == 0x04 && c->id_size >= 2 && c->id[1] == 0xf7) return "Zstandard or another plug-in method";
    return "a method";
}

static int decode_folder(struct job *job, FILE *f, struct folder *fo, struct sink *out) {
    if (fo->packs != 1) return job_fail(job, "The archive uses %s that DOS-XC cannot unpack.", "BCJ2 or several packed streams");
    int main = -1;
    for (int c = 0; c < fo->coders; ++c) {
        int bound = 0;
        for (int b = 0; b < fo->bonds; ++b) bound |= fo->bond_out[b] == c;
        if (!bound) main = c;
    }
    if (main < 0) return job_fail(job, "The 7z archive is damaged.");
    int chain[MAX_CODERS], depth = 0;
    for (int c = main; depth < MAX_CODERS;) {
        chain[depth++] = c;
        if (fo->c[c].streams != 1) return job_fail(job, "The archive uses %s, which DOS-XC cannot unpack.", coder_name(&fo->c[c]));
        int in = 0;
        for (int k = 0; k < c; ++k) in += fo->c[k].streams;
        int next = -1;
        for (int b = 0; b < fo->bonds; ++b)
            if (fo->bond_in[b] == in) next = fo->bond_out[b];
        if (next < 0) break;
        c = next;
    }
    struct filter *filters[MAX_CODERS] = {0};
    struct sink *target = out;
    int r = 0;
    for (int i = 0; i < depth - 1 && !r; ++i) {
        const struct coder *c = &fo->c[chain[i]];
        int kind = id_is(c, "03030103") ? 1 : id_is(c, "03") ? 2 : id_is(c, "00") ? 3 : 0;
        if (!kind) { r = job_fail(job, "The archive uses %s, which DOS-XC cannot unpack.", coder_name(c)); break; }
        if (kind == 3) continue;
        struct filter *fl = calloc(1, sizeof(*fl));
        if (!fl) { r = job_fail(job, "Out of memory."); break; }
        fl->sink.write = filter_write;
        fl->kind = kind;
        fl->distance = kind == 2 && c->props_size >= 1 ? c->props[0] + 1u : 1;
        filters[i] = fl;
    }
    for (int i = 0; i < depth - 1 && !r; ++i)
        if (filters[i]) { filters[i]->next = target; target = &filters[i]->sink; }
    if (!r) {
        const struct coder *c = &fo->c[chain[depth - 1]];
        u64 size = fo->unpack[chain[depth - 1]];
        struct src *in = malloc(sizeof(*in));
        if (!in) r = job_fail(job, "Out of memory.");
        else {
            src_open(in, f, fo->pack_offset, fo->pack_size[0], job);
            if (id_is(c, "030101") && c->props_size == 5) r = lzma_decode(in, c->props, size, target, job);
            else if (id_is(c, "21") && c->props_size == 1) r = lzma2_decode(in, c->props[0], size, target, job);
            else if (id_is(c, "040108")) r = inflate_stream(in, target, job, 0, 0);
            else if (id_is(c, "00")) {
                u64 left = size;
                while (!r && left && src_fill(in)) {
                    size_t k = in->len < left ? in->len : (size_t)left;
                    r = target->write(target, in->buf, k);
                    left -= k;
                }
                if (left) r = job_fail(job, "The 7z archive ends too soon.");
            } else r = job_fail(job, "The archive uses %s, which DOS-XC cannot unpack.", coder_name(c));
            free(in);
        }
    }
    for (int i = depth - 2; i >= 0; --i)
        if (filters[i]) {
            if (!r) r = filter_finish(filters[i]);
            free(filters[i]);
        }
    return r;
}

struct memsink { struct sink sink; u8 *data; size_t n, cap; int bad; };

static int mem_write(struct sink *s, const u8 *data, size_t n) {
    struct memsink *m = (struct memsink *)s;
    if (m->n + n > m->cap) {
        size_t cap = m->cap ? m->cap : 65536;
        while (cap < m->n + n) cap *= 2;
        if (cap > (256u << 20)) return -1;
        u8 *grown = realloc(m->data, cap);
        if (!grown) return -1;
        m->data = grown;
        m->cap = cap;
    }
    memcpy(m->data + m->n, data, n);
    m->n += n;
    return 0;
}

static int place_folders(struct job *job, struct archive *a, u64 file_size) {
    u64 offset = 32 + a->pack_pos;
    u64 pack = 0;
    for (int i = 0; i < a->nfolders; ++i) {
        struct folder *f = &a->folders[i];
        if (pack + (u64)f->packs > a->npacks) return job_fail(job, "The 7z archive is damaged.");
        f->first_pack = (int)pack;
        f->pack_offset = offset;
        for (int k = 0; k < f->packs; ++k) {
            f->pack_size[k] = a->pack_sizes[pack];
            offset += a->pack_sizes[pack++];
        }
        if (offset > file_size) return job_fail(job, "The 7z archive ends too soon.");
    }
    return 0;
}

struct split {
    struct sink sink;
    struct job *job;
    struct archive *a;
    u64 file, stream, last_stream;      /* next file index; substream index */
    u64 left;
    int open, writing;
    u32 want_crc;
    int check;
};

static int next_file(struct split *s) {
    struct archive *a = s->a;
    while (s->file < a->nfiles && a->empty_stream[s->file]) ++s->file;
    if (s->file >= a->nfiles || s->stream >= s->last_stream) return job_fail(s->job, "The 7z archive is damaged.");
    const char *name = a->names[s->file] ? a->names[s->file] : "file";
    int r = out_begin(s->job, name, 0, a->mtime[s->file]);
    if (r < 0) return -1;
    s->writing = !r;
    s->open = 1;
    s->left = a->sub_size[s->stream];
    s->check = a->sub_crc_defined[s->stream];
    s->want_crc = a->sub_crc[s->stream];
    ++s->stream;
    ++s->file;
    return 0;
}

static int close_file(struct split *s) {
    s->open = 0;
    if (!s->writing) return 0;
    if (s->check && s->job->crc != s->want_crc) return job_fail(s->job, "\"%s\" is damaged (its check does not match).", s->job->name);
    return out_end(s->job);
}

static int split_write(struct sink *sink, const u8 *data, size_t n) {
    struct split *s = (struct split *)sink;
    while (n) {
        if (!s->open && next_file(s)) return -1;
        size_t k = s->left < n ? (size_t)s->left : n;
        if (s->writing && out_write(s->job, data, k)) return -1;
        s->left -= k;
        data += k;
        n -= k;
        if (!s->left && close_file(s)) return -1;
    }
    return 0;
}

static int split_finish(struct split *s) {
    for (;;) {
        if (s->open) { if (s->left) return job_fail(s->job, "The 7z archive ends too soon."); if (close_file(s)) return -1; }
        if (s->stream >= s->last_stream) return 0;
        if (s->a->sub_size[s->stream]) return job_fail(s->job, "The 7z archive ends too soon.");
        if (next_file(s)) return -1;
    }
}

static int read_header(struct job *job, FILE *f, struct archive *a, u64 file_size) {
    u8 sig[32];
    if (arc_seek(f, 0) || fread(sig, 1, 32, f) != 32) return job_fail(job, "The archive could not be read.");
    if (sig[6] != 0) return job_fail(job, "This 7z archive is a newer version than DOS-XC understands.");
    u64 offset = get64(sig + 12), size = get64(sig + 20);
    u32 crc = get32(sig + 28);
    if (crc32_update(0, sig + 12, 20) != get32(sig + 8)) return job_fail(job, "The 7z archive is damaged.");
    if (!size) return 0;
    if (32 + offset + size > file_size) return job_fail(job, "The 7z archive is incomplete (it may be one part of several).");
    if (size > (64u << 20)) return job_fail(job, "The 7z archive's header is too large.");
    u8 *header = malloc((size_t)size);
    if (!header) return job_fail(job, "Out of memory.");
    if (arc_seek(f, 32 + offset) || fread(header, 1, (size_t)size, f) != size) { free(header); return job_fail(job, "The archive could not be read."); }
    if (crc32_update(0, header, (size_t)size) != crc) { free(header); return job_fail(job, "The 7z archive's header is damaged."); }
    for (int round = 0; round < 4; ++round) {
        struct rd r = {header, header + size, 0};
        int id = (int)rnum(&r);
        if (id == K_HEADER) {
            for (;;) {
                int k = (int)rnum(&r);
                if (r.bad || k == K_END) break;
                if (k == K_ARCHIVE_PROPERTIES) {
                    for (;;) { int p = (int)rnum(&r); if (r.bad || !p) break; rskip(&r, rnum(&r)); }
                } else if (k == K_ADDITIONAL) {
                    struct archive skip;
                    memset(&skip, 0, sizeof(skip));
                    read_streams(&r, &skip);
                    free_archive(&skip);
                } else if (k == K_MAIN) read_streams(&r, a);
                else if (k == K_FILES) read_files(&r, a);
                else r.bad = 1;
            }
            free(header);
            if (r.bad) return job_fail(job, "The 7z archive's header is damaged.");
            return 0;
        }
        if (id != K_ENCODED_HEADER) break;
        struct archive packed;
        memset(&packed, 0, sizeof(packed));
        read_streams(&r, &packed);
        if (r.bad || packed.nfolders < 1 || place_folders(job, &packed, file_size)) { free_archive(&packed); break; }
        struct memsink m = {{mem_write}, 0, 0, 0, 0};
        int bad = decode_folder(job, f, &packed.folders[0], &m.sink);
        if (!bad && packed.folders[0].crc_defined && crc32_update(0, m.data, m.n) != packed.folders[0].crc) bad = 1;
        free_archive(&packed);
        free(header);
        header = m.data;
        size = m.n;
        if (bad || !header) { free(header); return job_fail(job, "The 7z archive's header is damaged."); }
    }
    free(header);
    return job_fail(job, "The 7z archive's header is damaged.");
}

int sevenz_extract(struct job *job, FILE *f) {
    struct archive a;
    memset(&a, 0, sizeof(a));
    u64 file_size = arc_file_size(f);
    int r = read_header(job, f, &a, file_size);
    if (!r && a.nfolders && place_folders(job, &a, file_size)) r = -1;
    if (!r && a.nfolders && !a.sub_size) {
        a.nsub = (u64)a.nfolders;
        a.sub_size = calloc(a.nsub + 1, sizeof(u64));
        a.sub_crc = calloc(a.nsub + 1, sizeof(u32));
        a.sub_crc_defined = calloc(a.nsub + 1, 1);
        if (!a.sub_size || !a.sub_crc || !a.sub_crc_defined) r = job_fail(job, "Out of memory.");
        for (int i = 0; !r && i < a.nfolders; ++i) {
            struct folder *fo = &a.folders[i];
            a.sub_size[i] = fo->unpack[0];
            for (int c = 0; c < fo->coders; ++c) {
                int bound = 0;
                for (int b = 0; b < fo->bonds; ++b) bound |= fo->bond_out[b] == c;
                if (!bound) a.sub_size[i] = fo->unpack[c];
            }
            a.sub_crc[i] = fo->crc;
            a.sub_crc_defined[i] = (u8)fo->crc_defined;
        }
    }
    for (u64 i = 0; !r && i < a.nfiles; ++i) {
        if (!a.empty_stream[i]) continue;
        int directory = !a.empty_file[i] || (a.has_attributes[i] && (a.attributes[i] & 0x10));
        const char *name = a.names[i] ? a.names[i] : "file";
        int k = out_begin(job, name, directory, a.mtime[i]);
        if (k < 0) r = -1;
        else if (!k && out_end(job)) r = -1;
    }
    struct split s;
    memset(&s, 0, sizeof(s));
    s.sink.write = split_write;
    s.job = job;
    s.a = &a;
    for (int i = 0; !r && i < a.nfolders; ++i) {
        s.last_stream = s.stream + a.folders[i].substreams;
        r = decode_folder(job, f, &a.folders[i], &s.sink);
        if (!r) r = split_finish(&s);
    }
    if (r && s.open && s.writing) out_abort(job);
    free_archive(&a);
    return r;
}

struct buf { u8 *p; size_t n, cap; int bad; };

static void bput(struct buf *b, const void *data, size_t n) {
    if (b->bad) return;
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->n + n) cap *= 2;
        u8 *grown = realloc(b->p, cap);
        if (!grown) { b->bad = 1; return; }
        b->p = grown;
        b->cap = cap;
    }
    memcpy(b->p + b->n, data, n);
    b->n += n;
}

static void bbyte(struct buf *b, u8 v) { bput(b, &v, 1); }

static void bnum(struct buf *b, u64 v) {
    int extra = 0;
    while (extra < 8 && v >= 1ull << (7 * (extra + 1))) ++extra;
    u8 first = (u8)(0xff00u >> extra);
    if (extra < 8) first |= (u8)(v >> (8 * extra));
    bbyte(b, first);
    for (int i = 0; i < extra; ++i) bbyte(b, (u8)(v >> (8 * i)));
}

static void bbits(struct buf *b, const u8 *bits, u64 n) {
    u8 byte = 0;
    int mask = 0x80;
    for (u64 i = 0; i < n; ++i) {
        if (bits[i]) byte |= (u8)mask;
        mask >>= 1;
        if (!mask) { bbyte(b, byte); byte = 0; mask = 0x80; }
    }
    if (mask != 0x80) bbyte(b, byte);
}

static void bprop(struct buf *b, int id, const struct buf *data) {
    bbyte(b, (u8)id);
    bnum(b, data->n);
    bput(b, data->p, data->n);
}

static void utf16(struct buf *b, const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        u32 c = *p++;
        if (c >= 0xf0 && p[0] && p[1] && p[2]) { c = (c & 7) << 18 | (u32)(p[0] & 63) << 12 | (u32)(p[1] & 63) << 6 | (p[2] & 63); p += 3; }
        else if (c >= 0xe0 && p[0] && p[1]) { c = (c & 15) << 12 | (u32)(p[0] & 63) << 6 | (p[1] & 63); p += 2; }
        else if (c >= 0xc0 && p[0]) { c = (c & 31) << 6 | (p[0] & 63); p += 1; }
        if (c >= 0x10000) {
            c -= 0x10000;
            u8 hi[2] = {(u8)(0xd800 + (c >> 10)), (u8)((0xd800 + (c >> 10)) >> 8)};
            u8 lo[2] = {(u8)(0xdc00 + (c & 1023)), (u8)((0xdc00 + (c & 1023)) >> 8)};
            bput(b, hi, 2);
            bput(b, lo, 2);
        } else {
            u8 w[2] = {(u8)c, (u8)(c >> 8)};
            bput(b, w, 2);
        }
    }
    bput(b, "\0", 2);
}

static int lzma2_feed(void *context, const u8 *data, size_t n) { return lzma2_enc_write(context, data, n); }

int sevenz_create(struct job *job, FILE *f, struct arc_item *items, int n) {
    u8 blank[32] = {0};
    struct fsink file;
    fsink_init(&file, f);
    if (file.sink.write(&file.sink, blank, 32)) return job_fail(job, "The archive could not be written.");
    u64 *sizes = calloc((size_t)n + 1, sizeof(u64));
    u32 *crcs = calloc((size_t)n + 1, sizeof(u32));
    u8 *empty = calloc((size_t)n + 1, 1), *empty_file = calloc((size_t)n + 1, 1);
    if (!sizes || !crcs || !empty || !empty_file) { free(sizes); free(crcs); free(empty); free(empty_file); return job_fail(job, "Out of memory."); }
    int streams = 0, empties = 0, empty_files = 0, r = 0;
    u8 prop = 0;
    u64 total = 0;
    struct lzma2_enc *e = 0;
    for (int i = 0; i < n; ++i) {
        if (items[i].directory || !items[i].size) {
            empty[i] = 1;
            empty_file[empties++] = !items[i].directory;
            empty_files += !items[i].directory;
        }
    }
    for (int i = 0; i < n && !r; ++i) {
        if (empty[i]) continue;
        if (!e && !(e = lzma2_enc_new(&file.sink, &prop))) { r = job_fail(job, "Out of memory."); break; }
        if (arc_feed(job, &items[i], lzma2_feed, e, &crcs[streams])) { r = -1; break; }
        sizes[streams++] = items[i].size;
        total += items[i].size;
    }
    if (e) {
        if (r) lzma2_enc_free(e);
        else if (lzma2_enc_finish(e)) r = -1;
    }
    u64 packed = file.count - 32;
    struct buf h = {0}, p = {0};
    if (!r) {
        bbyte(&h, K_HEADER);
        if (streams) {
            bbyte(&h, K_MAIN);
            bbyte(&h, K_PACK);
            bnum(&h, 0);
            bnum(&h, 1);
            bbyte(&h, K_SIZE);
            bnum(&h, packed);
            bbyte(&h, K_END);
            bbyte(&h, K_UNPACK);
            bbyte(&h, K_FOLDER);
            bnum(&h, 1);
            bbyte(&h, 0);
            bnum(&h, 1);
            bbyte(&h, 0x21);
            bbyte(&h, 0x21);
            bnum(&h, 1);
            bbyte(&h, prop);
            bbyte(&h, K_UNPACK_SIZE);
            bnum(&h, total);
            bbyte(&h, K_END);
            bbyte(&h, K_SUBSTREAMS);
            if (streams != 1) {
                bbyte(&h, K_NUM_UNPACK);
                bnum(&h, (u64)streams);
                bbyte(&h, K_SIZE);
                for (int i = 0; i + 1 < streams; ++i) bnum(&h, sizes[i]);
            }
            bbyte(&h, K_CRC);
            bbyte(&h, 1);
            for (int i = 0; i < streams; ++i) { u8 c[4]; put32(c, crcs[i]); bput(&h, c, 4); }
            bbyte(&h, K_END);
            bbyte(&h, K_END);
        }
        if (n) {
            bbyte(&h, K_FILES);
            bnum(&h, (u64)n);
            if (empties) {
                p.n = 0;
                bbits(&p, empty, (u64)n);
                bprop(&h, K_EMPTY_STREAM, &p);
                if (empty_files) {
                    p.n = 0;
                    bbits(&p, empty_file, (u64)empties);
                    bprop(&h, K_EMPTY_FILE, &p);
                }
            }
            p.n = 0;
            bbyte(&p, 0);
            for (int i = 0; i < n; ++i) utf16(&p, items[i].name);
            bprop(&h, K_NAME, &p);
            p.n = 0;
            bbyte(&p, 1);
            bbyte(&p, 0);
            for (int i = 0; i < n; ++i) {
                u8 t[8];
                put64(t, items[i].mtime > 0 ? ((u64)items[i].mtime + 11644473600ull) * 10000000ull : 0);
                bput(&p, t, 8);
            }
            bprop(&h, K_MTIME, &p);
            p.n = 0;
            bbyte(&p, 1);
            bbyte(&p, 0);
            for (int i = 0; i < n; ++i) { u8 a[4]; put32(a, items[i].directory ? 0x10 : 0x20); bput(&p, a, 4); }
            bprop(&h, K_ATTRIBUTES, &p);
            bbyte(&h, K_END);
        }
        bbyte(&h, K_END);
        if (h.bad || p.bad) r = job_fail(job, "Out of memory.");
    }
    if (!r && file.sink.write(&file.sink, h.p, h.n)) r = -1;
    if (!r) {
        u8 sig[32] = {'7', 'z', 0xbc, 0xaf, 0x27, 0x1c, 0, 4};
        put64(sig + 12, packed);
        put64(sig + 20, h.n);
        put32(sig + 28, crc32_update(0, h.p, h.n));
        put32(sig + 8, crc32_update(0, sig + 12, 20));
        if (arc_seek(f, 0) || fwrite(sig, 1, 32, f) != 32 || fseek(f, 0, SEEK_END)) r = -1;
    }
    free(h.p);
    free(p.p);
    free(sizes);
    free(crcs);
    free(empty);
    free(empty_file);
    if (r || file.error) return job_fail(job, "The archive could not be written (is the disk full?).");
    return 0;
}
