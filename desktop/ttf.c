/*
 * DOS-XC /desktop/ttf.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "ttf.h"
#include <math.h>

static unsigned u8_at(const struct ttf *f, unsigned at) { return at < f->size ? f->data[at] : 0; }
static unsigned u16_at(const struct ttf *f, unsigned at) { return u8_at(f, at) << 8 | u8_at(f, at + 1); }
static int s16_at(const struct ttf *f, unsigned at) { return (short)u16_at(f, at); }
static unsigned u32_at(const struct ttf *f, unsigned at) { return u16_at(f, at) << 16 | u16_at(f, at + 2); }

static unsigned table(const struct ttf *f, const char *tag) {
    unsigned count = u16_at(f, f->base + 4);
    for (unsigned i = 0; i < count; ++i) {
        unsigned record = f->base + 12 + 16 * i;
        if (record + 16 <= f->size && !memcmp(f->data + record, tag, 4)) return u32_at(f, record + 8);
    }
    return 0;
}

int ttf_faces(const unsigned char *data, size_t size) {
    if (size >= 12 && !memcmp(data, "ttcf", 4)) return (int)((unsigned)data[8] << 24 | data[9] << 16 | data[10] << 8 | data[11]);
    return size >= 12 ? 1 : 0;
}

int ttf_open(struct ttf *f, const unsigned char *data, size_t size, int face) {
    memset(f, 0, sizeof(*f));
    f->data = data;
    f->size = size;
    if (size >= 12 && !memcmp(data, "ttcf", 4)) {
        if (face >= ttf_faces(data, size)) return -1;
        f->base = u32_at(f, 12 + 4 * (unsigned)face);
    }
    f->cmap = table(f, "cmap");
    f->head = table(f, "head");
    f->hhea = table(f, "hhea");
    f->hmtx = table(f, "hmtx");
    f->maxp = table(f, "maxp");
    f->name = table(f, "name");
    f->glyf = table(f, "glyf");
    f->loca = table(f, "loca");
    f->cff = table(f, "CFF ");
    if (!f->cmap || !f->head || !f->hhea || !f->hmtx || !f->maxp) return -1;
    f->units = (int)u16_at(f, f->head + 18);
    f->long_loca = s16_at(f, f->head + 50) != 0;
    f->glyph_count = (int)u16_at(f, f->maxp + 4);
    f->ascent = s16_at(f, f->hhea + 4);
    f->descent = s16_at(f, f->hhea + 6);
    f->metric_count = (int)u16_at(f, f->hhea + 34);
    if (!f->units) return -1;
    unsigned count = u16_at(f, f->cmap + 2);
    for (unsigned i = 0; i < count; ++i) {
        unsigned record = f->cmap + 4 + 8 * i, platform = u16_at(f, record), encoding = u16_at(f, record + 2);
        unsigned sub = f->cmap + u32_at(f, record + 4);
        int format = (int)u16_at(f, sub);
        int unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
        if (unicode && (format == 4 || format == 12) && (!f->subtable || format == 12)) {
            f->subtable = sub;
            f->subtable_format = format;
        }
    }
    return f->subtable ? 0 : -1;
}

int ttf_has_outlines(const struct ttf *f) { return f->glyf && f->loca; }

void ttf_name(const struct ttf *f, int id, char *out, size_t size) {
    out[0] = 0;
    if (!f->name || !size) return;
    unsigned count = u16_at(f, f->name + 2), strings = f->name + u16_at(f, f->name + 4);
    int best = -1;
    for (unsigned i = 0; i < count; ++i) {
        unsigned record = f->name + 6 + 12 * i;
        if ((int)u16_at(f, record + 6) != id) continue;
        unsigned platform = u16_at(f, record);
        if (best < 0 || platform == 3) best = (int)i;
    }
    if (best < 0) return;
    unsigned record = f->name + 6 + 12 * (unsigned)best, platform = u16_at(f, record);
    unsigned length = u16_at(f, record + 8), offset = strings + u16_at(f, record + 10);
    size_t n = 0;
    int wide = platform == 3 || platform == 0;
    for (unsigned i = 0; i < length && n + 1 < size; i += wide ? 2 : 1) {
        unsigned ch = wide ? u16_at(f, offset + i) : u8_at(f, offset + i);
        out[n++] = (char)(ch < 256 ? ch : '?');
    }
    out[n] = 0;
}

static int glyph_index(const struct ttf *f, unsigned code) {
    unsigned s = f->subtable;
    if (f->subtable_format == 12) {
        unsigned groups = u32_at(f, s + 12);
        for (unsigned i = 0; i < groups; ++i) {
            unsigned g = s + 16 + 12 * i, start = u32_at(f, g), end = u32_at(f, g + 4);
            if (code >= start && code <= end) return (int)(u32_at(f, g + 8) + code - start);
        }
        return 0;
    }
    if (code > 0xffff) return 0;
    unsigned segments = u16_at(f, s + 6) / 2, ends = s + 14, starts = ends + 2 * segments + 2;
    unsigned deltas = starts + 2 * segments, offsets = deltas + 2 * segments;
    for (unsigned i = 0; i < segments; ++i) {
        if (code > u16_at(f, ends + 2 * i)) continue;
        unsigned start = u16_at(f, starts + 2 * i);
        if (code < start) return 0;
        unsigned delta = u16_at(f, deltas + 2 * i), range = u16_at(f, offsets + 2 * i);
        if (!range) return (int)((code + delta) & 0xffff);
        unsigned glyph = u16_at(f, offsets + 2 * i + range + 2 * (code - start));
        return glyph ? (int)((glyph + delta) & 0xffff) : 0;
    }
    return 0;
}

static int advance(const struct ttf *f, int glyph) {
    int i = glyph < f->metric_count ? glyph : f->metric_count - 1;
    return i < 0 ? 0 : (int)u16_at(f, f->hmtx + 4 * (unsigned)i);
}

static unsigned glyph_offset(const struct ttf *f, int glyph, unsigned *length) {
    if (glyph < 0 || glyph >= f->glyph_count) return 0;
    unsigned a, b;
    if (f->long_loca) { a = u32_at(f, f->loca + 4 * (unsigned)glyph); b = u32_at(f, f->loca + 4 * (unsigned)glyph + 4); }
    else { a = 2 * u16_at(f, f->loca + 2 * (unsigned)glyph); b = 2 * u16_at(f, f->loca + 2 * (unsigned)glyph + 2); }
    *length = b > a ? b - a : 0;
    return f->glyf + a;
}

/* Edges in pixels, y down, with their winding direction. */
struct edge { float x0, y0, x1, y1; int dir; };
struct outline { struct edge *edges; int count, capacity; };
struct transform { float a, b, c, d, dx, dy; };

static void add_line(struct outline *o, float x0, float y0, float x1, float y1) {
    if (y0 == y1) return;
    if (o->count == o->capacity) {
        o->capacity = o->capacity ? o->capacity * 2 : 256;
        o->edges = realloc(o->edges, sizeof(*o->edges) * (size_t)o->capacity);
    }
    o->edges[o->count++] = y0 < y1 ? (struct edge){x0, y0, x1, y1, 1} : (struct edge){x1, y1, x0, y0, -1};
}

static void add_curve(struct outline *o, float x0, float y0, float cx, float cy, float x1, float y1) {
    float px = x0, py = y0;
    for (int i = 1; i <= 8; ++i) {
        float t = (float)i / 8, u = 1 - t;
        float x = u * u * x0 + 2 * u * t * cx + t * t * x1, y = u * u * y0 + 2 * u * t * cy + t * t * y1;
        add_line(o, px, py, x, y);
        px = x;
        py = y;
    }
}

static void glyph_outline(const struct ttf *f, int glyph, struct transform m, float scale, struct outline *o, int depth) {
    unsigned length, at = glyph_offset(f, glyph, &length);
    if (!length || depth > 4) return;
    int contours = s16_at(f, at);
    if (contours < 0) {
        unsigned p = at + 10, flags;
        do {
            flags = u16_at(f, p);
            int component = (int)u16_at(f, p + 2);
            p += 4;
            float dx, dy;
            if (flags & 1) { dx = (float)s16_at(f, p); dy = (float)s16_at(f, p + 2); p += 4; }
            else { dx = (float)(signed char)u8_at(f, p); dy = (float)(signed char)u8_at(f, p + 1); p += 2; }
            struct transform t = {1, 0, 0, 1, 0, 0};
            if (flags & 8) { t.a = t.d = (float)s16_at(f, p) / 16384; p += 2; }
            else if (flags & 0x40) { t.a = (float)s16_at(f, p) / 16384; t.d = (float)s16_at(f, p + 2) / 16384; p += 4; }
            else if (flags & 0x80) {
                t.a = (float)s16_at(f, p) / 16384; t.b = (float)s16_at(f, p + 2) / 16384;
                t.c = (float)s16_at(f, p + 4) / 16384; t.d = (float)s16_at(f, p + 6) / 16384;
                p += 8;
            }
            struct transform combined = {m.a * t.a + m.c * t.b, m.b * t.a + m.d * t.b, m.a * t.c + m.c * t.d, m.b * t.c + m.d * t.d,
                                         m.dx + (m.a * dx + m.c * dy) * scale, m.dy - (m.b * dx + m.d * dy) * scale};
            if (flags & 2) glyph_outline(f, component, combined, scale, o, depth + 1);
        } while (flags & 0x20);
        return;
    }
    unsigned ends = at + 10;
    int points = contours ? (int)u16_at(f, ends + 2 * (unsigned)(contours - 1)) + 1 : 0;
    if (points <= 0 || points > 4000) return;
    unsigned p = ends + 2 * (unsigned)contours;
    p += 2 + u16_at(f, p);
    unsigned char *flags = malloc((size_t)points);
    float *xs = malloc(sizeof(float) * (size_t)points), *ys = malloc(sizeof(float) * (size_t)points);
    for (int i = 0; i < points;) {
        unsigned flag = u8_at(f, p++), repeat = flag & 8 ? u8_at(f, p++) : 0;
        for (unsigned r = 0; r <= repeat && i < points; ++r) flags[i++] = (unsigned char)flag;
    }
    int v = 0;
    for (int i = 0; i < points; ++i) {
        if (flags[i] & 2) { int d = (int)u8_at(f, p++); v += flags[i] & 16 ? d : -d; }
        else if (!(flags[i] & 16)) { v += s16_at(f, p); p += 2; }
        xs[i] = (float)v;
    }
    v = 0;
    for (int i = 0; i < points; ++i) {
        if (flags[i] & 4) { int d = (int)u8_at(f, p++); v += flags[i] & 32 ? d : -d; }
        else if (!(flags[i] & 32)) { v += s16_at(f, p); p += 2; }
        ys[i] = (float)v;
    }
    for (int i = 0; i < points; ++i) {
        float x = xs[i] * scale, y = ys[i] * scale;
        xs[i] = m.dx + m.a * x + m.c * y;
        ys[i] = m.dy - (m.b * x + m.d * y);
    }
    int first = 0;
    for (int c = 0; c < contours; ++c) {
        int last = (int)u16_at(f, ends + 2 * (unsigned)c);
        if (last >= points || last < first) break;
        int n = last - first + 1, start = first;
        while (start <= last && !(flags[start] & 1)) ++start;
        float sx, sy;
        if (start > last) { sx = (xs[first] + xs[last]) / 2; sy = (ys[first] + ys[last]) / 2; start = first; }
        else { sx = xs[start]; sy = ys[start]; }
        float px = sx, py = sy, cx = 0, cy = 0;
        int have_control = 0;
        for (int k = 1; k <= n; ++k) {
            int i = first + (start - first + k) % n;
            float x = xs[i], y = ys[i];
            if (flags[i] & 1) {
                if (have_control) add_curve(o, px, py, cx, cy, x, y);
                else add_line(o, px, py, x, y);
                px = x; py = y; have_control = 0;
            } else {
                if (have_control) {
                    float mx = (cx + x) / 2, my = (cy + y) / 2;
                    add_curve(o, px, py, cx, cy, mx, my);
                    px = mx; py = my;
                }
                cx = x; cy = y; have_control = 1;
            }
        }
        if (have_control) add_curve(o, px, py, cx, cy, sx, sy);
        else add_line(o, px, py, sx, sy);
        first = last + 1;
    }
    free(flags);
    free(xs);
    free(ys);
}

#define SUB 4

/* Fill the outline with the non-zero rule into the canvas, 4 x 4 antialiased. */
static void fill_outline(const struct canvas *c, const struct outline *o, u32 color) {
    if (!o->count) return;
    float top = o->edges[0].y0, bottom = o->edges[0].y1, left = o->edges[0].x0, right = left;
    for (int i = 0; i < o->count; ++i) {
        const struct edge *e = &o->edges[i];
        if (e->y0 < top) top = e->y0;
        if (e->y1 > bottom) bottom = e->y1;
        float lo = e->x0 < e->x1 ? e->x0 : e->x1, hi = e->x0 < e->x1 ? e->x1 : e->x0;
        if (lo < left) left = lo;
        if (hi > right) right = hi;
    }
    int x0 = (int)floorf(left), x1 = (int)ceilf(right) + 1, y0 = (int)floorf(top), y1 = (int)ceilf(bottom);
    int w = x1 - x0;
    if (w <= 0 || w > 4096 || y1 - y0 > 4096) return;
    float *cover = calloc((size_t)w, sizeof(float));
    struct { float x; int dir; } cross[256];
    for (int y = y0; y < y1; ++y) {
        memset(cover, 0, sizeof(float) * (size_t)w);
        for (int s = 0; s < SUB; ++s) {
            float sy = (float)y + ((float)s + 0.5f) / SUB;
            int n = 0;
            for (int i = 0; i < o->count && n < 256; ++i) {
                const struct edge *e = &o->edges[i];
                if (sy < e->y0 || sy >= e->y1) continue;
                cross[n].x = e->x0 + (sy - e->y0) * (e->x1 - e->x0) / (e->y1 - e->y0);
                cross[n++].dir = e->dir;
            }
            for (int i = 1; i < n; ++i)
                for (int k = i; k > 0 && cross[k].x < cross[k - 1].x; --k) {
                    __typeof__(cross[0]) t = cross[k]; cross[k] = cross[k - 1]; cross[k - 1] = t;
                }
            int winding = 0;
            for (int i = 0; i + 1 < n; ++i) {
                winding += cross[i].dir;
                if (!winding) continue;
                float a = cross[i].x - (float)x0, b = cross[i + 1].x - (float)x0;
                for (int px = (int)floorf(a); px <= (int)floorf(b) && px < w; ++px) {
                    if (px < 0) continue;
                    float lo = a > (float)px ? a : (float)px, hi = b < (float)(px + 1) ? b : (float)(px + 1);
                    if (hi > lo) cover[px] += (hi - lo) / SUB;
                }
            }
        }
        int dy = y + c->oy;
        if (y < c->clip.y || y >= c->clip.y + c->clip.h || dy < 0 || dy >= c->s->h) continue;
        for (int i = 0; i < w; ++i) {
            int x = x0 + i, dx = x + c->ox;
            float a = cover[i] > 1 ? 1 : cover[i];
            if (a <= 0 || x < c->clip.x || x >= c->clip.x + c->clip.w || dx < 0 || dx >= c->s->w) continue;
            u32 *px = &c->s->px[dy * c->s->w + dx];
            unsigned alpha = (unsigned)(a * 256);
            u32 r = ((*px >> 16 & 255) * (256 - alpha) + (color >> 16 & 255) * alpha) >> 8;
            u32 g = ((*px >> 8 & 255) * (256 - alpha) + (color >> 8 & 255) * alpha) >> 8;
            u32 b = ((*px & 255) * (256 - alpha) + (color & 255) * alpha) >> 8;
            *px = r << 16 | g << 8 | b;
        }
    }
    free(cover);
}

int ttf_width(const struct ttf *f, int pixels, const char *text) {
    float scale = (float)pixels / (float)f->units, x = 0;
    for (const unsigned char *t = (const unsigned char *)text; *t; ++t) x += (float)advance(f, glyph_index(f, *t)) * scale;
    return (int)ceilf(x);
}

int ttf_draw(const struct ttf *f, const struct canvas *c, int x, int y, int pixels, const char *text, u32 color) {
    float scale = (float)pixels / (float)f->units, pen = (float)x;
    for (const unsigned char *t = (const unsigned char *)text; *t; ++t) {
        int glyph = glyph_index(f, *t);
        if (ttf_has_outlines(f) && *t != ' ') {
            struct outline o = {0};
            glyph_outline(f, glyph, (struct transform){1, 0, 0, 1, pen, (float)y}, scale, &o, 0);
            fill_outline(c, &o, color);
            free(o.edges);
        }
        pen += (float)advance(f, glyph) * scale;
    }
    return (int)ceilf(pen) - x;
}
