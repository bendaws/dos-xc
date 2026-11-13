/*
 * DOS-XC /desktop/archive/deflate.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "arc.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define WSIZE 32768
#define BUFSIZE (8 * WSIZE)
#define MIN_LOOKAHEAD 262
#define HASH_BITS 15
#define CHAIN 128
#define NICE 128
#define LAZY 16
#define GOOD 8
#define SYMBOLS 16384

struct deflater {
    struct sink *out;
    u8 outbuf[65536];
    size_t outn;
    u64 bitbuf;
    int bitcount, error;
    u8 win[BUFSIZE];
    long long base;                   /* stream offset of win[0] */
    int start, end, emitted, block_start;
    int match_length, match_start, prev_length, prev_match, available;
    long long head[1 << HASH_BITS], prev[WSIZE];
    u16 lit[SYMBOLS], dist[SYMBOLS];
    int count;
};

static const u16 len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const u8 len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const u16 dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const u8 dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int len_code(int length) { int c = 28; while (len_base[c] > length) --c; return c; }
static int dist_code(int distance) { int c = 29; while (dist_base[c] > distance) --c; return c; }

static void out_flush(struct deflater *d) {
    if (d->outn && !d->error && d->out->write(d->out, d->outbuf, d->outn)) d->error = 1;
    d->outn = 0;
}

static void put_bits(struct deflater *d, u32 value, int n) {
    d->bitbuf |= (u64)value << d->bitcount;
    d->bitcount += n;
    while (d->bitcount >= 8) {
        if (d->outn == sizeof(d->outbuf)) out_flush(d);
        d->outbuf[d->outn++] = (u8)d->bitbuf;
        d->bitbuf >>= 8;
        d->bitcount -= 8;
    }
}

static u32 reverse(u32 code, int n) {
    u32 r = 0;
    while (n--) { r = r << 1 | (code & 1); code >>= 1; }
    return r;
}

/* Huffman code lengths of at most LIMIT bits for FREQ. */
static void lengths_for(const u32 *freq_in, int n, int limit, u8 *len) {
    u32 freq[300];
    memcpy(freq, freq_in, sizeof(u32) * (size_t)n);
    int used = 0;
    for (int i = 0; i < n; ++i) used += freq[i] != 0;
    if (used < 2) {
        for (int i = 0; i < n && used < 2; ++i)
            if (!freq[i]) { freq[i] = 1; ++used; }
    }
    for (;;) {
        u32 weight[600];
        int parent[600], alive[600], nodes = 0;
        int leaf[300];
        for (int i = 0; i < n; ++i) {
            leaf[i] = -1;
            if (!freq[i]) continue;
            leaf[i] = nodes;
            weight[nodes] = freq[i];
            parent[nodes] = -1;
            alive[nodes] = 1;
            ++nodes;
        }
        int live = nodes;
        while (live > 1) {
            int a = -1, b = -1;
            for (int i = 0; i < nodes; ++i) {
                if (!alive[i]) continue;
                if (a < 0 || weight[i] < weight[a]) { b = a; a = i; }
                else if (b < 0 || weight[i] < weight[b]) b = i;
            }
            alive[a] = alive[b] = 0;
            weight[nodes] = weight[a] + weight[b];
            parent[nodes] = -1;
            alive[nodes] = 1;
            parent[a] = parent[b] = nodes;
            ++nodes;
            --live;
        }
        int top = 0;
        for (int i = 0; i < n; ++i) {
            len[i] = 0;
            if (leaf[i] < 0) continue;
            int depth = 0;
            for (int k = leaf[i]; parent[k] >= 0; k = parent[k]) ++depth;
            len[i] = (u8)depth;
            if (depth > top) top = depth;
        }
        if (top <= limit) return;
        for (int i = 0; i < n; ++i)
            if (freq[i]) freq[i] = (freq[i] + 1) / 2;
    }
}

static void canonical(const u8 *len, int n, u16 *code) {
    int count[16] = {0}, next[16];
    for (int i = 0; i < n; ++i) count[len[i]]++;
    count[0] = 0;
    int c = 0;
    for (int l = 1; l < 16; ++l) { c = (c + count[l - 1]) << 1; next[l] = c; }
    for (int i = 0; i < n; ++i)
        code[i] = len[i] ? (u16)reverse((u32)next[len[i]]++, len[i]) : 0;
}

struct rle { u8 sym, extra; };

static int run_lengths(const u8 *len, int n, struct rle *out) {
    int k = 0;
    for (int i = 0; i < n;) {
        int run = 1;
        while (i + run < n && len[i + run] == len[i]) ++run;
        if (!len[i] && run >= 3) {
            int r = run > 138 ? 138 : run;
            if (r >= 11) out[k++] = (struct rle){18, (u8)(r - 11)};
            else out[k++] = (struct rle){17, (u8)(r - 3)};
            i += r;
        } else if (len[i] && run >= 4) {
            out[k++] = (struct rle){len[i], 0};
            int r = run - 1 > 6 ? 6 : run - 1;
            out[k++] = (struct rle){16, (u8)(r - 3)};
            i += r + 1;
        } else {
            out[k++] = (struct rle){len[i], 0};
            ++i;
        }
    }
    return k;
}

static void emit_symbols(struct deflater *d, const u16 *lcode, const u8 *llen, const u16 *dcode, const u8 *dlen) {
    for (int i = 0; i < d->count; ++i) {
        if (!d->dist[i]) { put_bits(d, lcode[d->lit[i]], llen[d->lit[i]]); continue; }
        int lc = len_code(d->lit[i]), dc = dist_code(d->dist[i]);
        put_bits(d, lcode[257 + lc], llen[257 + lc]);
        put_bits(d, (u32)(d->lit[i] - len_base[lc]), len_extra[lc]);
        put_bits(d, dcode[dc], dlen[dc]);
        put_bits(d, (u32)(d->dist[i] - dist_base[dc]), dist_extra[dc]);
    }
    put_bits(d, lcode[256], llen[256]);
}

static void emit_block(struct deflater *d, int last) {
    u32 lf[286] = {0}, df[30] = {0};
    for (int i = 0; i < d->count; ++i) {
        if (!d->dist[i]) lf[d->lit[i]]++;
        else { lf[257 + len_code(d->lit[i])]++; df[dist_code(d->dist[i])]++; }
    }
    lf[256] = 1;
    u8 ll[286], dl[30];
    lengths_for(lf, 286, 15, ll);
    lengths_for(df, 30, 15, dl);
    int hlit = 286, hdist = 30;
    while (hlit > 257 && !ll[hlit - 1]) --hlit;
    while (hdist > 1 && !dl[hdist - 1]) --hdist;
    u8 all[316];
    memcpy(all, ll, (size_t)hlit);
    memcpy(all + hlit, dl, (size_t)hdist);
    struct rle runs[316];
    int nruns = run_lengths(all, hlit + hdist, runs);
    u32 cf[19] = {0};
    for (int i = 0; i < nruns; ++i) cf[runs[i].sym]++;
    u8 cl[19];
    lengths_for(cf, 19, 7, cl);
    static const u8 order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int hclen = 19;
    while (hclen > 4 && !cl[order[hclen - 1]]) --hclen;

    u64 extra = 0, dyn = 3 + 14 + (u64)hclen * 3, fix = 3, raw = (u64)(d->emitted - d->block_start);
    for (int i = 0; i < 29; ++i) extra += (u64)lf[257 + i] * len_extra[i];
    for (int i = 0; i < 30; ++i) extra += (u64)df[i] * dist_extra[i];
    for (int i = 0; i < nruns; ++i) dyn += cl[runs[i].sym] + (runs[i].sym == 16 ? 2 : runs[i].sym == 17 ? 3 : runs[i].sym == 18 ? 7 : 0);
    for (int i = 0; i < 286; ++i) {
        dyn += (u64)lf[i] * ll[i];
        fix += (u64)lf[i] * (i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
    }
    for (int i = 0; i < 30; ++i) { dyn += (u64)df[i] * dl[i]; fix += (u64)df[i] * 5; }
    dyn += extra;
    fix += extra;
    u64 store = (raw / 65535 + 1) * 40 + raw * 8 + 7;

    if (store <= dyn && store <= fix) {
        const u8 *p = d->win + d->block_start;
        u64 left = raw;
        do {
            u32 n = left > 65535 ? 65535 : (u32)left;
            left -= n;
            put_bits(d, last && !left ? 1 : 0, 1);
            put_bits(d, 0, 2);
            if (d->bitcount) put_bits(d, 0, 8 - d->bitcount);
            put_bits(d, n, 16);
            put_bits(d, n ^ 0xffff, 16);
            for (u32 i = 0; i < n; ++i) put_bits(d, p[i], 8);
            p += n;
        } while (left);
    } else if (fix <= dyn) {
        u8 fl[288], fd[30];
        u16 fc[288], fdc[30];
        for (int i = 0; i < 288; ++i) fl[i] = (u8)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
        for (int i = 0; i < 30; ++i) fd[i] = 5;
        canonical(fl, 288, fc);
        canonical(fd, 30, fdc);
        put_bits(d, last, 1);
        put_bits(d, 1, 2);
        emit_symbols(d, fc, fl, fdc, fd);
    } else {
        u16 lc[286], dc[30], cc[19];
        canonical(ll, 286, lc);
        canonical(dl, 30, dc);
        canonical(cl, 19, cc);
        put_bits(d, last, 1);
        put_bits(d, 2, 2);
        put_bits(d, (u32)(hlit - 257), 5);
        put_bits(d, (u32)(hdist - 1), 5);
        put_bits(d, (u32)(hclen - 4), 4);
        for (int i = 0; i < hclen; ++i) put_bits(d, cl[order[i]], 3);
        for (int i = 0; i < nruns; ++i) {
            put_bits(d, cc[runs[i].sym], cl[runs[i].sym]);
            if (runs[i].sym == 16) put_bits(d, runs[i].extra, 2);
            else if (runs[i].sym == 17) put_bits(d, runs[i].extra, 3);
            else if (runs[i].sym == 18) put_bits(d, runs[i].extra, 7);
        }
        emit_symbols(d, lc, ll, dc, dl);
    }
    d->count = 0;
    d->block_start = d->emitted;
}

static void literal(struct deflater *d, int at) {
    d->lit[d->count] = d->win[at];
    d->dist[d->count++] = 0;
    d->emitted = at + 1;
    if (d->count == SYMBOLS) emit_block(d, 0);
}

static void match(struct deflater *d, int at, int length, int distance) {
    d->lit[d->count] = (u16)length;
    d->dist[d->count++] = (u16)distance;
    d->emitted = at + length;
    if (d->count == SYMBOLS) emit_block(d, 0);
}

static unsigned hash_at(const struct deflater *d, int at) {
    return ((unsigned)d->win[at] << 10 ^ (unsigned)d->win[at + 1] << 5 ^ d->win[at + 2]) & ((1u << HASH_BITS) - 1);
}

static long long insert(struct deflater *d, int at) {
    if (at + 3 > d->end) return -1;
    unsigned h = hash_at(d, at);
    long long abs_at = d->base + at, old = d->head[h];
    d->prev[abs_at & (WSIZE - 1)] = old;
    d->head[h] = abs_at;
    return old;
}

static int longest(struct deflater *d, long long candidate) {
    int at = d->start, best = d->prev_length, chain = CHAIN;
    int limit = d->end - at < 258 ? d->end - at : 258;
    if (d->prev_length >= GOOD) chain >>= 2;
    long long floor = d->base + at - WSIZE;
    const u8 *here = d->win + at;
    while (candidate >= 0 && candidate > floor && candidate >= d->base && chain--) {
        const u8 *there = d->win + (candidate - d->base);
        if (there[best] == here[best] && there[0] == here[0] && there[1] == here[1]) {
            int n = 2;
            while (n < limit && there[n] == here[n]) ++n;
            if (n > best) {
                best = n;
                d->match_start = (int)(candidate - d->base);
                if (n >= NICE || n >= limit) break;
            }
        }
        candidate = d->prev[candidate & (WSIZE - 1)];
    }
    return best;
}

static void process(struct deflater *d, int final) {
    for (;;) {
        int lookahead = d->end - d->start;
        if (lookahead < MIN_LOOKAHEAD && !final) return;
        if (!lookahead) break;
        long long candidate = insert(d, d->start);
        d->prev_length = d->match_length;
        d->prev_match = d->match_start;
        d->match_length = 2;
        if (candidate >= 0 && d->prev_length < LAZY && d->base + d->start - candidate <= WSIZE - MIN_LOOKAHEAD) {
            int saved = d->prev_length;
            d->prev_length = 2;
            int n = longest(d, candidate);
            d->prev_length = saved;
            if (n > 2) {
                d->match_length = n;
                if (n == 3 && d->start - d->match_start > 4096) d->match_length = 2;
            }
        }
        if (d->prev_length >= 3 && d->match_length <= d->prev_length) {
            int at = d->start - 1;
            match(d, at, d->prev_length, at - d->prev_match);
            int stop = at + d->prev_length;
            for (int p = d->start + 1; p < stop; ++p) insert(d, p);
            d->start = stop;
            d->available = 0;
            d->match_length = 2;
        } else if (d->available) {
            literal(d, d->start - 1);
            ++d->start;
        } else {
            d->available = 1;
            ++d->start;
        }
    }
    if (d->available) { literal(d, d->start - 1); d->available = 0; }
}

struct deflater *deflate_new(struct sink *out) {
    struct deflater *d = malloc(sizeof(*d));
    if (!d) return 0;
    memset(d, 0, offsetof(struct deflater, win));
    d->out = out;
    for (int i = 0; i < 1 << HASH_BITS; ++i) d->head[i] = -1;
    d->match_length = d->prev_length = 2;
    d->count = 0;
    return d;
}

static void slide(struct deflater *d) {
    if (d->count) emit_block(d, 0);
    int keep_from = d->start - WSIZE - 1;
    if (d->available) --keep_from;
    if (keep_from > d->block_start) keep_from = d->block_start;
    if (keep_from <= 0) return;
    memmove(d->win, d->win + keep_from, (size_t)(d->end - keep_from));
    d->base += keep_from;
    d->start -= keep_from;
    d->end -= keep_from;
    d->emitted -= keep_from;
    d->block_start -= keep_from;
    d->match_start -= keep_from;
    d->prev_match -= keep_from;
}

int deflate_write(struct deflater *d, const u8 *data, size_t n) {
    while (n && !d->error) {
        if (d->end == BUFSIZE) slide(d);
        size_t room = (size_t)(BUFSIZE - d->end), k = n < room ? n : room;
        memcpy(d->win + d->end, data, k);
        d->end += (int)k;
        data += k;
        n -= k;
        process(d, 0);
    }
    return d->error ? -1 : 0;
}

int deflate_finish(struct deflater *d) {
    process(d, 1);
    emit_block(d, 1);
    if (d->bitcount) put_bits(d, 0, 8 - d->bitcount);
    out_flush(d);
    int r = d->error ? -1 : 0;
    free(d);
    return r;
}

void deflate_free(struct deflater *d) { free(d); }
