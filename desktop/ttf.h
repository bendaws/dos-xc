/*
 * DOS-XC /desktop/ttf.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once
#include "wm.h"

/* A TrueType font (one face of a .ttf, .otf or .ttc) held in memory. */
struct ttf {
    const unsigned char *data;
    size_t size;
    unsigned base, cmap, glyf, loca, head, hhea, hmtx, maxp, name, cff;
    int long_loca, glyph_count, units, ascent, descent, metric_count;
    unsigned subtable;          /* the Unicode cmap subtable used */
    int subtable_format;
};

int ttf_open(struct ttf *f, const unsigned char *data, size_t size, int face);   /* 0 when usable */
int ttf_faces(const unsigned char *data, size_t size);
int ttf_has_outlines(const struct ttf *f);   /* 0 for PostScript (CFF) outlines, which are not drawn */
void ttf_name(const struct ttf *f, int id, char *out, size_t size);              /* 1 family, 2 style, 4 full name */
/* Draws Latin-1 TEXT with its baseline at Y; returns the width in pixels. */
int ttf_draw(const struct ttf *f, const struct canvas *c, int x, int y, int pixels, const char *text, u32 color);
int ttf_width(const struct ttf *f, int pixels, const char *text);
