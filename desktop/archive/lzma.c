/*
 * DOS-XC /desktop/archive/lzma.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "arc.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define PROB_INIT 1024
#define MATCH_MAX 273

struct lzma_len { u16 choice, choice2, low[16][8], mid[16][8], high[256]; };
struct model {
    u16 is_match[12 << 4], is_rep[12], g0[12], g1[12], g2[12], rep0_long[12 << 4];
    u16 slot[4][64], pos[115], align[16];
    struct lzma_len len, rep_len;
    u16 *lit;
    unsigned lc, lp, pb;
    u32 state, rep[4];
};

static void fill_probs(u16 *p, size_t n) { while (n--) *p++ = PROB_INIT; }

static void model_reset(struct model *m) {
    u16 *lit = m->lit;
    fill_probs((u16 *)m, offsetof(struct model, lit) / sizeof(u16));
    fill_probs(lit, (size_t)0x300 << (m->lc + m->lp));
    m->state = 0;
    m->rep[0] = m->rep[1] = m->rep[2] = m->rep[3] = 0;
}

static int model_props(struct model *m, unsigned lc, unsigned lp, unsigned pb) {
    if (lc > 8 || lp > 4 || pb > 4) return -1;
    if (m->lit && m->lc + m->lp != lc + lp) { free(m->lit); m->lit = 0; }
    m->lc = lc;
    m->lp = lp;
    m->pb = pb;
    if (!m->lit && !(m->lit = malloc(sizeof(u16) * ((size_t)0x300 << (lc + lp))))) return -1;
    return 0;
}

static u32 literal_state(const struct model *m, u64 position, u8 previous) {
    return (u32)(((position & ((1u << m->lp) - 1)) << m->lc) + (previous >> (8 - m->lc)));
}

static u32 update_literal(u32 s) { return s < 4 ? 0 : s < 10 ? s - 3 : s - 6; }

struct dec {
    struct src *in;
    u32 range, code;
    int bad;
    u8 *dict;
    u32 size, pos, flushed;
    u64 total;
    struct sink *out;
    struct model m;
};

static int rc_init(struct dec *d) {
    d->range = 0xffffffffu;
    d->code = 0;
    if (src_byte(d->in) != 0) return -1;
    for (int i = 0; i < 4; ++i) {
        int b = src_byte(d->in);
        if (b < 0) return -1;
        d->code = d->code << 8 | (u32)b;
    }
    return d->code == d->range ? -1 : 0;
}

static inline void rc_norm(struct dec *d) {
    if (d->range < (1u << 24)) {
        int b = src_byte(d->in);
        if (b < 0) { d->bad = 1; b = 0; }
        d->range <<= 8;
        d->code = d->code << 8 | (u32)b;
    }
}

static inline int bit(struct dec *d, u16 *p) {
    u32 bound = (d->range >> 11) * *p;
    int r;
    if (d->code < bound) { d->range = bound; *p += (2048 - *p) >> 5; r = 0; }
    else { d->range -= bound; d->code -= bound; *p -= *p >> 5; r = 1; }
    rc_norm(d);
    return r;
}

static u32 direct(struct dec *d, int n) {
    u32 r = 0;
    while (n--) {
        d->range >>= 1;
        d->code -= d->range;
        u32 t = 0 - (d->code >> 31);
        d->code += d->range & t;
        r = (r << 1) + (t + 1);
        rc_norm(d);
    }
    return r;
}

static u32 tree(struct dec *d, u16 *p, int n) {
    u32 m = 1;
    for (int i = 0; i < n; ++i) m = (m << 1) + (u32)bit(d, p + m);
    return m - (1u << n);
}

static u32 tree_reverse(struct dec *d, u16 *p, int n) {
    u32 m = 1, s = 0;
    for (int i = 0; i < n; ++i) {
        u32 b = (u32)bit(d, p + m);
        m = (m << 1) + b;
        s |= b << i;
    }
    return s;
}

static u32 length(struct dec *d, struct lzma_len *l, u32 pos_state) {
    if (!bit(d, &l->choice)) return tree(d, l->low[pos_state], 3);
    if (!bit(d, &l->choice2)) return 8 + tree(d, l->mid[pos_state], 3);
    return 16 + tree(d, l->high, 8);
}

static int dict_flush(struct dec *d) {
    if (d->pos > d->flushed && d->out->write(d->out, d->dict + d->flushed, d->pos - d->flushed)) return -1;
    d->flushed = d->pos;
    if (d->pos == d->size) d->pos = d->flushed = 0;
    return 0;
}

static inline int put(struct dec *d, u8 b) {
    d->dict[d->pos++] = b;
    ++d->total;
    return d->pos == d->size ? dict_flush(d) : 0;
}

static inline u8 back(const struct dec *d, u32 distance) {
    u32 at = d->pos >= distance ? d->pos - distance : d->size - distance + d->pos;
    return d->dict[at];
}

static int dec_alloc(struct dec *d, u32 dict_size, u64 size) {
    if (size && size < dict_size) dict_size = (u32)size;
    if (dict_size < 4096) dict_size = 4096;
    d->size = dict_size;
    d->dict = malloc(dict_size);
    d->pos = d->flushed = 0;
    d->total = 0;
    d->m.lit = 0;
    return d->dict ? 0 : -1;
}

/* Decode until LEFT bytes are out (or the end marker when ALLOW_END). 1 at the end marker. */
static int run(struct dec *d, u64 left, int allow_end, struct job *job) {
    struct model *m = &d->m;
    u32 pb_mask = (1u << m->pb) - 1;
    unsigned ticks = 0;
    while (left || allow_end) {
        if (!(++ticks & 0xffff) && job_tick(job)) return -1;
        if (d->bad) return -1;
        u32 pos_state = (u32)d->total & pb_mask, s = m->state;
        if (!bit(d, &m->is_match[(s << 4) + pos_state])) {
            if (!left) return -1;
            u8 previous = d->total ? back(d, 1) : 0;
            u16 *p = m->lit + 0x300 * literal_state(m, d->total, previous);
            u32 sym = 1;
            if (s >= 7) {
                u32 match_byte = back(d, m->rep[0] + 1);
                do {
                    u32 match_bit = (match_byte >> 7) & 1;
                    match_byte <<= 1;
                    u32 b = (u32)bit(d, p + ((1 + match_bit) << 8) + sym);
                    sym = sym << 1 | b;
                    if (match_bit != b) break;
                } while (sym < 0x100);
            }
            while (sym < 0x100) sym = sym << 1 | (u32)bit(d, p + sym);
            if (put(d, (u8)sym)) return -1;
            m->state = update_literal(s);
            --left;
            continue;
        }
        u32 len;
        if (bit(d, &m->is_rep[s])) {
            if (!left || !d->total) return -1;
            if (!bit(d, &m->g0[s])) {
                if (!bit(d, &m->rep0_long[(s << 4) + pos_state])) {
                    m->state = s < 7 ? 9 : 11;
                    if (put(d, back(d, m->rep[0] + 1))) return -1;
                    --left;
                    continue;
                }
            } else {
                u32 dist;
                if (!bit(d, &m->g1[s])) dist = m->rep[1];
                else {
                    if (!bit(d, &m->g2[s])) dist = m->rep[2];
                    else { dist = m->rep[3]; m->rep[3] = m->rep[2]; }
                    m->rep[2] = m->rep[1];
                }
                m->rep[1] = m->rep[0];
                m->rep[0] = dist;
            }
            len = length(d, &m->rep_len, pos_state);
            m->state = s < 7 ? 8 : 11;
        } else {
            m->rep[3] = m->rep[2];
            m->rep[2] = m->rep[1];
            m->rep[1] = m->rep[0];
            len = length(d, &m->len, pos_state);
            m->state = s < 7 ? 7 : 10;
            u32 slot = tree(d, m->slot[len < 4 ? len : 3], 6), dist;
            if (slot < 4) dist = slot;
            else {
                int n = (int)(slot >> 1) - 1;
                dist = (2 | (slot & 1)) << n;
                if (slot < 14) dist += tree_reverse(d, m->pos + dist - slot, n);
                else dist += (direct(d, n - 4) << 4) + tree_reverse(d, m->align, 4);
            }
            if (dist == 0xffffffffu) return allow_end && !d->bad ? 1 : -1;
            if (!left) return -1;
            m->rep[0] = dist;
        }
        len += 2;
        u64 distance = (u64)m->rep[0] + 1;
        if (distance > d->total || distance > d->size) return -1;
        if (len > left) return -1;
        for (u32 i = 0; i < len; ++i)
            if (put(d, back(d, (u32)distance))) return -1;
        left -= len;
    }
    return 0;
}

static void dec_free(struct dec *d) {
    free(d->dict);
    free(d->m.lit);
}

int lzma_decode(struct src *in, const u8 props[5], u64 size, struct sink *out, struct job *job) {
    struct dec *d = calloc(1, sizeof(*d));
    if (!d) return job_fail(job, "Out of memory.");
    unsigned p = props[0];
    u32 dict_size = get32(props + 1);
    d->in = in;
    d->out = out;
    int r = -1;
    if (p < 225 && !dec_alloc(d, dict_size, size) && !model_props(&d->m, p % 9, p / 9 % 5, p / 45) && !rc_init(d)) {
        model_reset(&d->m);
        r = run(d, size, size == ~0ull, job);
        if (r >= 0) r = dict_flush(d);
    }
    dec_free(d);
    free(d);
    if (r < 0 && !job->cancelled) return job_fail(job, "The LZMA data is damaged.");
    return r < 0 ? -1 : 0;
}

static u32 lzma2_dict(u8 prop) {
    if (prop > 40) return 0;
    if (prop == 40) return 0xffffffffu;
    return (2u | (prop & 1)) << (prop / 2 + 11);
}

int lzma2_decode(struct src *in, u8 prop, u64 size, struct sink *out, struct job *job) {
    struct dec *d = calloc(1, sizeof(*d));
    if (!d) return job_fail(job, "Out of memory.");
    d->in = in;
    d->out = out;
    u32 dict_size = lzma2_dict(prop);
    int r = dict_size && !dec_alloc(d, dict_size, size == ~0ull ? 0 : size) ? 0 : -1, have_props = 0;
    while (!r) {
        int control = src_byte(in);
        if (control <= 0) { r = control < 0 ? -1 : 0; break; }
        int b1 = src_byte(in), b2 = src_byte(in);
        if (b2 < 0) { r = -1; break; }
        if (control < 0x80) {
            if (control > 2) { r = -1; break; }
            u32 n = ((u32)b1 << 8 | (u32)b2) + 1;
            while (n-- && !r) {
                int b = src_byte(in);
                r = b < 0 ? -1 : put(d, (u8)b);
            }
            continue;
        }
        u64 unpacked = ((u64)(control & 0x1f) << 16 | (u64)b1 << 8 | (u64)b2) + 1;
        int p1 = src_byte(in), p2 = src_byte(in);
        if (p2 < 0) { r = -1; break; }
        int reset = (control >> 5) & 3;
        if (reset >= 2) {
            int p = src_byte(in);
            if (p < 0 || p >= 225 || (p % 9) + (p / 9 % 5) > 4 || model_props(&d->m, (unsigned)p % 9, (unsigned)p / 9 % 5, (unsigned)p / 45)) { r = -1; break; }
            have_props = 1;
        }
        if (!have_props) { r = -1; break; }
        if (reset >= 1) model_reset(&d->m);
        (void)p1;
        if (rc_init(d) || run(d, unpacked, 0, job) || d->bad) r = -1;
    }
    if (!r) r = dict_flush(d);
    dec_free(d);
    free(d);
    if (r && !job->cancelled) return job_fail(job, "The LZMA2 data is damaged.");
    return r ? -1 : 0;
}

#define DICT (1u << 22)
#define STEP (1u << 20)
#define WINDOW (DICT + STEP + MATCH_MAX * 2)
#define HASH4 20
#define HASH3 16
#define DEPTH 48
#define NICE 64
#define CHUNK_UNPACKED (1u << 21)
#define CHUNK_PACKED 65536

struct lzma2_enc {
    struct sink *out;
    struct model m;
    u64 low;
    u32 range, cache_size;
    u8 cache;
    u8 chunk[CHUNK_PACKED + 64];
    u32 chunk_n, chunk_unpacked;
    int need_props, need_state, first, error, overflow;
    u8 *win;                          /* WINDOW bytes; win[0] is stream offset base */
    u64 base;
    u32 start, end, chunk_start;
    u32 *head4, *head3, *chain;
    u32 inserted;                     /* win offset up to which positions are hashed */
};

static void shift_low(struct lzma2_enc *e) {
    if ((u32)e->low < 0xff000000u || (e->low >> 32)) {
        u8 carry = (u8)(e->low >> 32), temp = e->cache;
        do {
            if (e->chunk_n < sizeof(e->chunk)) e->chunk[e->chunk_n++] = (u8)(temp + carry);
            else e->overflow = 1;
            temp = 0xff;
        } while (--e->cache_size);
        e->cache = (u8)(e->low >> 24);
    }
    ++e->cache_size;
    e->low = (e->low & 0x00ffffffu) << 8;
}

static inline void ebit(struct lzma2_enc *e, u16 *p, u32 b) {
    u32 bound = (e->range >> 11) * *p;
    if (!b) { e->range = bound; *p += (2048 - *p) >> 5; }
    else { e->low += bound; e->range -= bound; *p -= *p >> 5; }
    while (e->range < (1u << 24)) { e->range <<= 8; shift_low(e); }
}

static void edirect(struct lzma2_enc *e, u32 v, int n) {
    while (n--) {
        e->range >>= 1;
        e->low += e->range & (0u - ((v >> n) & 1));
        while (e->range < (1u << 24)) { e->range <<= 8; shift_low(e); }
    }
}

static void etree(struct lzma2_enc *e, u16 *p, int n, u32 sym) {
    u32 m = 1;
    while (n--) { u32 b = (sym >> n) & 1; ebit(e, p + m, b); m = m << 1 | b; }
}

static void etree_reverse(struct lzma2_enc *e, u16 *p, int n, u32 sym) {
    u32 m = 1;
    while (n--) { u32 b = sym & 1; ebit(e, p + m, b); m = m << 1 | b; sym >>= 1; }
}

static void elength(struct lzma2_enc *e, struct lzma_len *l, u32 len, u32 pos_state) {
    if (len < 8) { ebit(e, &l->choice, 0); etree(e, l->low[pos_state], 3, len); }
    else if (len < 16) { ebit(e, &l->choice, 1); ebit(e, &l->choice2, 0); etree(e, l->mid[pos_state], 3, len - 8); }
    else { ebit(e, &l->choice, 1); ebit(e, &l->choice2, 1); etree(e, l->high, 8, len - 16); }
}

static void rc_start(struct lzma2_enc *e) {
    e->low = 0;
    e->range = 0xffffffffu;
    e->cache = 0;
    e->cache_size = 1;
    e->chunk_n = 0;
    e->overflow = 0;
}

static u64 epos(const struct lzma2_enc *e, u32 at) { return e->base + at; }

static void encode_literal(struct lzma2_enc *e, u32 at) {
    struct model *m = &e->m;
    u64 position = epos(e, at);
    u32 pos_state = (u32)position & ((1u << m->pb) - 1);
    ebit(e, &m->is_match[(m->state << 4) + pos_state], 0);
    u8 previous = position ? e->win[at - 1] : 0;
    u16 *p = m->lit + 0x300 * literal_state(m, position, previous);
    u32 sym = e->win[at] | 0x100;
    if (m->state >= 7) {
        u32 match_byte = e->win[at - m->rep[0] - 1], offs = 0x100;
        do {
            match_byte <<= 1;
            ebit(e, p + (offs + (match_byte & offs) + (sym >> 8)), (sym >> 7) & 1);
            sym <<= 1;
            offs &= ~(match_byte ^ sym);
        } while (sym < 0x10000);
    } else {
        do { ebit(e, p + (sym >> 8), (sym >> 7) & 1); sym <<= 1; } while (sym < 0x10000);
    }
    m->state = update_literal(m->state);
}

static u32 slot_of(u32 dist) {
    if (dist < 4) return dist;
    int n = 31 - __builtin_clz(dist);
    return (u32)(n << 1) | ((dist >> (n - 1)) & 1);
}

static void encode_match(struct lzma2_enc *e, u32 at, u32 len, u32 dist) {
    struct model *m = &e->m;
    u32 pos_state = (u32)epos(e, at) & ((1u << m->pb) - 1);
    ebit(e, &m->is_match[(m->state << 4) + pos_state], 1);
    ebit(e, &m->is_rep[m->state], 0);
    elength(e, &m->len, len - 2, pos_state);
    u32 slot = slot_of(dist);
    etree(e, m->slot[len - 2 < 4 ? len - 2 : 3], 6, slot);
    if (slot >= 4) {
        int n = (int)(slot >> 1) - 1;
        u32 base = (2 | (slot & 1)) << n, reduced = dist - base;
        if (slot < 14) etree_reverse(e, m->pos + base - slot, n, reduced);
        else { edirect(e, reduced >> 4, n - 4); etree_reverse(e, m->align, 4, reduced & 15); }
    }
    m->rep[3] = m->rep[2];
    m->rep[2] = m->rep[1];
    m->rep[1] = m->rep[0];
    m->rep[0] = dist;
    m->state = m->state < 7 ? 7 : 10;
}

static void encode_rep(struct lzma2_enc *e, u32 at, u32 len, int index) {
    struct model *m = &e->m;
    u32 pos_state = (u32)epos(e, at) & ((1u << m->pb) - 1), s = m->state;
    ebit(e, &m->is_match[(s << 4) + pos_state], 1);
    ebit(e, &m->is_rep[s], 1);
    if (!index) {
        ebit(e, &m->g0[s], 0);
        ebit(e, &m->rep0_long[(s << 4) + pos_state], len != 1);
        if (len == 1) { m->state = s < 7 ? 9 : 11; return; }
    } else {
        ebit(e, &m->g0[s], 1);
        u32 dist = m->rep[index];
        if (index == 1) ebit(e, &m->g1[s], 0);
        else {
            ebit(e, &m->g1[s], 1);
            ebit(e, &m->g2[s], index == 3);
            if (index == 3) m->rep[3] = m->rep[2];
            m->rep[2] = m->rep[1];
        }
        m->rep[1] = m->rep[0];
        m->rep[0] = dist;
    }
    elength(e, &m->rep_len, len - 2, pos_state);
    m->state = s < 7 ? 8 : 11;
}

static inline u32 h4(const u8 *p) { return ((u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24) * 2654435761u >> (32 - HASH4); }
static inline u32 h3(const u8 *p) { return ((u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16) * 2654435761u >> (32 - HASH3); }

static void hash_insert(struct lzma2_enc *e, u32 at) {
    if (at + 4 > e->end) return;
    u32 abs_at = (u32)epos(e, at);
    const u8 *p = e->win + at;
    u32 k = h4(p);
    e->chain[abs_at & (DICT - 1)] = e->head4[k];
    e->head4[k] = abs_at + 1;
    e->head3[h3(p)] = abs_at + 1;
}

static void hash_upto(struct lzma2_enc *e, u32 at) {
    while (e->inserted < at) hash_insert(e, e->inserted++);
}

static u32 match_len(const u8 *a, const u8 *b, u32 limit) {
    u32 n = 0;
    while (n < limit && a[n] == b[n]) ++n;
    return n;
}

/* The longest match at AT (positions before AT hashed); DIST gets distance - 1. */
static u32 find(struct lzma2_enc *e, u32 at, u32 *dist) {
    u32 limit = e->end - at;
    if (limit > MATCH_MAX) limit = MATCH_MAX;
    if (limit < 4) return 0;
    u32 abs_at = (u32)epos(e, at), history = abs_at < DICT ? abs_at : DICT;
    if (history > at) history = at;
    const u8 *here = e->win + at;
    u32 best = 0, best_dist = 0;
    u32 c3 = e->head3[h3(here)];
    if (c3) {
        u32 d = abs_at - (c3 - 1);
        if (d && d <= history) {
            u32 n = match_len(here, here - d, limit);
            if (n >= 3) { best = n; best_dist = d; }
        }
    }
    u32 candidate = e->head4[h4(here)];
    for (int depth = 0; candidate && depth < DEPTH && best < limit && best < NICE; ++depth) {
        u32 d = abs_at - (candidate - 1);
        if (!d || d > history) break;
        const u8 *there = here - d;
        if (there[best] == here[best]) {
            u32 n = match_len(here, there, limit);
            if (n > best || (n == best && d < best_dist)) { best = n; best_dist = d; }
        }
        u32 next = e->chain[(candidate - 1) & (DICT - 1)];
        if (next && abs_at - (next - 1) <= d) break;
        candidate = next;
    }
    *dist = best_dist - 1;
    return best >= 3 || (best == 2 && best_dist < 128) ? best : 0;
}

static u32 rep_len(struct lzma2_enc *e, u32 at, int index, u32 limit) {
    u32 d = e->m.rep[index] + 1;
    if (d > at || epos(e, at) < d) return 0;
    return match_len(e->win + at, e->win + at - d, limit);
}

static int change_pair(u32 small, u32 big) { return (big >> 7) > small; }

static void chunk_header_flush(struct lzma2_enc *e);

static void end_chunk(struct lzma2_enc *e) {
    if (!e->chunk_unpacked) return;
    for (int i = 0; i < 5; ++i) shift_low(e);
    chunk_header_flush(e);
}

static void emit(struct lzma2_enc *e, const u8 *p, size_t n) {
    if (!e->error && n && e->out->write(e->out, p, n)) e->error = 1;
}

static void chunk_header_flush(struct lzma2_enc *e) {
    u32 u = e->chunk_unpacked;
    if (!e->overflow && e->chunk_n <= u) {
        u8 h[6];
        int reset = e->first ? 3 : e->need_props ? 2 : e->need_state ? 1 : 0;
        h[0] = (u8)(0x80 | reset << 5 | ((u - 1) >> 16));
        h[1] = (u8)((u - 1) >> 8);
        h[2] = (u8)(u - 1);
        h[3] = (u8)((e->chunk_n - 1) >> 8);
        h[4] = (u8)(e->chunk_n - 1);
        int n = 5;
        if (reset >= 2) h[n++] = (u8)((e->m.pb * 5 + e->m.lp) * 9 + e->m.lc);
        emit(e, h, (size_t)n);
        emit(e, e->chunk, e->chunk_n);
        e->need_props = e->need_state = 0;
    } else {
        const u8 *p = e->win + e->chunk_start;
        while (u) {
            u32 n = u > 65536 ? 65536 : u;
            u8 h[3] = {(u8)(e->first ? 1 : 2), (u8)((n - 1) >> 8), (u8)(n - 1)};
            emit(e, h, 3);
            emit(e, p, n);
            e->first = 0;
            p += n;
            u -= n;
        }
        e->need_state = 1;
    }
    e->first = 0;
    e->chunk_unpacked = 0;
    e->chunk_start = e->start;
}

static void begin_chunk(struct lzma2_enc *e) {
    rc_start(e);
    if (e->first || e->need_state) model_reset(&e->m);
    e->chunk_start = e->start;
}

static void advance(struct lzma2_enc *e, u32 n) {
    e->start += n;
    e->chunk_unpacked += n;
}

static void step(struct lzma2_enc *e) {
    if (!e->chunk_unpacked) begin_chunk(e);
    u32 at = e->start, avail = e->end - at;
    if (avail > MATCH_MAX) avail = MATCH_MAX;
    hash_upto(e, at);
    if (avail < 2 || !epos(e, at)) { encode_literal(e, at); advance(e, 1); goto check; }
    u32 best_rep = 0;
    int rep_index = 0;
    for (int i = 0; i < 4; ++i) {
        u32 n = rep_len(e, at, i, avail);
        if (n > best_rep) { best_rep = n; rep_index = i; }
    }
    if (best_rep >= NICE) { encode_rep(e, at, best_rep, rep_index); advance(e, best_rep); goto check; }
    u32 dist = 0, len = find(e, at, &dist);
    if (len >= NICE) { encode_match(e, at, len, dist); advance(e, len); goto check; }
    if (best_rep >= 2 && (best_rep + 1 >= len || (best_rep + 2 >= len && dist >= (1u << 9)) || (best_rep + 3 >= len && dist >= (1u << 15)))) {
        encode_rep(e, at, best_rep, rep_index);
        advance(e, best_rep);
        goto check;
    }
    if (len >= 2 && avail > 2) {
        hash_upto(e, at + 1);
        u32 next_dist = 0, next = find(e, at + 1, &next_dist);
        if (next >= 2 && ((next >= len && next_dist < dist) || (next == len + 1 && !change_pair(dist, next_dist)) || next > len + 1 ||
                          (next + 1 >= len && len >= 3 && change_pair(next_dist, dist))))
            len = 0;
    }
    if (len >= 2) { encode_match(e, at, len, dist); advance(e, len); goto check; }
    if (e->m.rep[0] + 1 <= at && e->win[at] == e->win[at - e->m.rep[0] - 1] && e->m.state >= 7) {
        encode_rep(e, at, 1, 0);
        advance(e, 1);
        goto check;
    }
    encode_literal(e, at);
    advance(e, 1);
check:
    if (e->chunk_unpacked + MATCH_MAX >= CHUNK_UNPACKED || e->chunk_n + e->cache_size + 64 >= CHUNK_PACKED) end_chunk(e);
}

static void encode_ready(struct lzma2_enc *e, int final) {
    while (!e->error && e->start < e->end && (final || e->end - e->start >= MATCH_MAX + 1)) step(e);
}

static void slide(struct lzma2_enc *e) {
    end_chunk(e);
    if (e->start <= DICT) return;
    u32 shift = e->start - DICT;
    hash_upto(e, e->start);
    memmove(e->win, e->win + shift, e->end - shift);
    e->base += shift;
    e->start -= shift;
    e->end -= shift;
    e->inserted -= shift;
    e->chunk_start = e->start;
}

struct lzma2_enc *lzma2_enc_new(struct sink *out, u8 *dict_prop) {
    struct lzma2_enc *e = calloc(1, sizeof(*e));
    if (!e) return 0;
    e->win = malloc(WINDOW);
    e->head4 = calloc(1u << HASH4, sizeof(u32));
    e->head3 = calloc(1u << HASH3, sizeof(u32));
    e->chain = malloc(sizeof(u32) * DICT);
    if (!e->win || !e->head4 || !e->head3 || !e->chain || model_props(&e->m, 3, 0, 2)) { lzma2_enc_free(e); return 0; }
    e->out = out;
    e->first = 1;
    e->need_props = 1;
    *dict_prop = 20;                  /* 2 << (20 / 2 + 11) = 4 MiB = DICT */
    return e;
}

int lzma2_enc_write(struct lzma2_enc *e, const u8 *data, size_t n) {
    while (n && !e->error) {
        if (e->end == WINDOW) slide(e);
        size_t room = WINDOW - e->end, k = n < room ? n : room;
        memcpy(e->win + e->end, data, k);
        e->end += (u32)k;
        data += k;
        n -= k;
        encode_ready(e, 0);
    }
    return e->error ? -1 : 0;
}

void lzma2_enc_free(struct lzma2_enc *e) {
    if (!e) return;
    free(e->win);
    free(e->head4);
    free(e->head3);
    free(e->chain);
    free(e->m.lit);
    free(e);
}

int lzma2_enc_finish(struct lzma2_enc *e) {
    encode_ready(e, 1);
    end_chunk(e);
    u8 zero = 0;
    emit(e, &zero, 1);
    int r = e->error ? -1 : 0;
    lzma2_enc_free(e);
    return r;
}
