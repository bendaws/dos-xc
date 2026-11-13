/*
 * DOS-XC /desktop/archive/inflate.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "arc.h"
#include <stdlib.h>
#include <string.h>

#define RING 65536

struct inflate {
    struct src *in;
    u64 bits;
    int count, over;
    struct sink *out;
    u64 pos, flushed;
    u16 lit[1 << 15], dist[1 << 15], lens[1 << 7];
    int lit_max, dist_max, lens_max;
    u8 ring[RING];
};

static int need(struct inflate *s, int n) {
    while (s->count < n) {
        int b = src_byte(s->in);
        if (b < 0) {
            if (s->in->error || ++s->over > 8) return -1;
            b = 0;
        }
        s->bits |= (u64)b << s->count;
        s->count += 8;
    }
    return 0;
}

static int bits(struct inflate *s, int n, u32 *v) {
    if (need(s, n)) return -1;
    *v = (u32)(s->bits & ((1ull << n) - 1));
    s->bits >>= n;
    s->count -= n;
    return 0;
}

static int build(u16 *table, int *max, const u8 *lengths, int n) {
    int count[16] = {0}, next[16];
    for (int i = 0; i < n; ++i) count[lengths[i]]++;
    count[0] = 0;
    int left = 1, top = 0;
    for (int l = 1; l < 16; ++l) {
        left = (left << 1) - count[l];
        if (left < 0) return -1;
        if (count[l]) top = l;
    }
    if (!top) top = 1;
    *max = top;
    memset(table, 0, sizeof(u16) << top);
    int code = 0;
    for (int l = 1; l < 16; ++l) { code = (code + count[l - 1]) << 1; next[l] = code; }
    for (int i = 0; i < n; ++i) {
        int l = lengths[i];
        if (!l) continue;
        unsigned c = (unsigned)next[l]++, r = 0;
        for (int k = 0; k < l; ++k) { r = r << 1 | (c & 1); c >>= 1; }
        for (unsigned k = r; k < 1u << top; k += 1u << l) table[k] = (u16)(i << 4 | l);
    }
    return 0;
}

static int decode(struct inflate *s, const u16 *table, int max) {
    if (need(s, max)) {
        if (s->count <= 0) return -1;
    }
    u16 e = table[s->bits & ((1u << max) - 1)];
    int l = e & 15;
    if (!l || l > s->count) return -1;
    s->bits >>= l;
    s->count -= l;
    return e >> 4;
}

static int flush(struct inflate *s) {
    while (s->flushed < s->pos) {
        size_t at = (size_t)(s->flushed & (RING - 1));
        size_t n = (size_t)(s->pos - s->flushed);
        if (n > RING - at) n = RING - at;
        if (s->out->write(s->out, s->ring + at, n)) return -1;
        s->flushed += n;
    }
    return 0;
}

static inline int put(struct inflate *s, u8 b) {
    s->ring[s->pos++ & (RING - 1)] = b;
    return 0;
}

static const u16 len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const u8 len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const u16 dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const u8 dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int codes(struct inflate *s) {
    for (;;) {
        if (s->pos - s->flushed >= RING / 2 && flush(s)) return -1;
        int sym = decode(s, s->lit, s->lit_max);
        if (sym < 0) return -1;
        if (sym < 256) { put(s, (u8)sym); continue; }
        if (sym == 256) return 0;
        sym -= 257;
        if (sym >= 29) return -1;
        u32 extra, d;
        if (bits(s, len_extra[sym], &extra)) return -1;
        unsigned length = len_base[sym] + extra;
        int ds = decode(s, s->dist, s->dist_max);
        if (ds < 0 || ds >= 30 || bits(s, dist_extra[ds], &d)) return -1;
        u64 distance = dist_base[ds] + d;
        if (distance > s->pos) return -1;
        for (unsigned i = 0; i < length; ++i) put(s, s->ring[(s->pos - distance) & (RING - 1)]);
    }
}

static int stored(struct inflate *s) {
    s->bits >>= s->count & 7;
    s->count -= s->count & 7;
    u32 len, nlen;
    if (bits(s, 16, &len) || bits(s, 16, &nlen) || (len ^ 0xffff) != nlen) return -1;
    while (len && s->count >= 8) { u32 b = 0; bits(s, 8, &b); put(s, (u8)b); --len; }
    while (len) {
        if (s->pos - s->flushed >= RING / 2 && flush(s)) return -1;
        int b = src_byte(s->in);
        if (b < 0) return -1;
        put(s, (u8)b);
        --len;
    }
    return 0;
}

static int fixed(struct inflate *s) {
    u8 l[320];
    int i = 0;
    for (; i < 144; ++i) l[i] = 8;
    for (; i < 256; ++i) l[i] = 9;
    for (; i < 280; ++i) l[i] = 7;
    for (; i < 288; ++i) l[i] = 8;
    if (build(s->lit, &s->lit_max, l, 288)) return -1;
    for (i = 0; i < 30; ++i) l[i] = 5;
    return build(s->dist, &s->dist_max, l, 30);
}

static int dynamic(struct inflate *s) {
    static const u8 order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    u32 hlit, hdist, hclen;
    if (bits(s, 5, &hlit) || bits(s, 5, &hdist) || bits(s, 4, &hclen)) return -1;
    hlit += 257;
    hdist += 1;
    hclen += 4;
    if (hlit > 286 || hdist > 30) return -1;
    u8 l[320];
    memset(l, 0, sizeof(l));
    for (u32 i = 0; i < hclen; ++i) {
        u32 v;
        if (bits(s, 3, &v)) return -1;
        l[order[i]] = (u8)v;
    }
    if (build(s->lens, &s->lens_max, l, 19)) return -1;
    u8 all[320];
    u32 n = 0;
    while (n < hlit + hdist) {
        int sym = decode(s, s->lens, s->lens_max);
        if (sym < 0) return -1;
        if (sym < 16) { all[n++] = (u8)sym; continue; }
        u32 repeat, value = 0;
        if (sym == 16) {
            if (!n || bits(s, 2, &repeat)) return -1;
            repeat += 3;
            value = all[n - 1];
        } else if (sym == 17) {
            if (bits(s, 3, &repeat)) return -1;
            repeat += 3;
        } else {
            if (bits(s, 7, &repeat)) return -1;
            repeat += 11;
        }
        if (n + repeat > hlit + hdist) return -1;
        while (repeat--) all[n++] = (u8)value;
    }
    if (!all[256]) return -1;
    if (build(s->lit, &s->lit_max, all, (int)hlit)) return -1;
    return build(s->dist, &s->dist_max, all + hlit, (int)hdist);
}

int inflate_stream(struct src *in, struct sink *out, struct job *job, u8 *rest, int *rest_count) {
    struct inflate *s = malloc(sizeof(*s));
    if (!s) return job_fail(job, "Out of memory.");
    s->in = in;
    s->bits = 0;
    s->count = s->over = 0;
    s->out = out;
    s->pos = s->flushed = 0;
    int r = 0;
    for (u32 last = 0; !last && !r;) {
        u32 type;
        if (bits(s, 1, &last) || bits(s, 2, &type)) { r = -1; break; }
        if (type == 0) r = stored(s);
        else if (type == 1) r = fixed(s) || codes(s);
        else if (type == 2) r = dynamic(s) || codes(s);
        else r = -1;
    }
    if (!r) r = flush(s);
    if (rest_count) {
        s->bits >>= s->count & 7;
        s->count -= s->count & 7;
        int n = 0, whole = s->count / 8 - s->over;
        while (n < whole && n < 8) { rest[n++] = (u8)s->bits; s->bits >>= 8; }
        *rest_count = n;
    }
    free(s);
    if (r && !job->cancelled && !(job->error && job->error[0])) job_fail(job, "The compressed data is damaged.");
    return r ? -1 : 0;
}
