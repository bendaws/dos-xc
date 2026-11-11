/*
 * DOS-XC /os/pkg/inflate.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "dsp.h"
#include <string.h>

struct state {
    const unsigned char *in;
    size_t in_size, in_pos;
    unsigned char *out;
    size_t out_size, out_pos;
    unsigned bits, count;
};

struct huffman { short counts[16], symbols[288]; };

static int bits(struct state *s, int need) {
    long value = s->bits;
    while (s->count < (unsigned)need) {
        if (s->in_pos == s->in_size) return -1;
        value |= (long)s->in[s->in_pos++] << s->count;
        s->count += 8;
    }
    s->bits = (unsigned)(value >> need);
    s->count -= (unsigned)need;
    return (int)(value & ((1L << need) - 1));
}

static int build(struct huffman *h, const short *lengths, int n) {
    short offsets[16];
    memset(h->counts, 0, sizeof(h->counts));
    for (int i = 0; i < n; ++i) h->counts[lengths[i]]++;
    if (h->counts[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; ++len) {
        left <<= 1;
        left -= h->counts[len];
        if (left < 0) return -1;
    }
    offsets[1] = 0;
    for (int len = 1; len < 15; ++len) offsets[len + 1] = (short)(offsets[len] + h->counts[len]);
    for (int i = 0; i < n; ++i)
        if (lengths[i]) h->symbols[offsets[lengths[i]]++] = (short)i;
    return left;
}

static int decode(struct state *s, const struct huffman *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        int bit = bits(s, 1);
        if (bit < 0) return -1;
        code |= bit;
        int count = h->counts[len];
        if (code - count < first) return h->symbols[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const short length_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short distance_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const short distance_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int codes(struct state *s, const struct huffman *lengths, const struct huffman *distances) {
    for (;;) {
        int symbol = decode(s, lengths);
        if (symbol < 0) return -1;
        if (symbol < 256) {
            if (s->out_pos == s->out_size) return -1;
            s->out[s->out_pos++] = (unsigned char)symbol;
        } else if (symbol == 256) {
            return 0;
        } else {
            symbol -= 257;
            if (symbol >= 29) return -1;
            int extra = bits(s, length_extra[symbol]);
            if (extra < 0) return -1;
            size_t length = (size_t)(length_base[symbol] + extra);
            int d = decode(s, distances);
            if (d < 0 || d >= 30) return -1;
            extra = bits(s, distance_extra[d]);
            if (extra < 0) return -1;
            size_t distance = (size_t)(distance_base[d] + extra);
            if (distance > s->out_pos || length > s->out_size - s->out_pos) return -1;
            for (size_t i = 0; i < length; ++i, ++s->out_pos) s->out[s->out_pos] = s->out[s->out_pos - distance];
        }
    }
}

static int fixed(struct state *s) {
    static struct huffman lengths, distances;
    static int ready;
    if (!ready) {
        short l[288];
        int i = 0;
        for (; i < 144; ++i) l[i] = 8;
        for (; i < 256; ++i) l[i] = 9;
        for (; i < 280; ++i) l[i] = 7;
        for (; i < 288; ++i) l[i] = 8;
        build(&lengths, l, 288);
        for (i = 0; i < 30; ++i) l[i] = 5;
        build(&distances, l, 30);
        ready = 1;
    }
    return codes(s, &lengths, &distances);
}

static int dynamic(struct state *s) {
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short l[320];
    struct huffman lengths, distances;
    int nlen = bits(s, 5), ndist = bits(s, 5), ncode = bits(s, 4);
    if (nlen < 0 || ndist < 0 || ncode < 0) return -1;
    nlen += 257;
    ndist += 1;
    ncode += 4;
    if (nlen > 286 || ndist > 30) return -1;
    int i = 0;
    for (; i < ncode; ++i) { int b = bits(s, 3); if (b < 0) return -1; l[order[i]] = (short)b; }
    for (; i < 19; ++i) l[order[i]] = 0;
    if (build(&lengths, l, 19) != 0) return -1;
    for (i = 0; i < nlen + ndist;) {
        int symbol = decode(s, &lengths);
        if (symbol < 0) return -1;
        if (symbol < 16) { l[i++] = (short)symbol; continue; }
        int repeat, value = 0;
        if (symbol == 16) {
            if (!i) return -1;
            value = l[i - 1];
            repeat = 3 + bits(s, 2);
        } else if (symbol == 17) repeat = 3 + bits(s, 3);
        else repeat = 11 + bits(s, 7);
        if (repeat < 3 || i + repeat > nlen + ndist) return -1;
        while (repeat--) l[i++] = (short)value;
    }
    if (!l[256]) return -1;
    int r = build(&lengths, l, nlen);
    if (r < 0 || (r > 0 && nlen - lengths.counts[0] != 1)) return -1;
    r = build(&distances, l + nlen, ndist);
    if (r < 0 || (r > 0 && ndist - distances.counts[0] != 1)) return -1;
    return codes(s, &lengths, &distances);
}

static int stored(struct state *s) {
    s->bits = 0;
    s->count = 0;
    if (s->in_pos + 4 > s->in_size) return -1;
    unsigned length = s->in[s->in_pos] | s->in[s->in_pos + 1] << 8;
    unsigned check = s->in[s->in_pos + 2] | s->in[s->in_pos + 3] << 8;
    s->in_pos += 4;
    if (length != (~check & 0xffff) || s->in_pos + length > s->in_size || length > s->out_size - s->out_pos) return -1;
    memcpy(s->out + s->out_pos, s->in + s->in_pos, length);
    s->in_pos += length;
    s->out_pos += length;
    return 0;
}

/* Returns the decompressed size, or -1. */
int dsp_inflate(const unsigned char *in, size_t in_size, unsigned char *out, size_t out_size) {
    struct state s = {in, in_size, 0, out, out_size, 0, 0, 0};
    int last;
    do {
        last = bits(&s, 1);
        int type = bits(&s, 2);
        if (last < 0 || type < 0) return -1;
        int r = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : -1;
        if (r) return -1;
    } while (!last);
    return (int)s.out_pos;
}
