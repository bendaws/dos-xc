/*
 * DOS-XC /desktop/image.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * Picture files: BMP (read and write), PNG, and JPEG (baseline and
 * progressive), decoded to 32-bit surfaces, plus scaling.
 */
#include "wm.h"
#include <strings.h>

typedef unsigned char u8;
typedef unsigned short u16;
int dsp_inflate(const unsigned char *in, size_t in_size, unsigned char *out, size_t out_size);

static int surface_new(struct surface *s, int w, int h) {
    memset(s, 0, sizeof(*s));
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return -1;
    return surface_size(s, w, h);
}

/* ---- BMP ---------------------------------------------------------------- */

static u32 le32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u16 le16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }

static int decode_bmp(const u8 *d, size_t size, struct surface *out) {
    if (size < 54) return -1;
    u32 offset = le32(d + 10), header = le32(d + 14);
    int w = (int)le32(d + 18), h = (int)le32(d + 22), depth = le16(d + 28);
    u32 compression = le32(d + 30), colors = le32(d + 46);
    int top_down = h < 0;
    if (top_down) h = -h;
    if ((compression != 0 && compression != 3) || (depth != 8 && depth != 24 && depth != 32)) return -1;
    if (surface_new(out, w, h)) return -1;
    if (!colors) colors = 256;
    const u8 *palette = d + 14 + header;
    size_t stride = ((size_t)w * depth / 8 + 3) & ~(size_t)3;
    if (offset + stride * h > size) { free(out->px); out->px = 0; return -1; }
    for (int y = 0; y < h; ++y) {
        const u8 *row = d + offset + stride * (top_down ? y : h - 1 - y);
        u32 *to = out->px + y * w;
        for (int x = 0; x < w; ++x) {
            if (depth == 8) {
                unsigned n = row[x];
                const u8 *c = palette + (n < colors ? n : 0) * 4;
                to[x] = (u32)c[2] << 16 | (u32)c[1] << 8 | c[0];
            } else {
                const u8 *c = row + x * (depth / 8);
                to[x] = (u32)c[2] << 16 | (u32)c[1] << 8 | c[0];
            }
        }
    }
    return 0;
}

int image_save_bmp(const char *path, const struct surface *s) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t stride = ((size_t)s->w * 3 + 3) & ~(size_t)3, body = stride * s->h;
    u8 head[54] = {'B', 'M'};
    u32 total = (u32)(body + 54);
    memcpy(head + 2, &total, 4);
    head[10] = 54;
    head[14] = 40;
    memcpy(head + 18, &s->w, 4);
    memcpy(head + 22, &s->h, 4);
    head[26] = 1;
    head[28] = 24;
    int ok = fwrite(head, 1, 54, f) == 54;
    u8 *row = calloc(1, stride);
    if (!row) ok = 0;
    for (int y = s->h - 1; ok && y >= 0; --y) {
        const u32 *from = s->px + y * s->w;
        for (int x = 0; x < s->w; ++x) {
            row[x * 3] = (u8)from[x];
            row[x * 3 + 1] = (u8)(from[x] >> 8);
            row[x * 3 + 2] = (u8)(from[x] >> 16);
        }
        if (fwrite(row, 1, stride, f) != stride) ok = 0;
    }
    free(row);
    if (fclose(f)) ok = 0;
    return ok ? 0 : -1;
}

/* ---- PNG ---------------------------------------------------------------- */

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

static int decode_png(const u8 *d, size_t size, struct surface *out) {
    if (size < 33 || memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return -1;
    int w = 0, h = 0, depth = 0, type = 0;
    u8 palette[768];
    memset(palette, 0, sizeof(palette));
    u8 *packed = malloc(size);
    if (!packed) return -1;
    size_t packed_size = 0;
    for (size_t at = 8; at + 12 <= size;) {
        u32 length = be32(d + at);
        const u8 *tag = d + at + 4, *data = d + at + 8;
        if (length > size - at - 12) break;
        if (!memcmp(tag, "IHDR", 4) && length >= 13) {
            w = (int)be32(data);
            h = (int)be32(data + 4);
            depth = data[8];
            type = data[9];
            if (data[12]) { free(packed); return -1; }   /* interlaced */
        } else if (!memcmp(tag, "PLTE", 4)) {
            memcpy(palette, data, length < 768 ? length : 768);
        } else if (!memcmp(tag, "IDAT", 4)) {
            memcpy(packed + packed_size, data, length);
            packed_size += length;
        } else if (!memcmp(tag, "IEND", 4)) break;
        at += 12 + (size_t)length;
    }
    int channels = type == 0 ? 1 : type == 2 ? 3 : type == 3 ? 1 : type == 4 ? 2 : type == 6 ? 4 : 0;
    if (!channels || (depth != 8 && depth != 16) || packed_size < 6 || surface_new(out, w, h)) { free(packed); return -1; }
    size_t bytes = (size_t)channels * (depth / 8), stride = (size_t)w * bytes + 1, raw_size = stride * h;
    u8 *raw = malloc(raw_size);
    if (!raw || dsp_inflate(packed + 2, packed_size - 2, raw, raw_size) < 0) {
        free(raw); free(packed); free(out->px); out->px = 0;
        return -1;
    }
    free(packed);
    for (int y = 0; y < h; ++y) {
        u8 *row = raw + y * stride, *cur = row + 1;
        const u8 *above = y ? raw + (y - 1) * stride + 1 : 0;
        int filter = row[0];
        for (size_t i = 0; i < stride - 1; ++i) {
            int a = i >= bytes ? cur[i - bytes] : 0, b = above ? above[i] : 0;
            int c = above && i >= bytes ? above[i - bytes] : 0, add = 0;
            switch (filter) {
            case 1: add = a; break;
            case 2: add = b; break;
            case 3: add = (a + b) / 2; break;
            case 4: {
                int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                add = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
                break;
            }
            }
            cur[i] = (u8)(cur[i] + add);
        }
        u32 *to = out->px + y * w;
        for (int x = 0; x < w; ++x) {
            const u8 *p = cur + x * bytes;
            int step = depth / 8;
            if (type == 3) to[x] = (u32)palette[p[0] * 3] << 16 | (u32)palette[p[0] * 3 + 1] << 8 | palette[p[0] * 3 + 2];
            else if (type == 0 || type == 4) to[x] = (u32)p[0] * 0x010101;
            else to[x] = (u32)p[0] << 16 | (u32)p[step] << 8 | p[step * 2];
        }
    }
    free(raw);
    return 0;
}

/* ---- JPEG ---------------------------------------------------------------- */

#define FAST_BITS 9
struct huffman {
    u16 fast[1 << FAST_BITS];     /* length << 8 | symbol; 0 = longer code */
    int min_code[18], max_code[18], first[18];
    u8 symbols[256];
};
struct component { int id, h, v, quant, dc, ac, blocks_w, blocks_h, pred; short *coef; u8 *plane; };
struct jpeg {
    const u8 *at, *end;
    u32 bits;
    int count, eob_run;
    int width, height, h_max, v_max, mcu_w, mcu_h, components, progressive, restart;
    u16 quant[4][64];
    struct huffman dc[4], ac[4];
    struct component c[3];
};

static const u8 zigzag[80] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21,
    28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61,
    54, 47, 55, 62, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63};

static void fill_bits(struct jpeg *j) {
    while (j->count <= 24) {
        int byte = 0;
        if (j->at < j->end && *j->at != 0xff) byte = *j->at++;
        else if (j->at + 1 < j->end && !j->at[1]) { byte = 0xff; j->at += 2; }
        /* a marker: feed zeros until the caller deals with it */
        j->bits |= (u32)byte << (24 - j->count);
        j->count += 8;
    }
}

static int get_bits(struct jpeg *j, int n) {
    if (!n) return 0;
    if (j->count < n) fill_bits(j);
    int v = (int)(j->bits >> (32 - n));
    j->bits <<= n;
    j->count -= n;
    return v;
}

static int decode_symbol(struct jpeg *j, const struct huffman *h) {
    if (j->count < 16) fill_bits(j);
    int entry = h->fast[j->bits >> (32 - FAST_BITS)];
    if (entry) {
        int length = entry >> 8;
        j->bits <<= length;
        j->count -= length;
        return entry & 255;
    }
    for (int length = FAST_BITS + 1; length <= 16; ++length) {
        int code = (int)(j->bits >> (32 - length));
        if (code <= h->max_code[length]) {
            j->bits <<= length;
            j->count -= length;
            return h->symbols[h->first[length] + code - h->min_code[length]];
        }
    }
    j->bits <<= 16;                     /* damaged data */
    j->count -= 16;
    return 0;
}

static int extend(int value, int bits) { return bits && value < 1 << (bits - 1) ? value - (1 << bits) + 1 : value; }

static void build_huffman(struct huffman *h, const u8 *counts) {
    memset(h->fast, 0, sizeof(h->fast));
    int code = 0, k = 0;
    for (int length = 1; length <= 16; ++length) {
        h->first[length] = k;
        h->min_code[length] = code;
        for (int i = 0; i < counts[length - 1]; ++i, ++k, ++code)
            if (length <= FAST_BITS) {
                int at = code << (FAST_BITS - length);
                for (int n = 0; n < 1 << (FAST_BITS - length); ++n) h->fast[at + n] = (u16)(length << 8 | h->symbols[k]);
            }
        h->max_code[length] = counts[length - 1] ? code - 1 : -1;
        code <<= 1;
    }
}

static void block_baseline(struct jpeg *j, struct component *c, short *b) {
    int t = decode_symbol(j, &j->dc[c->dc]);
    c->pred += extend(get_bits(j, t), t);
    b[0] = (short)c->pred;
    for (int k = 1; k < 64;) {
        int rs = decode_symbol(j, &j->ac[c->ac]), r = rs >> 4, s = rs & 15;
        if (!s) { if (r < 15) break; k += 16; continue; }
        k += r;
        b[zigzag[k++]] = (short)extend(get_bits(j, s), s);
    }
}

static void block_dc(struct jpeg *j, struct component *c, short *b, int high, int low) {
    if (!high) {
        int t = decode_symbol(j, &j->dc[c->dc]);
        c->pred += extend(get_bits(j, t), t);
        b[0] = (short)(c->pred * (1 << low));
    } else if (get_bits(j, 1)) b[0] |= (short)(1 << low);
}

static void block_ac_first(struct jpeg *j, struct component *c, short *b, int ss, int se, int low) {
    if (j->eob_run) { --j->eob_run; return; }
    for (int k = ss; k <= se;) {
        int rs = decode_symbol(j, &j->ac[c->ac]), r = rs >> 4, s = rs & 15;
        if (!s) {
            if (r < 15) {
                j->eob_run = (1 << r) - 1;
                if (r) j->eob_run += get_bits(j, r);
                return;
            }
            k += 16;
        } else {
            k += r;
            b[zigzag[k++]] = (short)(extend(get_bits(j, s), s) * (1 << low));
        }
    }
}

static void refine(struct jpeg *j, short *p, int bit) {
    if (get_bits(j, 1) && !(*p & bit)) *p += (short)(*p > 0 ? bit : -bit);
}

static void block_ac_refine(struct jpeg *j, struct component *c, short *b, int ss, int se, int low) {
    int bit = 1 << low, k = ss;
    if (j->eob_run) {
        --j->eob_run;
        for (; k <= se; ++k)
            if (b[zigzag[k]]) refine(j, &b[zigzag[k]], bit);
        return;
    }
    while (k <= se) {
        int rs = decode_symbol(j, &j->ac[c->ac]), r = rs >> 4, s = rs & 15, value = 0;
        if (!s) {
            if (r < 15) {
                j->eob_run = (1 << r) - 1;
                if (r) j->eob_run += get_bits(j, r);
                r = 64;
            }
        } else value = get_bits(j, 1) ? bit : -bit;
        while (k <= se) {
            short *p = &b[zigzag[k++]];
            if (*p) refine(j, p, bit);
            else {
                if (!r) { *p = (short)value; break; }
                --r;
            }
        }
    }
}

static void decode_block(struct jpeg *j, struct component *c, short *b, int ss, int se, int high, int low) {
    if (!j->progressive) block_baseline(j, c, b);
    else if (!ss) block_dc(j, c, b, high, low);
    else if (!high) block_ac_first(j, c, b, ss, se, low);
    else block_ac_refine(j, c, b, ss, se, low);
}

/* Reset the bit reader at a restart marker. */
static void restart_scan(struct jpeg *j) {
    j->bits = 0;
    j->count = 0;
    j->eob_run = 0;
    for (int i = 0; i < j->components; ++i) j->c[i].pred = 0;
    while (j->at + 1 < j->end && !(j->at[0] == 0xff && j->at[1] >= 0xd0 && j->at[1] <= 0xd7)) {
        if (j->at[0] == 0xff && j->at[1] && j->at[1] != 0xff) return;   /* another marker: leave it */
        ++j->at;
    }
    if (j->at + 1 < j->end) j->at += 2;
}

static void decode_scan(struct jpeg *j, struct component **scan, int count, int ss, int se, int high, int low) {
    j->bits = 0;
    j->count = 0;
    j->eob_run = 0;
    for (int i = 0; i < j->components; ++i) j->c[i].pred = 0;
    int until_restart = j->restart;
    if (count == 1) {                   /* not interleaved: blocks of this component only */
        struct component *c = scan[0];
        int w = ((j->width * c->h + j->h_max - 1) / j->h_max + 7) / 8;
        int h = ((j->height * c->v + j->v_max - 1) / j->v_max + 7) / 8;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                decode_block(j, c, c->coef + 64 * (y * c->blocks_w + x), ss, se, high, low);
                if (j->restart && !--until_restart) { restart_scan(j); until_restart = j->restart; }
            }
        return;
    }
    for (int my = 0; my < j->mcu_h; ++my)
        for (int mx = 0; mx < j->mcu_w; ++mx) {
            for (int i = 0; i < count; ++i) {
                struct component *c = scan[i];
                for (int v = 0; v < c->v; ++v)
                    for (int h = 0; h < c->h; ++h)
                        decode_block(j, c, c->coef + 64 * ((my * c->v + v) * c->blocks_w + mx * c->h + h), ss, se, high, low);
            }
            if (j->restart && !--until_restart) { restart_scan(j); until_restart = j->restart; }
        }
}

/* Integer inverse DCT, fixed point with 12 fractional bits. */
#define F(x) ((int)((x) * 4096 + 0.5))
static u8 clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (u8)v; }

static void idct_1d(const int *in, int step, int *out) {
    int s0 = in[0], s1 = in[step], s2 = in[2 * step], s3 = in[3 * step];
    int s4 = in[4 * step], s5 = in[5 * step], s6 = in[6 * step], s7 = in[7 * step];
    int p1 = (s2 + s6) * F(0.5411961);
    int t2 = p1 + s6 * F(-1.847759065), t3 = p1 + s2 * F(0.765366865);
    int t0 = (s0 + s4) * 4096, t1 = (s0 - s4) * 4096;
    int x0 = t0 + t3, x3 = t0 - t3, x1 = t1 + t2, x2 = t1 - t2;
    int a0 = s7, a1 = s5, a2 = s3, a3 = s1;
    int p3 = a0 + a2, p4 = a1 + a3, p1b = a0 + a3, p2 = a1 + a2, p5 = (p3 + p4) * F(1.175875602);
    a0 *= F(0.298631336); a1 *= F(2.053119869); a2 *= F(3.072711026); a3 *= F(1.501321110);
    p1b *= F(-0.899976223); p2 *= F(-2.562915447); p3 *= F(-1.961570560); p4 *= F(-0.390180644);
    p3 += p5; p4 += p5;
    a0 += p1b + p3; a1 += p2 + p4; a2 += p2 + p3; a3 += p1b + p4;
    out[0] = x0 + a3; out[7] = x0 - a3;
    out[1] = x1 + a2; out[6] = x1 - a2;
    out[2] = x2 + a1; out[5] = x2 - a1;
    out[3] = x3 + a0; out[4] = x3 - a0;
}

static void idct_block(const short *coef, const u16 *quant, u8 *out, int stride) {
    int v[64], w[8];
    for (int i = 0; i < 64; ++i) v[i] = coef[i] * quant[i];
    for (int x = 0; x < 8; ++x) {
        int col[8];
        for (int y = 0; y < 8; ++y) col[y] = v[y * 8 + x];
        idct_1d(col, 1, w);
        for (int y = 0; y < 8; ++y) v[y * 8 + x] = (w[y] + 512) >> 10;
    }
    for (int y = 0; y < 8; ++y) {
        idct_1d(v + y * 8, 1, w);
        for (int x = 0; x < 8; ++x) out[y * stride + x] = clamp8(((w[x] + (1 << 16)) >> 17) + 128);
    }
}

static int decode_jpeg(const u8 *d, size_t size, struct surface *out) {
    if (size < 4 || d[0] != 0xff || d[1] != 0xd8) return -1;
    struct jpeg *j = calloc(1, sizeof(*j));
    if (!j) return -1;
    int result = -1, have_frame = 0;
    j->at = d + 2;
    j->end = d + size;
    for (;;) {
        while (j->at + 1 < j->end && (j->at[0] != 0xff || !j->at[1] || j->at[1] == 0xff)) ++j->at;
        if (j->at + 4 > j->end) break;
        int marker = j->at[1];
        if (marker == 0xd9) break;
        j->at += 2;
        if (marker >= 0xd0 && marker <= 0xd7) continue;
        int length = j->at[0] << 8 | j->at[1];
        const u8 *p = j->at + 2, *stop = j->at + length;
        if (stop > j->end) break;
        if (marker == 0xdb) {
            while (p < stop) {
                int precision = *p >> 4, id = *p++ & 3;
                for (int i = 0; i < 64 && p < stop; ++i) {
                    int value = *p++;
                    if (precision) value = value << 8 | *p++;
                    j->quant[id][zigzag[i]] = (u16)value;
                }
            }
        } else if (marker == 0xc4) {
            while (p + 17 <= stop) {
                int ac = *p >> 4, id = *p++ & 3, total = 0;
                struct huffman *h = ac ? &j->ac[id] : &j->dc[id];
                for (int i = 0; i < 16; ++i) total += p[i];
                if (total > 256 || p + 16 + total > stop) goto done;
                memcpy(h->symbols, p + 16, (size_t)total);
                build_huffman(h, p);
                p += 16 + total;
            }
        } else if (marker == 0xc0 || marker == 0xc1 || marker == 0xc2) {
            if (p[0] != 8 || have_frame) goto done;
            j->progressive = marker == 0xc2;
            j->height = p[1] << 8 | p[2];
            j->width = p[3] << 8 | p[4];
            j->components = p[5];
            if ((j->components != 1 && j->components != 3) || !j->width || !j->height) goto done;
            for (int i = 0; i < j->components; ++i) {
                struct component *c = &j->c[i];
                c->id = p[6 + i * 3];
                c->h = j->components == 1 ? 1 : p[7 + i * 3] >> 4;
                c->v = j->components == 1 ? 1 : p[7 + i * 3] & 15;
                c->quant = p[8 + i * 3] & 3;
                if (!c->h || !c->v || c->h > 4 || c->v > 4) goto done;
                if (c->h > j->h_max) j->h_max = c->h;
                if (c->v > j->v_max) j->v_max = c->v;
            }
            j->mcu_w = (j->width + 8 * j->h_max - 1) / (8 * j->h_max);
            j->mcu_h = (j->height + 8 * j->v_max - 1) / (8 * j->v_max);
            for (int i = 0; i < j->components; ++i) {
                struct component *c = &j->c[i];
                c->blocks_w = j->mcu_w * c->h;
                c->blocks_h = j->mcu_h * c->v;
                c->coef = calloc((size_t)c->blocks_w * c->blocks_h, 64 * sizeof(short));
                c->plane = malloc((size_t)c->blocks_w * c->blocks_h * 64);
                if (!c->coef || !c->plane) goto done;
            }
            have_frame = 1;
        } else if (marker >= 0xc3 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
            goto done;                  /* lossless, arithmetic */
        } else if (marker == 0xdd) {
            j->restart = p[0] << 8 | p[1];
        } else if (marker == 0xda) {
            if (!have_frame) goto done;
            int count = *p++;
            struct component *scan[4];
            if (count < 1 || count > j->components) goto done;
            for (int i = 0; i < count; ++i) {
                int id = *p++, tables = *p++;
                scan[i] = 0;
                for (int n = 0; n < j->components; ++n)
                    if (j->c[n].id == id) scan[i] = &j->c[n];
                if (!scan[i]) goto done;
                scan[i]->dc = tables >> 4 & 3;
                scan[i]->ac = tables & 3;
            }
            int ss = p[0], se = p[1], high = p[2] >> 4, low = p[2] & 15;
            if (!j->progressive) { ss = 0; se = 63; high = low = 0; }
            j->at = stop;
            decode_scan(j, scan, count, ss, se > 63 ? 63 : se, high, low);
            /* find the next marker */
            while (j->at + 1 < j->end && !(j->at[0] == 0xff && j->at[1] && j->at[1] != 0xff && (j->at[1] < 0xd0 || j->at[1] > 0xd7))) ++j->at;
            continue;
        }
        j->at = stop;
    }
    if (!have_frame || surface_new(out, j->width, j->height)) goto done;
    for (int i = 0; i < j->components; ++i) {
        struct component *c = &j->c[i];
        for (int y = 0; y < c->blocks_h; ++y)
            for (int x = 0; x < c->blocks_w; ++x)
                idct_block(c->coef + 64 * (y * c->blocks_w + x), j->quant[c->quant],
                           c->plane + y * 8 * c->blocks_w * 8 + x * 8, c->blocks_w * 8);
    }
    for (int y = 0; y < j->height; ++y) {
        u32 *to = out->px + y * j->width;
        for (int x = 0; x < j->width; ++x) {
            int s[3];
            for (int i = 0; i < j->components; ++i) {
                const struct component *c = &j->c[i];
                s[i] = c->plane[(y * c->v / j->v_max) * c->blocks_w * 8 + x * c->h / j->h_max];
            }
            if (j->components == 1) { to[x] = (u32)s[0] * 0x010101; continue; }
            int cb = s[1] - 128, cr = s[2] - 128;
            int r = s[0] + (91881 * cr >> 16), g = s[0] - ((22554 * cb + 46802 * cr) >> 16), b = s[0] + (116130 * cb >> 16);
            to[x] = (u32)clamp8(r) << 16 | (u32)clamp8(g) << 8 | clamp8(b);
        }
    }
    result = 0;
done:
    for (int i = 0; i < 3; ++i) { free(j->c[i].coef); free(j->c[i].plane); }
    free(j);
    return result;
}

/* ---- loading and scaling ------------------------------------------------ */

int image_load(const char *path, struct surface *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *data = size > 0 ? malloc((size_t)size) : 0;
    int r = -1;
    if (data && fread(data, 1, (size_t)size, f) == (size_t)size) {
        if (size > 2 && data[0] == 'B' && data[1] == 'M') r = decode_bmp(data, (size_t)size, out);
        else if (size > 8 && data[0] == 0x89 && data[1] == 'P') r = decode_png(data, (size_t)size, out);
        else if (size > 2 && data[0] == 0xff && data[1] == 0xd8) r = decode_jpeg(data, (size_t)size, out);
    }
    free(data);
    fclose(f);
    return r;
}

int image_is_picture(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".BMP") || !strcasecmp(dot, ".PNG") || !strcasecmp(dot, ".JPG") || !strcasecmp(dot, ".JPEG"));
}

/* Bilinear scaling of the part of FROM at (sx, sy, sw, sh) to the whole of TO. */
static void scale_part(const struct surface *from, int sx, int sy, int sw, int sh, struct surface *to) {
    int shrink_x = sw / (to->w ? to->w : 1), shrink_y = sh / (to->h ? to->h : 1);
    for (int y = 0; y < to->h; ++y) {
        u32 *out = to->px + y * to->w;
        long long fy = (long long)y * (sh - 1) * 256 / (to->h > 1 ? to->h - 1 : 1);
        int yi = sy + (int)(fy >> 8), yf = (int)fy & 255;
        for (int x = 0; x < to->w; ++x) {
            long long fx = (long long)x * (sw - 1) * 256 / (to->w > 1 ? to->w - 1 : 1);
            int xi = sx + (int)(fx >> 8), xf = (int)fx & 255;
            if (shrink_x >= 3 || shrink_y >= 3) { out[x] = from->px[yi * from->w + xi]; continue; }
            const u32 *row = from->px + yi * from->w;
            const u32 *next = yi + 1 < from->h ? row + from->w : row;
            int x2 = xi + 1 < from->w ? xi + 1 : xi;
            u32 a = row[xi], b = row[x2], c = next[xi], d = next[x2], px = 0;
            for (int shift = 0; shift <= 16; shift += 8) {
                int top = ((int)(a >> shift & 255) * (256 - xf) + (int)(b >> shift & 255) * xf) >> 8;
                int low = ((int)(c >> shift & 255) * (256 - xf) + (int)(d >> shift & 255) * xf) >> 8;
                px |= (u32)((top * (256 - yf) + low * yf) >> 8) << shift;
            }
            out[x] = px;
        }
    }
}

/* Fill TO, cropping FROM to the same shape. */
void image_cover(const struct surface *from, struct surface *to) {
    int sw = from->w, sh = from->h;
    if ((long long)from->w * to->h > (long long)from->h * to->w) sw = (int)((long long)from->h * to->w / to->h);
    else sh = (int)((long long)from->w * to->h / to->w);
    scale_part(from, (from->w - sw) / 2, (from->h - sh) / 2, sw, sh, to);
}

void image_scale(const struct surface *from, struct surface *to) { scale_part(from, 0, 0, from->w, from->h, to); }
