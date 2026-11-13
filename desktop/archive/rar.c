/*
 * DOS-XC /desktop/archive/rar.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "arc.h"
#include <stdlib.h>
#include <string.h>

struct bits { struct src *in; u64 buf; int n; u64 used, total; };

static void bits_open(struct bits *b, struct src *in, u64 bytes) { b->in = in; b->buf = 0; b->n = 0; b->used = 0; b->total = bytes * 8; }

static inline u32 peek(struct bits *b, int k) {
    while (b->n < k) {
        int c = src_byte(b->in);
        b->buf = b->buf << 8 | (u64)(c < 0 ? 0 : c);
        b->n += 8;
    }
    return (u32)((b->buf >> (b->n - k)) & ((1ull << k) - 1));
}
static inline void skip(struct bits *b, int k) { b->n -= k; b->used += (u64)k; }
static inline u32 take(struct bits *b, int k) { if (!k) return 0; u32 v = peek(b, k); skip(b, k); return v; }
static u64 take_long(struct bits *b, int k) {
    if (k <= 32) return take(b, k);
    u64 high = take(b, k - 32);
    return high << 32 | take(b, 32);
}
static void align(struct bits *b) { int k = (int)((8 - (b->used & 7)) & 7); if (k) take(b, k); }
static int exhausted(const struct bits *b) { return b->used >= b->total; }

#define HBITS 15
struct huff { u16 sym[1 << HBITS]; u8 len[1 << HBITS]; };

static void make_table(struct huff *h, const u8 *lengths, int n) {
    int count[16] = {0};
    for (int i = 0; i < n; ++i) count[lengths[i] & 15]++;
    count[0] = 0;
    u32 code = 0, start[16];
    for (int l = 1; l < 16; ++l) { start[l] = code; code = (code + (u32)count[l]) << 1; }
    int first = 0;
    for (int l = 1, done = 0; l < 16 && !done; ++l)
        for (int i = 0; i < n; ++i)
            if ((lengths[i] & 15) == l) { first = i; done = 1; break; }
    for (u32 i = 0; i < (1u << HBITS); ++i) { h->sym[i] = (u16)first; h->len[i] = HBITS; }
    for (int l = 1; l < 16; ++l)
        for (int i = 0; i < n; ++i) {
            if ((lengths[i] & 15) != l) continue;
            u32 c = start[l]++;
            if (c >= (1u << l)) continue;
            u32 from = c << (HBITS - l), to = from + (1u << (HBITS - l));
            for (u32 k = from; k < to; ++k) { h->sym[k] = (u16)i; h->len[k] = (u8)l; }
        }
}

static inline int decode(struct bits *b, const struct huff *h) {
    u32 v = peek(b, HBITS);
    skip(b, h->len[v]);
    return h->sym[v];
}

#define FILTERS 8192
enum { F_DELTA, F_E8, F_E8E9, F_ARM, F_RGB, F_AUDIO, F_ITANIUM, F_NONE };

struct filter { u64 start; u32 length; int type; u32 r[7]; int used; };

struct rar {
    struct job *job;
    u8 *win;
    u64 size, mask;
    u64 unp, wr;                      /* absolute: decoded, written */
    u64 file_start, file_size;
    int writing;
    struct bits b;
    struct huff ld, dd, ldd, rd, bd;
    int tables, extra_dist, solid_archive;
    u64 old[4];
    u32 last_length;
    struct filter filters[FILTERS];
    int nfilters;
    /* RAR 2.9 */
    u8 old_table[404];
    u32 prev_low, low_rep;
    struct { int type; u32 length; } slots[1024];
    int nslots, last_slot;
    int block_ppm;
};

static int write_out(struct rar *r, const u8 *data, u64 from, size_t n) {
    u64 end = r->file_start + r->file_size;
    if (from >= end || !r->writing) return 0;
    if (from + n > end) n = (size_t)(end - from);
    if (from < r->file_start) {
        u64 cut = r->file_start - from;
        if (cut >= n) return 0;
        data += cut;
        n -= (size_t)cut;
    }
    return out_write(r->job, data, n);
}

static int write_window(struct rar *r, u64 to) {
    while (r->wr < to) {
        u64 at = r->wr & r->mask;
        size_t n = (size_t)(to - r->wr);
        if (n > r->size - at) n = (size_t)(r->size - at);
        if (write_out(r, r->win + at, r->wr, n)) return -1;
        r->wr += n;
    }
    return 0;
}

static u32 get_le32(const u8 *p) { return get32(p); }
static void put_le32(u8 *p, u32 v) { put32(p, v); }

static u32 itanium_get(const u8 *d, u32 pos, u32 count) {
    u32 at = pos / 8, bit = pos & 7;
    u32 v = d[at] | (u32)d[at + 1] << 8 | (u32)d[at + 2] << 16 | (u32)d[at + 3] << 24;
    return (v >> bit) & (0xffffffffu >> (32 - count));
}

static void itanium_set(u8 *d, u32 value, u32 pos, u32 count) {
    u32 at = pos / 8, bit = pos & 7;
    u32 and_mask = ~((0xffffffffu >> (32 - count)) << bit);
    value <<= bit;
    for (int i = 0; i < 4; ++i) {
        d[at + i] &= (u8)and_mask;
        d[at + i] |= (u8)value;
        and_mask = (and_mask >> 8) | 0xff000000u;
        value >>= 8;
    }
}

/* Runs F on DATA (LEN bytes, with LEN more after it for output); returns the output. */
static u8 *run_filter(struct filter *f, u8 *data, u32 len, u32 offset, int rar5) {
    switch (f->type) {
    case F_E8: case F_E8E9: {
        const u32 file_size = 0x1000000;
        u8 second = f->type == F_E8E9 ? 0xe9 : 0xe8;
        if (len < 4 + (rar5 ? 0 : 1)) return data;
        for (u32 pos = 0; pos + 4 < len;) {
            u8 c = data[pos++];
            if (c == 0xe8 || c == second) {
                u32 at = rar5 ? (pos + offset) % file_size : pos + offset;
                u32 addr = get_le32(data + pos);
                if (addr & 0x80000000u) { if (!((addr + at) & 0x80000000u)) put_le32(data + pos, addr + file_size); }
                else if ((addr - file_size) & 0x80000000u) put_le32(data + pos, addr - at);
                pos += 4;
            }
        }
        return data;
    }
    case F_ARM:
        for (u32 pos = 0; pos + 3 < len; pos += 4) {
            u8 *d = data + pos;
            if (d[3] == 0xeb) {
                u32 v = d[0] | (u32)d[1] << 8 | (u32)d[2] << 16;
                v -= (offset + pos) / 4;
                d[0] = (u8)v;
                d[1] = (u8)(v >> 8);
                d[2] = (u8)(v >> 16);
            }
        }
        return data;
    case F_DELTA: {
        u32 channels = rar5 ? f->r[0] : f->r[0], src = 0;
        if (!channels) return 0;
        u8 *out = data + len;
        for (u32 c = 0; c < channels; ++c) {
            u8 previous = 0;
            for (u32 d = c; d < len; d += channels) out[d] = previous = (u8)(previous - data[src++]);
        }
        return out;
    }
    case F_RGB: {
        u32 width = f->r[0] - 3, pos_r = f->r[1];
        if (len < 3 || width > len || pos_r > 2) return 0;
        u8 *src = data, *out = data + len;
        for (u32 c = 0; c < 3; ++c) {
            u32 previous = 0;
            for (u32 i = c; i < len; i += 3) {
                u32 predicted;
                if (i >= width + 3) {
                    u8 *upper = out + i - width;
                    u32 up = *upper, up_left = *(upper - 3);
                    predicted = previous + up - up_left;
                    int pa = abs((int)(predicted - previous)), pb = abs((int)(predicted - up)), pc = abs((int)(predicted - up_left));
                    if (pa <= pb && pa <= pc) predicted = previous;
                    else if (pb <= pc) predicted = up;
                    else predicted = up_left;
                } else predicted = previous;
                out[i] = (u8)(previous = (u8)(predicted - *src++));
            }
        }
        for (u32 i = pos_r; i + 2 < len; i += 3) {
            u8 g = out[i + 1];
            out[i] += g;
            out[i + 2] += g;
        }
        return out;
    }
    case F_AUDIO: {
        u32 channels = f->r[0];
        if (!channels || channels > 128) return 0;
        u8 *src = data, *out = data + len;
        for (u32 c = 0; c < channels; ++c) {
            u32 previous = 0, prev_delta = 0, dif[7] = {0};
            int d1 = 0, d2 = 0, d3, k1 = 0, k2 = 0, k3 = 0;
            for (u32 i = c, count = 0; i < len; i += channels, ++count) {
                d3 = d2;
                d2 = (int)prev_delta - d1;
                d1 = (int)prev_delta;
                u32 predicted = 8 * previous + (u32)(k1 * d1 + k2 * d2 + k3 * d3);
                predicted = (predicted >> 3) & 0xff;
                u32 cur = *src++;
                predicted -= cur;
                out[i] = (u8)predicted;
                prev_delta = (u32)(signed char)(predicted - previous);
                previous = predicted & 0xff;
                int d = (signed char)cur * 8;
                dif[0] += (u32)abs(d);
                dif[1] += (u32)abs(d - d1);
                dif[2] += (u32)abs(d + d1);
                dif[3] += (u32)abs(d - d2);
                dif[4] += (u32)abs(d + d2);
                dif[5] += (u32)abs(d - d3);
                dif[6] += (u32)abs(d + d3);
                if (!(count & 0x1f)) {
                    u32 min = dif[0], which = 0;
                    dif[0] = 0;
                    for (u32 j = 1; j < 7; ++j) {
                        if (dif[j] < min) { min = dif[j]; which = j; }
                        dif[j] = 0;
                    }
                    switch (which) {
                    case 1: if (k1 >= -16) --k1; break;
                    case 2: if (k1 < 16) ++k1; break;
                    case 3: if (k2 >= -16) --k2; break;
                    case 4: if (k2 < 16) ++k2; break;
                    case 5: if (k3 >= -16) --k3; break;
                    case 6: if (k3 < 16) ++k3; break;
                    }
                }
            }
        }
        return out;
    }
    case F_ITANIUM: {
        if (len < 21) return data;
        static const u8 masks[16] = {4, 4, 6, 6, 0, 0, 7, 7, 4, 4, 0, 0, 4, 4, 0, 0};
        u32 file_offset = offset >> 4;
        u8 *d = data;
        for (u32 pos = 0; pos + 21 < len; pos += 16, d += 16, ++file_offset) {
            int byte = (d[0] & 0x1f) - 0x10;
            if (byte < 0) continue;
            u8 cmd = masks[byte];
            if (!cmd) continue;
            for (u32 i = 0; i <= 2; ++i)
                if (cmd & (1u << i)) {
                    u32 start = i * 41 + 5;
                    if (itanium_get(d, start + 37, 4) == 5) {
                        u32 off = itanium_get(d, start + 13, 20);
                        itanium_set(d, (off - file_offset) & 0xfffff, start + 13, 20);
                    }
                }
        }
        return data;
    }
    }
    return data;
}

/* Write what is decoded, through the filters whose blocks are complete. */
static int flush(struct rar *r, int final, int rar5) {
    for (;;) {
        int best = -1;
        for (int i = 0; i < r->nfilters; ++i)
            if (r->filters[i].used && (best < 0 || r->filters[i].start < r->filters[best].start)) best = i;
        if (best < 0 || r->filters[best].start >= r->unp) return write_window(r, r->unp);
        struct filter *f = &r->filters[best];
        if (f->start < r->wr) { f->used = 0; continue; }
        if (r->wr < f->start) { if (write_window(r, f->start)) return -1; continue; }
        if (f->start + f->length > r->unp) {
            if (!final) return 0;
            f->used = 0;
            continue;
        }
        u32 len = f->length;
        u8 *mem = malloc((size_t)len * 2 + 8);
        if (!mem) return job_fail(r->job, "Out of memory.");
        for (u32 i = 0; i < len; ++i) mem[i] = r->win[(f->start + i) & r->mask];
        u32 offset = (u32)(f->start - r->file_start);
        u8 *out = run_filter(f, mem, len, offset, rar5);
        f->used = 0;
        u64 start = f->start;
        /* RAR 2.9: filters on the same block run one after another. */
        while (!rar5 && out) {
            int next = -1;
            for (int i = 0; i < r->nfilters; ++i)
                if (r->filters[i].used && r->filters[i].start == start && r->filters[i].length == len) { next = i; break; }
            if (next < 0) break;
            if (out != mem) memmove(mem, out, len);
            out = run_filter(&r->filters[next], mem, len, offset, 0);
            r->filters[next].used = 0;
        }
        int bad = write_out(r, out ? out : mem, start, len);
        free(mem);
        if (bad) return -1;
        r->wr = start + len;
        while (r->nfilters && !r->filters[r->nfilters - 1].used) --r->nfilters;
    }
}

static int maybe_flush(struct rar *r, int rar5) {
    if (r->unp - r->wr < r->size / 2) return 0;
    if (flush(r, 0, rar5)) return -1;
    if (r->unp - r->wr > r->size - 0x2000) return job_fail(r->job, "The RAR data is damaged.");
    return 0;
}

static struct filter *add_filter(struct rar *r) {
    if (r->nfilters == FILTERS) {
        if (flush(r, 0, 1)) return 0;
        if (r->nfilters == FILTERS) return 0;
    }
    struct filter *f = &r->filters[r->nfilters++];
    memset(f, 0, sizeof(*f));
    f->used = 1;
    return f;
}

static inline void copy_string(struct rar *r, u32 length, u64 distance) {
    for (u32 i = 0; i < length; ++i) {
        r->win[r->unp & r->mask] = r->win[(r->unp - distance) & r->mask];
        ++r->unp;
    }
}

static void insert_old(struct rar *r, u64 distance) {
    r->old[3] = r->old[2];
    r->old[2] = r->old[1];
    r->old[1] = r->old[0];
    r->old[0] = distance;
}

struct block { u64 end_bits; int last, tables; };

static int read_block(struct rar *r, struct block *blk) {
    struct bits *b = &r->b;
    align(b);
    u32 flags = take(b, 8), stored = take(b, 8);
    int count = (int)((flags >> 3) & 3) + 1;
    if (count == 4) return -1;
    u32 size = 0;
    for (int i = 0; i < count; ++i) size += take(b, 8) << (8 * i);
    if ((u8)(0x5a ^ flags ^ size ^ (size >> 8) ^ (size >> 16)) != stored) return -1;
    u64 start = b->used / 8;
    blk->end_bits = (start + size - 1) * 8 + (flags & 7) + 1;
    if (!size) blk->end_bits = start * 8;
    blk->last = (flags & 0x40) != 0;
    blk->tables = (flags & 0x80) != 0;
    return 0;
}

static int read_lengths(struct bits *b, const struct huff *bd, u8 *table, int n, const u8 *old) {
    for (int i = 0; i < n;) {
        if (exhausted(b) && b->used > b->total + 64) return -1;
        int number = decode(b, bd);
        if (number < 16) { table[i] = old ? (u8)((number + old[i]) & 15) : (u8)number; ++i; }
        else if (number < 18) {
            int k = number == 16 ? (int)take(b, 3) + 3 : (int)take(b, 7) + 11;
            if (!i) return -1;
            while (k-- > 0 && i < n) { table[i] = table[i - 1]; ++i; }
        } else {
            int k = number == 18 ? (int)take(b, 3) + 3 : (int)take(b, 7) + 11;
            while (k-- > 0 && i < n) table[i++] = 0;
        }
    }
    return 0;
}

static void bit_lengths(struct bits *b, struct huff *bd) {
    u8 lengths[20];
    for (int i = 0; i < 20; ++i) {
        u32 len = take(b, 4);
        if (len == 15) {
            u32 zeros = take(b, 4);
            if (!zeros) lengths[i] = 15;
            else {
                zeros += 2;
                while (zeros-- > 0 && i < 20) lengths[i++] = 0;
                --i;
            }
        } else lengths[i] = (u8)len;
    }
    make_table(bd, lengths, 20);
}

static int read_tables5(struct rar *r) {
    struct bits *b = &r->b;
    bit_lengths(b, &r->bd);
    int dc = r->extra_dist ? 80 : 64;
    u8 table[306 + 80 + 16 + 44];
    int total = 306 + dc + 16 + 44;
    if (read_lengths(b, &r->bd, table, total, 0)) return -1;
    make_table(&r->ld, table, 306);
    make_table(&r->dd, table + 306, dc);
    make_table(&r->ldd, table + 306 + dc, 16);
    make_table(&r->rd, table + 306 + dc + 16, 44);
    r->tables = 1;
    return 0;
}

static u32 slot_length(struct bits *b, u32 slot) {
    u32 bits, length = 2;
    if (slot < 8) { bits = 0; length += slot; }
    else { bits = slot / 4 - 1; length += (4 | (slot & 3)) << bits; }
    if (bits) length += take(b, (int)bits);
    return length;
}

static u32 filter_data(struct bits *b) {
    u32 count = take(b, 2) + 1, v = 0;
    for (u32 i = 0; i < count; ++i) v += take(b, 8) << (i * 8);
    return v;
}

static int unpack5(struct rar *r) {
    struct bits *b = &r->b;
    struct block blk;
    if (read_block(r, &blk)) return job_fail(r->job, "The RAR data is damaged.");
    if (blk.tables && read_tables5(r)) return job_fail(r->job, "The RAR data is damaged.");
    if (!r->tables) return job_fail(r->job, "The RAR data is damaged.");
    u64 limit = r->file_start + r->file_size;
    unsigned ticks = 0;
    for (;;) {
        if (!(++ticks & 0xfff) && job_tick(r->job)) return -1;
        int done = 0;
        while (b->used >= blk.end_bits) {
            if (blk.last) { done = 1; break; }
            if (read_block(r, &blk) || (blk.tables && read_tables5(r))) return job_fail(r->job, "The RAR data is damaged.");
        }
        if (done || exhausted(b) || r->unp > limit + 0x10000) break;
        if (maybe_flush(r, 1)) return -1;
        int slot = decode(b, &r->ld);
        if (slot < 256) { r->win[r->unp++ & r->mask] = (u8)slot; continue; }
        if (slot >= 262) {
            u32 length = slot_length(b, (u32)slot - 262);
            int dslot = decode(b, &r->dd);
            u64 distance = 1;
            int dbits;
            if (dslot < 4) { dbits = 0; distance += (u64)dslot; }
            else { dbits = dslot / 2 - 1; distance += (u64)(2 | (dslot & 1)) << dbits; }
            if (dbits > 0) {
                if (dbits >= 4) {
                    if (dbits > 4) distance += take_long(b, dbits - 4) << 4;
                    distance += (u64)decode(b, &r->ldd);
                } else distance += take(b, dbits);
            }
            if (distance > 0x100) { ++length; if (distance > 0x2000) { ++length; if (distance > 0x40000) ++length; } }
            insert_old(r, distance);
            r->last_length = length;
            copy_string(r, length, distance);
            continue;
        }
        if (slot == 256) {
            u32 offset = filter_data(b), length = filter_data(b);
            int type = (int)take(b, 3);
            u32 channels = 0;
            if (type == 0) channels = take(b, 5) + 1;
            if (type > 3 || length > 0x400000 || !length) continue;
            struct filter *f = add_filter(r);
            if (!f) return job_fail(r->job, "The RAR data is damaged.");
            f->start = r->unp + offset;
            f->length = length;
            f->type = type == 0 ? F_DELTA : type == 1 ? F_E8 : type == 2 ? F_E8E9 : F_ARM;
            f->r[0] = channels;
            continue;
        }
        if (slot == 257) {
            if (r->last_length) copy_string(r, r->last_length, r->old[0]);
            continue;
        }
        int index = slot - 258;
        u64 distance = r->old[index];
        for (int i = index; i > 0; --i) r->old[i] = r->old[i - 1];
        r->old[0] = distance;
        u32 length = slot_length(b, (u32)decode(b, &r->rd));
        r->last_length = length;
        copy_string(r, length, distance);
    }
    return flush(r, 1, 1);
}

static const u8 ldecode[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224};
static const u8 lbits[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5};
static const u8 sddecode[] = {0, 4, 8, 16, 32, 64, 128, 192};
static const u8 sdbits[] = {2, 2, 3, 4, 5, 6, 6, 6};
static u32 ddecode[60];
static u8 dbits[60];

static void distance_tables(void) {
    if (ddecode[59]) return;
    static const int counts[] = {4, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 14, 0, 12};
    u32 dist = 0;
    int slot = 0;
    for (int bits = 0; bits < (int)(sizeof(counts) / sizeof(counts[0])); ++bits)
        for (int j = 0; j < counts[bits]; ++j, ++slot, dist += 1u << bits) {
            ddecode[slot] = dist;
            dbits[slot] = (u8)bits;
        }
}

static int read_tables3(struct rar *r) {
    struct bits *b = &r->b;
    align(b);
    u32 head = peek(b, 16);
    if (head & 0x8000) { r->block_ppm = 1; return job_fail(r->job, "It uses RAR's PPMd text compression, which DOS-XC cannot unpack yet."); }
    r->block_ppm = 0;
    r->prev_low = r->low_rep = 0;
    if (!(head & 0x4000)) memset(r->old_table, 0, sizeof(r->old_table));
    skip(b, 2);
    bit_lengths(b, &r->bd);
    u8 table[404];
    if (read_lengths(b, &r->bd, table, 404, r->old_table)) return job_fail(r->job, "The RAR data is damaged.");
    make_table(&r->ld, table, 299);
    make_table(&r->dd, table + 299, 60);
    make_table(&r->ldd, table + 359, 17);
    make_table(&r->rd, table + 376, 28);
    memcpy(r->old_table, table, sizeof(table));
    r->tables = 1;
    return 0;
}

static u32 vm_number(struct bits *b) {
    u32 data = peek(b, 16);
    switch (data & 0xc000) {
    case 0: skip(b, 6); return (data >> 10) & 15;
    case 0x4000:
        if (!(data & 0x3c00)) { skip(b, 14); return 0xffffff00u | ((data >> 2) & 0xff); }
        skip(b, 10);
        return (data >> 6) & 0xff;
    case 0x8000: skip(b, 2); return take(b, 16);
    default: skip(b, 2); { u32 high = take(b, 16); return high << 16 | take(b, 16); }
    }
}

static int standard_filter(const u8 *code, u32 n) {
    u8 x = 0;
    for (u32 i = 1; i < n; ++i) x ^= code[i];
    if (!n || x != code[0]) return F_NONE;
    static const struct { u32 length, crc; int type; } known[] = {
        {53, 0xad576887u, F_E8}, {57, 0x3cd7e57eu, F_E8E9}, {120, 0x3769893fu, F_ITANIUM},
        {29, 0x0e06077du, F_DELTA}, {149, 0x1c2c5dc8u, F_RGB}, {216, 0xbc85e701u, F_AUDIO}};
    u32 crc = crc32_update(0, code, n);
    for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); ++i)
        if (known[i].length == n && known[i].crc == crc) return known[i].type;
    return F_NONE;
}

static int add_vm_code(struct rar *r, u32 first, const u8 *code, u32 n) {
    struct src mem;
    memset(&mem, 0, sizeof(mem));
    if (n > sizeof(mem.buf)) return -1;
    memcpy(mem.buf, code, n);
    mem.len = n;
    struct bits v;
    bits_open(&v, &mem, n);
    u32 slot;
    if (first & 0x80) {
        slot = vm_number(&v);
        if (!slot) { r->nslots = 0; r->nfilters = 0; }
        else --slot;
    } else slot = (u32)r->last_slot;
    if (slot > (u32)r->nslots || slot >= 1024) return -1;
    r->last_slot = (int)slot;
    int fresh = slot == (u32)r->nslots;
    if (fresh) { r->slots[slot].type = F_NONE; r->slots[slot].length = 0; ++r->nslots; }
    u32 start = vm_number(&v);
    if (first & 0x40) start += 258;
    u32 length;
    if (first & 0x20) { length = vm_number(&v); r->slots[slot].length = length; }
    else length = r->slots[slot].length;
    u32 init[7] = {0};
    init[4] = length;
    if (first & 0x10) {
        u32 mask = take(&v, 7);
        for (int i = 0; i < 7; ++i)
            if (mask & (1u << i)) init[i] = vm_number(&v);
    }
    if (fresh) {
        u32 size = vm_number(&v);
        if (size >= 0x10000 || !size || v.used / 8 + size > n) return -1;
        u8 *prog = malloc(size);
        if (!prog) return -1;
        for (u32 i = 0; i < size; ++i) prog[i] = (u8)take(&v, 8);
        r->slots[slot].type = standard_filter(prog, size);
        free(prog);
    }
    if (r->slots[slot].type == F_NONE) return job_fail(r->job, "It uses a RAR filter DOS-XC does not know.");
    struct filter *f = add_filter(r);
    if (!f) return -1;
    f->start = r->unp + start;
    f->length = length;
    f->type = r->slots[slot].type;
    memcpy(f->r, init, sizeof(init));
    if (f->type == F_E8 || f->type == F_E8E9 || f->type == F_ITANIUM) { if (length > 0x40000) f->used = 0; }
    else if (length > 0x20000) f->used = 0;
    return 0;
}

static int read_vm_code(struct rar *r) {
    struct bits *b = &r->b;
    u32 first = take(b, 8), length = (first & 7) + 1;
    if (length == 7) length = take(b, 8) + 7;
    else if (length == 8) length = take(b, 16);
    if (!length) return -1;
    u8 *code = malloc(length);
    if (!code) return -1;
    for (u32 i = 0; i < length; ++i) code[i] = (u8)take(b, 8);
    int bad = add_vm_code(r, first, code, length);
    free(code);
    return bad;
}

/* 1 at the end of the file. */
static int end_of_block(struct rar *r) {
    struct bits *b = &r->b;
    u32 v = peek(b, 2);
    int new_table, new_file = 0;
    if (v & 2) { new_table = 1; skip(b, 1); }
    else { new_file = 1; new_table = (v & 1) != 0; skip(b, 2); }
    r->tables = !new_table;
    if (new_file) return 1;
    if (new_table && read_tables3(r)) return -1;
    return 0;
}

static int unpack29(struct rar *r, int solid) {
    struct bits *b = &r->b;
    distance_tables();
    if ((!solid || !r->tables) && read_tables3(r)) return -1;
    u64 limit = r->file_start + r->file_size;
    unsigned ticks = 0;
    while (!exhausted(b) && r->unp <= limit + 0x10000) {
        if (!(++ticks & 0xfff) && job_tick(r->job)) return -1;
        if (maybe_flush(r, 0)) return -1;
        int number = decode(b, &r->ld);
        if (number < 256) { r->win[r->unp++ & r->mask] = (u8)number; continue; }
        if (number >= 271) {
            number -= 271;
            if (number >= 28) return job_fail(r->job, "The RAR data is damaged.");
            u32 length = ldecode[number] + 3u + take(b, lbits[number]);
            int dn = decode(b, &r->dd);
            if (dn >= 60) return job_fail(r->job, "The RAR data is damaged.");
            u32 distance = ddecode[dn] + 1, bits = dbits[dn];
            if (bits) {
                if (dn > 9) {
                    if (bits > 4) distance += take(b, (int)bits - 4) << 4;
                    if (r->low_rep) { --r->low_rep; distance += r->prev_low; }
                    else {
                        int low = decode(b, &r->ldd);
                        if (low == 16) { r->low_rep = 15; distance += r->prev_low; }
                        else { distance += (u32)low; r->prev_low = (u32)low; }
                    }
                } else distance += take(b, (int)bits);
            }
            if (distance >= 0x2000) { ++length; if (distance >= 0x40000) ++length; }
            insert_old(r, distance);
            r->last_length = length;
            copy_string(r, length, distance);
            continue;
        }
        if (number == 256) {
            int e = end_of_block(r);
            if (e < 0) return -1;
            if (e) break;
            continue;
        }
        if (number == 257) {
            if (read_vm_code(r)) return job_fail(r->job, "The RAR data is damaged.");
            continue;
        }
        if (number == 258) {
            if (r->last_length) copy_string(r, r->last_length, r->old[0]);
            continue;
        }
        if (number < 263) {
            int index = number - 259;
            u64 distance = r->old[index];
            for (int i = index; i > 0; --i) r->old[i] = r->old[i - 1];
            r->old[0] = distance;
            int ln = decode(b, &r->rd);
            if (ln >= 28) return job_fail(r->job, "The RAR data is damaged.");
            u32 length = ldecode[ln] + 2u + take(b, lbits[ln]);
            r->last_length = length;
            copy_string(r, length, distance);
            continue;
        }
        number -= 263;
        u32 distance = sddecode[number] + 1u + take(b, sdbits[number]);
        insert_old(r, distance);
        r->last_length = 2;
        copy_string(r, 2, distance);
    }
    return flush(r, 1, 0);
}

static int window(struct rar *r, u64 want, int solid) {
    u64 size = 1u << 17;
    while (size < want) size <<= 1;
    if (r->win && (solid || r->size >= size)) {
        if (size > r->size) return job_fail(r->job, "The RAR archive is damaged.");
        return 0;
    }
    if (size > ((u64)1 << 30) || size > (size_t)-1) return job_fail(r->job, "Unpacking it needs more memory than there is.");
    free(r->win);
    r->win = calloc((size_t)size, 1);
    if (!r->win) return job_fail(r->job, "Unpacking it needs more memory than there is.");
    r->size = size;
    r->mask = size - 1;
    return 0;
}

static void reset_stream(struct rar *r) {
    r->unp = r->wr = 0;
    r->tables = 0;
    memset(r->old, 0, sizeof(r->old));
    r->last_length = 0;
    memset(r->old_table, 0, sizeof(r->old_table));
    r->nslots = r->last_slot = 0;
    r->prev_low = r->low_rep = 0;
}

static int extract_file(struct rar *r, FILE *f, const char *name, u64 data_at, u64 packed, u64 size, int method,
                        int version, u64 dict, int solid, long long mtime, u32 crc, int check) {
    int k = out_begin(r->job, name, 0, mtime);
    if (k < 0) return -1;
    r->writing = !k;
    struct src *in = malloc(sizeof(*in));
    if (!in) return job_fail(r->job, "Out of memory.");
    src_open(in, f, data_at, packed, r->job);
    int bad = 0;
    if (method == 0) {
        u64 left = size;
        while (!bad && left && src_fill(in)) {
            size_t n = in->len < left ? in->len : (size_t)left;
            if (r->writing && out_write(r->job, in->buf, n)) bad = -1;
            left -= n;
        }
        if (left && !bad) bad = job_fail(r->job, "The RAR archive ends too soon.");
    } else {
        u64 want = version == 29 ? (u64)4 << 20 : dict;
        if (!solid && !r->solid_archive && size && want > size) want = size;
        bad = window(r, want, solid && r->win);
        if (!bad) {
            if (!solid) reset_stream(r);
            r->nfilters = 0;
            r->file_start = r->unp;
            r->wr = r->unp;
            r->file_size = size;
            bits_open(&r->b, in, packed);
            bad = version == 29 ? unpack29(r, solid) : unpack5(r);
            if (!bad && r->writing && size != ~0ull >> 1 && r->job->written < size) bad = job_fail(r->job, "\"%s\" is damaged (it ends too soon).", name);
            r->file_start = r->unp;
        }
    }
    free(in);
    if (bad) return -1;
    if (!r->writing) return 0;
    if (check && r->job->crc != crc) return job_fail(r->job, "\"%s\" is damaged (its check does not match).", name);
    return out_end(r->job);
}

static u64 vint(const u8 **p, const u8 *end, int *bad) {
    u64 v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (*p >= end) { *bad = 1; return 0; }
        u8 c = *(*p)++;
        v |= (u64)(c & 0x7f) << shift;
        if (!(c & 0x80)) return v;
    }
    *bad = 1;
    return 0;
}

static int rar5(struct rar *r, FILE *f, u64 file_size) {
    u64 at = 8;
    u8 *h = 0;
    int result = 0;
    while (at < file_size && !result) {
        u8 head[16];
        size_t got;
        if (arc_seek(f, at) || (got = fread(head, 1, sizeof(head), f)) < 6) break;
        const u8 *p = head + 4;
        int bad = 0;
        u64 size = vint(&p, head + got, &bad);
        size_t size_len = (size_t)(p - (head + 4));
        if (bad || size > (2u << 20) || !size) { result = job_fail(r->job, "The RAR archive is damaged."); break; }
        free(h);
        h = malloc((size_t)size + size_len);
        if (!h || arc_seek(f, at + 4) || fread(h, 1, (size_t)size + size_len, f) != size + size_len) { result = job_fail(r->job, "The RAR archive ends too soon."); break; }
        if (crc32_update(0, h, (size_t)size + size_len) != get32(head)) { result = job_fail(r->job, "The RAR archive is damaged."); break; }
        const u8 *q = h + size_len, *end = h + size_len + size;
        u64 type = vint(&q, end, &bad), flags = vint(&q, end, &bad), extra = 0, data = 0;
        if (flags & 1) extra = vint(&q, end, &bad);
        if (flags & 2) data = vint(&q, end, &bad);
        u64 next = at + 4 + size_len + size + data;
        if (bad || extra > (u64)(end - q)) { result = job_fail(r->job, "The RAR archive is damaged."); break; }
        const u8 *extra_at = end - extra;
        if (type == 4) { result = job_fail(r->job, "The archive is protected with a password, which DOS-XC cannot open yet."); break; }
        if (type == 5) break;
        if (type == 1) {
            u64 archive_flags = vint(&q, end, &bad);
            r->solid_archive = (archive_flags & 4) != 0;
            if ((archive_flags & 1) && (archive_flags & 2) && vint(&q, end, &bad)) { result = job_fail(r->job, "This is not the first part of a split archive."); break; }
        } else if (type == 2) {
            u64 file_flags = vint(&q, end, &bad), unpacked = vint(&q, end, &bad);
            u64 attributes = vint(&q, end, &bad);
            long long mtime = 0;
            u32 crc = 0;
            if (file_flags & 2) { if (q + 4 > end) bad = 1; else { mtime = get32(q); q += 4; } }
            if (file_flags & 4) { if (q + 4 > end) bad = 1; else { crc = get32(q); q += 4; } }
            u64 info = vint(&q, end, &bad);
            vint(&q, end, &bad);
            u64 name_len = vint(&q, end, &bad);
            if (bad || name_len > (u64)(end - q) || name_len > 4000) { result = job_fail(r->job, "The RAR archive is damaged."); break; }
            char name[4096];
            memcpy(name, q, (size_t)name_len);
            name[name_len] = 0;
            int encrypted = 0, link = 0;
            char target[1024] = "";
            for (const u8 *x = extra_at; x < end && !bad;) {
                u64 rsize = vint(&x, end, &bad);
                if (bad || rsize > (u64)(end - x)) break;
                const u8 *record = x, *record_end = x + rsize;
                u64 rtype = vint(&record, record_end, &bad);
                if (rtype == 1) encrypted = 1;
                if (rtype == 4) {
                    vint(&record, record_end, &bad);
                    u64 number = vint(&record, record_end, &bad);
                    size_t n = strlen(name);
                    if (!bad && number && n + 24 < sizeof(name)) snprintf(name + n, sizeof(name) - n, ";%llu", (unsigned long long)number);
                }
                if (rtype == 5) {
                    u64 kind = vint(&record, record_end, &bad);
                    vint(&record, record_end, &bad);
                    u64 len = vint(&record, record_end, &bad);
                    link = 1;
                    if (!bad && (kind == 4 || kind == 5) && len < sizeof(target) && len <= (u64)(record_end - record)) {
                        memcpy(target, record, (size_t)len);
                        target[len] = 0;
                    }
                }
                x = record_end;
            }
            if (encrypted) { result = job_fail(r->job, "\"%s\" is protected with a password, which DOS-XC cannot open yet.", name); break; }
            if (flags & 0x18) { result = job_fail(r->job, "The archive is split into parts, which DOS-XC cannot join yet."); break; }
            int version = (int)(info & 0x3f), solid = (int)((info >> 6) & 1), method = (int)((info >> 7) & 7);
            u64 dict_bits = (info >> 10) & (version ? 0x1f : 0x0f);
            u64 dict = ((u64)0x20000 << dict_bits);
            if (version == 1) dict += dict / 32 * ((info >> 15) & 0x1f);
            if (file_flags & 1) { if (out_begin(r->job, name, 1, mtime) < 0) result = -1; }
            else if (link) { if (target[0] && out_copy(r->job, name, target, mtime) < 0) result = -1; }
            else if (version > 1) result = job_fail(r->job, "\"%s\" was packed by a newer RAR than DOS-XC understands.", name);
            else {
                r->extra_dist = version == 1;
                if (file_flags & 8) unpacked = ~0ull >> 1;
                result = extract_file(r, f, name, at + 4 + size_len + size, data, unpacked, method, 50, dict, solid, mtime, crc, (file_flags & 4) != 0);
            }
            (void)attributes;
        }
        at = next;
    }
    free(h);
    return result;
}

static void unicode_name(const u8 *name, size_t ascii_len, const u8 *enc, size_t enc_size, char *out, size_t out_size) {
    u16 wide[1024];
    size_t ep = 0, dp = 0, max = sizeof(wide) / sizeof(wide[0]) - 1;
    u8 high = ep < enc_size ? enc[ep++] : 0, flags = 0;
    int flag_bits = 0;
    while (ep < enc_size && dp < max) {
        if (!flag_bits) { flags = enc[ep++]; flag_bits = 8; }
        switch (flags >> 6) {
        case 0: if (ep < enc_size) wide[dp++] = enc[ep++]; break;
        case 1: if (ep < enc_size) wide[dp++] = (u16)(enc[ep++] + (high << 8)); break;
        case 2: if (ep + 1 < enc_size) { wide[dp++] = (u16)(enc[ep] | enc[ep + 1] << 8); ep += 2; } break;
        case 3: {
            if (ep >= enc_size) break;
            int length = enc[ep++];
            if (length & 0x80) {
                if (ep >= enc_size) break;
                u8 correction = enc[ep++];
                for (length = (length & 0x7f) + 2; length > 0 && dp < max; --length, ++dp)
                    wide[dp] = (u16)(((dp < ascii_len ? name[dp] : 0) + correction) & 0xff) + (u16)(high << 8);
            } else
                for (length += 2; length > 0 && dp < max; --length, ++dp) wide[dp] = dp < ascii_len ? name[dp] : 0;
            break;
        }
        }
        flags <<= 2;
        flag_bits -= 2;
    }
    size_t n = 0;
    for (size_t i = 0; i < dp && n + 4 < out_size; ++i) {
        u32 c = wide[i];
        if (!c) break;
        if (c < 0x80) out[n++] = (char)c;
        else if (c < 0x800) { out[n++] = (char)(0xc0 | c >> 6); out[n++] = (char)(0x80 | (c & 63)); }
        else { out[n++] = (char)(0xe0 | c >> 12); out[n++] = (char)(0x80 | (c >> 6 & 63)); out[n++] = (char)(0x80 | (c & 63)); }
    }
    out[n] = 0;
}

static int rar4(struct rar *r, FILE *f, u64 file_size) {
    u64 at = 7;
    int result = 0;
    u8 *h = 0;
    while (at + 7 <= file_size && !result) {
        u8 head[7];
        if (arc_seek(f, at) || fread(head, 1, 7, f) != 7) break;
        u32 type = head[2], flags = get16(head + 3), size = get16(head + 5);
        if (size < 7) { result = job_fail(r->job, "The RAR archive is damaged."); break; }
        free(h);
        h = malloc(size);
        if (!h || arc_seek(f, at) || fread(h, 1, size, f) != size) { result = job_fail(r->job, "The RAR archive ends too soon."); break; }
        if (type != 0x7b && (crc32_update(0, h + 2, size - 2) & 0xffff) != get16(h)) { result = job_fail(r->job, "The RAR archive is damaged."); break; }
        u64 add = (flags & 0x8000) && size >= 11 ? get32(h + 7) : 0;
        if (type == 0x7b) break;
        if (type == 0x73 && (flags & 0x80)) { result = job_fail(r->job, "The archive is protected with a password, which DOS-XC cannot open yet."); break; }
        if (type == 0x73) r->solid_archive = (flags & 8) != 0;
        if (type == 0x74 && size >= 32) {
            u64 packed = get32(h + 7), unpacked = get32(h + 11);
            u32 crc = get32(h + 16), ftime = get32(h + 20), name_size = get16(h + 26), attributes = get32(h + 28);
            int version = h[24], method = h[25];
            u32 pos = 32;
            if (flags & 0x100) {
                if (size < 40) { result = job_fail(r->job, "The RAR archive is damaged."); break; }
                packed |= (u64)get32(h + 32) << 32;
                unpacked |= (u64)get32(h + 36) << 32;
                pos = 40;
            }
            add = packed;
            if (pos + name_size > size) { result = job_fail(r->job, "The RAR archive is damaged."); break; }
            char name[4096];
            const u8 *raw = h + pos;
            size_t ascii = 0;
            while (ascii < name_size && raw[ascii]) ++ascii;
            if ((flags & 0x200) && ascii < name_size) unicode_name(raw, ascii, raw + ascii + 1, name_size - ascii - 1, name, sizeof(name));
            else {
                size_t n = ascii < sizeof(name) - 1 ? ascii : sizeof(name) - 1;
                memcpy(name, raw, n);
                name[n] = 0;
            }
            long long mtime = from_dos_time((u16)(ftime >> 16), (u16)ftime);
            int directory = (flags & 0xe0) == 0xe0 || (attributes & 0x10 && h[15] < 3);
            if (flags & 4) { result = job_fail(r->job, "\"%s\" is protected with a password, which DOS-XC cannot open yet.", name); break; }
            if (flags & 3) { result = job_fail(r->job, "The archive is split into parts, which DOS-XC cannot join yet."); break; }
            int symlink = h[15] == 3 && (attributes & 0xf000) == 0xa000;
            if (directory) { if (out_begin(r->job, name, 1, mtime) < 0) result = -1; }
            else if (symlink) { /* links cannot be made on FAT */ }
            else if (method != 0x30 && version != 29 && version != 36)
                result = job_fail(r->job, "\"%s\" was packed by an old version of RAR that DOS-XC cannot unpack.", name);
            else if (method < 0x30 || method > 0x35) result = job_fail(r->job, "The RAR archive is damaged.");
            else result = extract_file(r, f, name, at + size, packed, unpacked, method == 0x30 ? 0 : 1, 29, (u64)4 << 20,
                                       (flags & 0x10) != 0, mtime, crc, 1);
        }
        at += size + add;
    }
    free(h);
    return result;
}

int rar_extract(struct job *job, FILE *f) {
    u8 sig[8];
    if (arc_seek(f, 0) || fread(sig, 1, 8, f) < 7) return job_fail(job, "The archive could not be read.");
    struct rar *r = calloc(1, sizeof(*r));
    if (!r) return job_fail(job, "Out of memory.");
    r->job = job;
    u64 size = arc_file_size(f);
    int result;
    if (!memcmp(sig, "Rar!\x1a\x07\x01\x00", 8)) result = rar5(r, f, size);
    else if (!memcmp(sig, "Rar!\x1a\x07\x00", 7)) result = rar4(r, f, size);
    else result = job_fail(job, "This RAR version is not supported.");
    free(r->win);
    free(r);
    return result;
}
