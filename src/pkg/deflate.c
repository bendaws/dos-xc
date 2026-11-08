/*
 * DOS-XC /pkg/deflate.c - a small DEFLATE encoder: LZ77 with hash chains
 * and the fixed Huffman code.  (Build-time packages use zlib; this lets
 * PKG PACK work on DOS-XC itself.)
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#include "dsp.h"
#include <stdlib.h>
#include <string.h>

#define WINDOW 32768
#define HASH_BITS 15
#define CHAIN 64

struct writer { unsigned char *out; size_t size, pos; unsigned long bits; unsigned count; int overflow; };

static void put(struct writer *w, unsigned long value, unsigned n) {
    w->bits |= value << w->count;
    w->count += n;
    while (w->count >= 8) {
        if (w->pos < w->size) w->out[w->pos++] = (unsigned char)w->bits;
        else w->overflow = 1;
        w->bits >>= 8;
        w->count -= 8;
    }
}

static unsigned reverse(unsigned code, unsigned n) {
    unsigned r = 0;
    while (n--) { r = r << 1 | (code & 1); code >>= 1; }
    return r;
}

static void literal(struct writer *w, unsigned symbol) {
    if (symbol < 144) put(w, reverse(0x30 + symbol, 8), 8);
    else if (symbol < 256) put(w, reverse(0x190 + symbol - 144, 9), 9);
    else if (symbol < 280) put(w, reverse(symbol - 256, 7), 7);
    else put(w, reverse(0xc0 + symbol - 280, 8), 8);
}

static const unsigned short length_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const unsigned char length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const unsigned short distance_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const unsigned char distance_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void match(struct writer *w, unsigned length, unsigned distance) {
    int l = 28;
    while (length_base[l] > length) --l;
    literal(w, 257 + (unsigned)l);
    put(w, length - length_base[l], length_extra[l]);
    int d = 29;
    while (distance_base[d] > distance) --d;
    put(w, reverse((unsigned)d, 5), 5);
    put(w, distance - distance_base[d], distance_extra[d]);
}

/* Returns the compressed size, or 0 when it would not fit in OUT_SIZE. */
size_t dsp_deflate(const unsigned char *in, size_t in_size, unsigned char *out, size_t out_size) {
    struct writer w = {out, out_size, 0, 0, 0, 0};
    int *head = malloc(sizeof(int) << HASH_BITS), *previous = malloc(sizeof(int) * WINDOW);
    if (!head || !previous) { free(head); free(previous); return 0; }
    for (int i = 0; i < 1 << HASH_BITS; ++i) head[i] = -1;
    put(&w, 1, 1);                      /* one final block */
    put(&w, 1, 2);                      /* fixed Huffman */
    size_t i = 0;
    while (i < in_size && !w.overflow) {
        unsigned best = 0, best_distance = 0;
        if (i + 3 <= in_size) {
            unsigned h = ((unsigned)in[i] << 10 ^ (unsigned)in[i + 1] << 5 ^ in[i + 2]) & ((1u << HASH_BITS) - 1);
            int candidate = head[h];
            for (int chain = 0; candidate >= 0 && chain < CHAIN && i - (size_t)candidate <= WINDOW; ++chain) {
                size_t limit = in_size - i < 258 ? in_size - i : 258, n = 0;
                while (n < limit && in[candidate + n] == in[i + n]) ++n;
                if (n > best) { best = (unsigned)n; best_distance = (unsigned)(i - (size_t)candidate); if (n == limit) break; }
                candidate = previous[candidate % WINDOW];
            }
            previous[i % WINDOW] = head[h];
            head[h] = (int)i;
        }
        if (best >= 3) {
            match(&w, best, best_distance);
            for (size_t k = 1; k < best && i + k + 3 <= in_size; ++k) {
                size_t at = i + k;
                unsigned h = ((unsigned)in[at] << 10 ^ (unsigned)in[at + 1] << 5 ^ in[at + 2]) & ((1u << HASH_BITS) - 1);
                previous[at % WINDOW] = head[h];
                head[h] = (int)at;
            }
            i += best;
        } else {
            literal(&w, in[i++]);
        }
    }
    literal(&w, 256);
    put(&w, 0, 7);                      /* flush the last byte */
    free(head);
    free(previous);
    return w.overflow ? 0 : w.pos;
}
