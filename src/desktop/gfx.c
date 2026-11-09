/*
 * DOS-XC /desktop/gfx.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * Drawing: clipped rectangles, gradients, blits, bitmap text and the
 * Windows-2000-style bevels of the theme.
 */
#include "wm.h"
#include "fonts.h"
#include "logo.h"

const struct font font_regular = {KATICA_HEIGHT, KATICA_SPACING, katica_widths, katica_rows};
const struct font font_bold = {KATICA_BOLD_HEIGHT, KATICA_BOLD_SPACING, katica_bold_widths, katica_bold_rows};
const struct font font_mono = {CSILLA_HEIGHT, CSILLA_SPACING, csilla_widths, csilla_rows};

/* ---- the accent color ---------------------------------------------------- */

static const struct theme default_theme = {0x6e2209, 0xf4ca9e, 0x421405, 0xa1320d, 0xfadcbb, 0x601e07, 0x84351a, 0xd2c0b6};
struct theme theme = {0x6e2209, 0xf4ca9e, 0x421405, 0xa1320d, 0xfadcbb, 0x601e07, 0x84351a, 0xd2c0b6};

static u32 color_scale(u32 c, int percent) {
    u32 out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        int v = (int)(c >> shift & 255) * percent / 100;
        out |= (u32)(v > 255 ? 255 : v) << shift;
    }
    return out;
}

static u32 color_toward_white(u32 c, int percent) {
    u32 out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        int v = (int)(c >> shift & 255);
        out |= (u32)(v + (255 - v) * percent / 100) << shift;
    }
    return out;
}

/* Every themed color comes from one choice.  Selections and title bars carry
   white text, so a light accent is darkened until that text can be read. */
void theme_set(u32 accent) {
    if (accent == ACCENT_DEFAULT) { theme = default_theme; return; }
    int luma = ((int)(accent >> 16 & 255) * 30 + (int)(accent >> 8 & 255) * 59 + (int)(accent & 255) * 11) / 100;
    u32 base = luma > 120 ? color_scale(accent, 120 * 100 / luma) : accent;
    theme.selection = base;
    theme.active1 = color_scale(base, 85);
    theme.active2 = color_toward_white(base, 72);
    theme.active_shadow = color_scale(base, 50);
    theme.moving1 = color_scale(base, 120);
    theme.moving2 = color_toward_white(base, 80);
    theme.moving_shadow = color_scale(base, 72);
    theme.menu_selection = color_toward_white(base, 65);
}

struct rect rect_both(struct rect a, struct rect b) {
    int x = a.x > b.x ? a.x : b.x, y = a.y > b.y ? a.y : b.y;
    int r = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w, e = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    return (struct rect){x, y, r > x ? r - x : 0, e > y ? e - y : 0};
}

struct rect rect_either(struct rect a, struct rect b) {
    if (!a.w || !a.h) return b;
    if (!b.w || !b.h) return a;
    int x = a.x < b.x ? a.x : b.x, y = a.y < b.y ? a.y : b.y;
    int r = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w, e = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return (struct rect){x, y, r - x, e - y};
}

int surface_size(struct surface *s, int w, int h) {
    if (w * h > s->capacity) {
        u32 *px = malloc((size_t)w * h * 4);
        if (!px) return -1;
        free(s->px);
        s->px = px;
        s->capacity = w * h;
    }
    s->w = w;
    s->h = h;
    return 0;
}

void fill(const struct canvas *c, int x, int y, int w, int h, u32 color) {
    struct rect r = rect_both((struct rect){x, y, w, h}, c->clip);
    for (int j = 0; j < r.h; ++j) {
        u32 *p = c->s->px + (r.y + j + c->oy) * c->s->w + r.x + c->ox;
        for (int i = 0; i < r.w; ++i) p[i] = color;
    }
}

void gradient(const struct canvas *c, int x, int y, int w, int h, u32 left, u32 right) {
    struct rect r = rect_both((struct rect){x, y, w, h}, c->clip);
    if (w <= 1 || !r.w) { fill(c, x, y, w, h, left); return; }
    for (int i = 0; i < r.w; ++i) {
        int at = r.x + i - x, span = w - 1;
        int cr = (int)(left >> 16 & 255), cg = (int)(left >> 8 & 255), cb = (int)(left & 255);
        cr += ((int)(right >> 16 & 255) - cr) * at / span;
        cg += ((int)(right >> 8 & 255) - cg) * at / span;
        cb += ((int)(right & 255) - cb) * at / span;
        u32 color = (u32)cr << 16 | (u32)cg << 8 | (u32)cb;
        for (int j = 0; j < r.h; ++j) c->s->px[(r.y + j + c->oy) * c->s->w + r.x + i + c->ox] = color;
    }
}

void blit(const struct canvas *c, int x, int y, const struct surface *from) {
    struct rect r = rect_both((struct rect){x, y, from->w, from->h}, c->clip);
    for (int j = 0; j < r.h; ++j)
        memcpy(c->s->px + (r.y + j + c->oy) * c->s->w + r.x + c->ox,
               from->px + (r.y + j - y) * from->w + (r.x - x), (size_t)r.w * 4);
}

void draw_pixels(const struct canvas *c, int x, int y, int w, int h, const u32 *px) {
    struct rect r = rect_both((struct rect){x, y, w, h}, c->clip);
    for (int j = 0; j < r.h; ++j) {
        const u32 *from = px + (r.y + j - y) * w + (r.x - x);
        u32 *to = c->s->px + (r.y + j + c->oy) * c->s->w + r.x + c->ox;
        for (int i = 0; i < r.w; ++i)
            if (from[i]) to[i] = from[i] & 0xffffff;
    }
}

void draw_icon(const struct canvas *c, int x, int y, int icon) {
    draw_pixels(c, x, y, ICON_SIZE, ICON_SIZE, icons[icon].px);
}

int text_width(const struct font *f, const char *s) {
    int w = 0;
    for (; *s; ++s) w += f->widths[(unsigned char)*s] + f->spacing;
    return w ? w - f->spacing : 0;
}

void text(const struct canvas *c, int x, int y, const struct font *f, const char *s, u32 color) {
    for (; *s; ++s) {
        unsigned ch = (unsigned char)*s;
        int width = f->widths[ch];
        for (int row = 0; row < f->height; ++row) {
            unsigned bits = f->rows[ch * f->height + row];
            for (int col = 0; col < width; ++col)
                if (bits >> col & 1) {
                    int px = x + col, py = y + row;
                    if (rect_has(c->clip, px, py)) c->s->px[(py + c->oy) * c->s->w + px + c->ox] = color;
                }
        }
        x += width + f->spacing;
    }
}

void text_big(const struct canvas *c, int x, int y, const char *s, u32 color, int scale) {
    for (; *s; ++s) {
        unsigned ch = (unsigned char)*s;
        int width = font_bold.widths[ch];
        for (int row = 0; row < font_bold.height; ++row) {
            unsigned bits = font_bold.rows[ch * font_bold.height + row];
            for (int col = 0; col < width; ++col)
                if (bits >> col & 1) fill(c, x + col * scale, y + row * scale, scale, scale, color);
        }
        x += (width + 1) * scale;
    }
}

void frame_box(const struct canvas *c, int x, int y, int w, int h, u32 color) {
    fill(c, x, y, w, 1, color);
    fill(c, x, y + h - 1, w, 1, color);
    fill(c, x, y, 1, h, color);
    fill(c, x + w - 1, y, 1, h, color);
}

void bevel(const struct canvas *c, int x, int y, int w, int h, int sunken) {
    u32 tl = sunken ? C_SHADOW1 : C_HIGHLIGHT, tl2 = sunken ? C_SHADOW2 : C_WINDOW;
    u32 br = sunken ? C_HIGHLIGHT : C_SHADOW2, br2 = sunken ? C_WINDOW : C_SHADOW1;
    fill(c, x, y, w, 1, tl);
    fill(c, x, y, 1, h, tl);
    fill(c, x + 1, y + 1, w - 2, 1, tl2);
    fill(c, x + 1, y + 1, 1, h - 2, tl2);
    fill(c, x, y + h - 1, w, 1, br);
    fill(c, x + w - 1, y, 1, h, br);
    fill(c, x + 1, y + h - 2, w - 2, 1, br2);
    fill(c, x + w - 2, y + 1, 1, h - 2, br2);
}

void button(const struct canvas *c, struct rect r, const char *label, int pressed, int bold) {
    fill(c, r.x, r.y, r.w, r.h, C_WINDOW);
    bevel(c, r.x, r.y, r.w, r.h, pressed);
    const struct font *f = bold ? &font_bold : &font_regular;
    text(c, r.x + (r.w - text_width(f, label)) / 2 + pressed, r.y + (r.h - f->height) / 2 + pressed, f, label, C_TEXT);
}

/* ---- icons, 16 x 16; each character is a palette entry ---------------- */

static u32 palette(char ch) {
    switch (ch) {
    case 'K': return 0xff000000;
    case 'W': return 0xffffffff;
    case 'G': return 0xff808080;
    case 'L': return 0xffc0c0c0;
    case 'D': return 0xff404040;
    case 'Y': return 0xfff4d068;
    case 'O': return 0xffc09028;
    case 'R': return 0xffc02020;
    case 'B': return 0xff2050a0;
    case 'C': return 0xff60a0d0;
    case 'E': return 0xff20a040;
    }
    return 0;
}

static const char *const art[ICON_COUNT - 1] = {
    /* FOLDER */
    "................"
    "................"
    "..KKKKK........."
    ".KYYYYYK........"
    "KOOOOOOOKKKKKKK."
    "KYYYYYYYYYYYYYOK"
    "KYYYYYYYYYYYYYOK"
    "KYYYYYYYYYYYYYOK"
    "KYYYYYYYYYYYYYOK"
    "KYYYYYYYYYYYYYOK"
    "KYYYYYYYYYYYYYOK"
    "KYYYYYYYYYYYYYOK"
    "KOOOOOOOOOOOOOOK"
    ".KKKKKKKKKKKKKK."
    "................"
    "................",
    /* FILE */
    "................"
    "..KKKKKKKK......"
    "..KWWWWWWKK....."
    "..KWWWWWWKLK...."
    "..KWLLLLWKKKK..."
    "..KWWWWWWWWWK..."
    "..KWLLLLLLLWK..."
    "..KWWWWWWWWWK..."
    "..KWLLLLLLLWK..."
    "..KWWWWWWWWWK..."
    "..KWLLLLLLLWK..."
    "..KWWWWWWWWWK..."
    "..KWWWWWWWWWK..."
    "..KKKKKKKKKKK..."
    "................"
    "................",
    /* PROGRAM */
    "................"
    ".KKKKKKKKKKKKKK."
    ".KBBBBBBBBBBBWK."
    ".KKKKKKKKKKKKKK."
    ".KWWWWWWWWWWWWK."
    ".KWLLLLWWWWWWWK."
    ".KWLLLLWWWWWWWK."
    ".KWLLLLWWWWWWWK."
    ".KWWWWWWWLLLLWK."
    ".KWWWWWWWLLLLWK."
    ".KWWWWWWWLLLLWK."
    ".KWWWWWWWWWWWWK."
    ".KWWWWWWWWWWWWK."
    ".KKKKKKKKKKKKKK."
    "................"
    "................",
    /* DRIVE */
    "................"
    "................"
    "................"
    "...KKKKKKKKKK..."
    "..KLLLLLLLLLLK.."
    ".KLLLLLLLLLLLLK."
    ".KGGGGGGGGGGGGK."
    ".KWWWWWWWWWWWWK."
    ".KLLLLLLLLLEELK."
    ".KLLLLLLLLLLLLK."
    ".KGGGGGGGGGGGGK."
    ".KKKKKKKKKKKKKK."
    "................"
    "................"
    "................"
    "................",
    /* PROMPT */
    "................"
    ".KKKKKKKKKKKKKK."
    ".KLLLLLLLLLLLLK."
    ".KKKKKKKKKKKKKK."
    ".KDDDDDDDDDDDDK."
    ".KDWDDDDDDDDDDK."
    ".KDDWDDDDDDDDDK."
    ".KDDDWDDDDDDDDK."
    ".KDDWDDDDDDDDDK."
    ".KDWDDDDWWWWDDK."
    ".KDDDDDDDDDDDDK."
    ".KKKKKKKKKKKKKK."
    "................"
    "................"
    "................"
    "................",
    /* DISPLAY */
    "................"
    ".KKKKKKKKKKKKKK."
    ".KLLLLLLLLLLLLK."
    ".KLCCCCCCCCCCLK."
    ".KLCCCCCCCCCCLK."
    ".KLCCCCCCCCCCLK."
    ".KLCCCCCCCCCCLK."
    ".KLCCCCCCCCCCLK."
    ".KLCCCCCCCCCCLK."
    ".KLLLLLLLLLLLLK."
    ".KKKKKKKKKKKKKK."
    "......KLLK......"
    "......KLLK......"
    "....KKKKKKKK...."
    "................"
    "................",
    /* INFO */
    "................"
    ".....KKKKKK....."
    "...KKBBBBBBKK..."
    "..KBBBBWWBBBBK.."
    "..KBBBBWWBBBBK.."
    ".KBBBBBBBBBBBBK."
    ".KBBBBWWWBBBBBK."
    ".KBBBBBWWBBBBBK."
    ".KBBBBBWWBBBBBK."
    ".KBBBBBWWBBBBBK."
    "..KBBBBWWBBBBK.."
    "..KBBBWWWWBBBK.."
    "...KKBBBBBBKK..."
    ".....KKKKKK....."
    "................"
    "................",
    /* EXIT */
    "................"
    "...KKKKKKKKK...."
    "...KLLLLLLLK...."
    "...KLLLLLLLK...."
    "...KLLLLKLLK...."
    "...KLLLLKKLK...K"
    "...KLLLLKWKK..KK"
    "...KLLLLKWWK.KRK"
    "...KLLLLKWKKKRRK"
    "...KLLLLKKLKKRRK"
    "...KLLLLKLLK.KRK"
    "...KLLLLLLLK..KK"
    "...KKKKKKKKK...K"
    "................"
    "................"
    "................",
    /* POWER */
    "................"
    ".......RR......."
    "...KK..RR..KK..."
    "..KRR..RR..RRK.."
    ".KRR...RR...RRK."
    ".KR....RR....RK."
    ".KR....RR....RK."
    ".KR..........RK."
    ".KR..........RK."
    ".KRR........RRK."
    "..KRR......RRK.."
    "...KRRRRRRRRK..."
    "....KKKKKKKK...."
    "................"
    "................"
    "................",
    /* RESTART */
    "................"
    ".....KKKKKK....."
    "...KKEEEEEEKK..."
    "..KEEEKKKKEEEK.."
    ".KEEK.....KEEKK."
    ".KEK.......KEEEK"
    ".KEK.......KEEEK"
    ".KEK..........K."
    ".KEK..........K."
    ".KEEK....KK....."
    "..KEEEKKKEEK...."
    "...KKEEEEEK....."
    ".....KKKKK......"
    "................"
    "................"
    "................",
    /* CALC */
    "................"
    "..KKKKKKKKKKKK.."
    "..KLLLLLLLLLLK.."
    "..KLKKKKKKKKLK.."
    "..KLKEEEEEEKLK.."
    "..KLKKKKKKKKLK.."
    "..KLLLLLLLLLLK.."
    "..KLWWLWWLWWLK.."
    "..KLWWLWWLWWLK.."
    "..KLLLLLLLLLLK.."
    "..KLWWLWWLRRLK.."
    "..KLWWLWWLRRLK.."
    "..KLLLLLLLLLLK.."
    "..KKKKKKKKKKKK.."
    "................"
    "................",
    /* EDITOR */
    "................"
    "..KKKKKKKKKK...."
    "..KWWWWWWWWKK..."
    "..KWLLLLLLWKOK.."
    "..KWWWWWWWWKOOK."
    "..KWLLLLLLWWKOOK"
    "..KWWWWWWWWWWKOK"
    "..KWLLLLLWWWWWKK"
    "..KWWWWWWWWWWWWK"
    "..KWLLLLLLLLLWWK"
    "..KWWWWWWWWWWWWK"
    "..KWLLLLLLLLLWWK"
    "..KWWWWWWWWWWWWK"
    "..KKKKKKKKKKKKKK"
    "................"
    "................",
    /* PAINT */
    "................"
    "......KKKK......"
    "....KKOOOOKK...."
    "...KOOOOOOOOK..."
    "..KOORROOOOEEK.."
    "..KOORROOOOEEK.."
    ".KOOOOOOOOOOOOK."
    ".KOOOOKKKOOOOOK."
    ".KOOOK...KOBBOK."
    ".KOOOK...KOBBOK."
    ".KOOOOKKKOOOOOK."
    "..KOOYYOOOOOOK.."
    "..KOOYYOOOOKK..."
    "...KKOOOOOK....."
    ".....KKKKK......"
    "................",
    /* TASKS */
    "................"
    ".KKKKKKKKKKKKKK."
    ".KLLLLLLLLLLLLK."
    ".KKKKKKKKKKKKKK."
    ".KDDDDDDDDDDDDK."
    ".KDDDDDDDDEDDDK."
    ".KDDDDDDDEDEDDK."
    ".KDDEDDDEDDDEDK."
    ".KDEDEDEDDDDDEK."
    ".KEDDDEDDDDDDDK."
    ".KDDDDDDDDDDDDK."
    ".KKKKKKKKKKKKKK."
    "................"
    "................"
    "................"
    "................",
    /* CARDS */
    "................"
    ".KKKKKKKK......."
    ".KWWWWWWKKKKKK.."
    ".KWRWWWWKWWWWWK."
    ".KWRRWWWKWWWKWK."
    ".KRRRRWWKWWKKWK."
    ".KWRRWWWKWKKKWK."
    ".KWRWWWWKKKKKWK."
    ".KWWWWWWKWKKKWK."
    ".KWWWWWWKWWKWWK."
    ".KKKKKKKKWWKWWK."
    "........KWWWWWK."
    "........KKKKKKK."
    "................"
    "................"
    "................",
    /* MINES */
    "................"
    ".......KK......."
    "..K....KK....K.."
    "...K.KKKKKK.K..."
    "....KKKKKKKK...."
    "...KKWWKKKKKK..."
    "...KKWKKKKKKK..."
    ".KKKKKKKKKKKKKK."
    ".KKKKKKKKKKKKKK."
    "...KKKKKKKKKK..."
    "...KKKKKKKKKK..."
    "....KKKKKKKK...."
    "...K.KKKKKK.K..."
    "..K....KK....K.."
    ".......KK......."
    "................",
    /* CHESS */
    "................"
    "......KKKK......"
    ".....KLLLLK....."
    "....KLLLLLLK...."
    "....KLLLLLLK...."
    ".....KLLLLK....."
    "......KLLK......"
    "......KLLK......"
    ".....KLLLLK....."
    ".....KLLLLK....."
    "....KLLLLLLK...."
    "...KLLLLLLLLK..."
    "..KLLLLLLLLLLK.."
    "..KKKKKKKKKKKK.."
    "................"
    "................",
    /* PICTURE */
    "................"
    ".KKKKKKKKKKKKKK."
    ".KCCCCCCCCCCCCK."
    ".KCCCCCCCCYYCCK."
    ".KCCCCCCCCYYCCK."
    ".KCCCCCCCCCCCCK."
    ".KCCCCEECCCCCCK."
    ".KCCCEEEECCCEEK."
    ".KCCEEEEEECEEEK."
    ".KCEEEEEEEEEEEK."
    ".KEEEEEEEEEEEEK."
    ".KKKKKKKKKKKKKK."
    "................"
    "................"
    "................"
    "................",
    /* COMPUTER */
    "................"
    "..KKKKKKKKKKKK.."
    "..KLLLLLLLLLLK.."
    "..KLBBBBBBBBLK.."
    "..KLBBBBBBBBLK.."
    "..KLBBBBBBBBLK.."
    "..KLBBBBBBBBLK.."
    "..KLLLLLLLLLLK.."
    "..KKKKKKKKKKKK.."
    "......KLLK......"
    "....KKKKKKKK...."
    ".KKKKKKKKKKKKKK."
    ".KLLLLLLLLLEELK."
    ".KKKKKKKKKKKKKK."
    "................"
    "................",
    /* STAR */
    "................"
    ".......KK......."
    ".......KK......."
    "......KYYK......"
    "......KYYK......"
    ".KKKKKKYYKKKKKK."
    ".KYYYYYYYYYYYYK."
    "..KYYYYYYYYYYK.."
    "...KYYYYYYYYK..."
    "....KYYYYYYK...."
    "....KYYYYYYK...."
    "...KYYKKKKYYK..."
    "...KYK....KYK..."
    "...KK......KK..."
    "................"
    "................",
    /* SOUND */
    "................"
    ".......KK......."
    "......KKK...K..."
    ".....KKKK....K.."
    "KKKKKKKKK..K..K."
    "KLLLLLKKK...K.K."
    "KLLLLLKKK...K.K."
    "KLLLLLKKK...K.K."
    "KLLLLLKKK...K.K."
    "KKKKKKKKK..K..K."
    ".....KKKK....K.."
    "......KKK...K..."
    ".......KK......."
    "................"
    "................"
    "................",
    /* NET */
    "................"
    ".KKKKKKKK......."
    ".KCCCCCCK......."
    ".KCCCCCCK......."
    ".KKKKKKKK......."
    "....KK.........."
    "....KK.EEEEEEEE."
    "....KKKE......E."
    "...KKK.E.EEEE.E."
    "...KKK.E......E."
    ".......EEEEEEEE."
    "..........EE...."
    ".......EEEEEEEE."
    "................"
    "................"
    "................",
    /* NET_OFF */
    "................"
    ".KKKKKKKK......."
    ".KLLLLLLK......."
    ".KLLLLLLK......."
    ".KKKKKKKK......."
    "....KK.........."
    "....KK.GGGGGGGG."
    "....KKKG......G."
    "...KKK.GRRRRRRG."
    "...KKK.G......G."
    ".......GGGGGGGG."
    "..........GG...."
    ".......GGGGGGGG."
    "................"
    "................"
    "................",
    /* PACKAGE */
    "................"
    "......OOOO......"
    "....OOYYYYOO...."
    "..OOYYYYYYYYOO.."
    ".OYYYYYYYYYYYYO."
    ".OOOYYYYYYYYOOO."
    ".OYYOOYYYYOOYYO."
    ".OYYYYOOOOYYYYO."
    ".OYYYYYOOYYYYYO."
    ".OYYYYYOOYYYYYO."
    ".OYYYYYOOYYYYYO."
    ".OOYYYYOOYYYYOO."
    "...OOYYOOYYOO..."
    ".....OOOOOO....."
    "................"
    "................",
    /* SETUP */
    "................"
    "..KKKKKKKKKKKK.."
    "..KBBBBBBBBBBK.."
    "..KBKKKKKKKKBK.."
    "..KBBBBBBBBBBK.."
    "..KBKKKKKKKKBK.."
    "..KBBBBBBBBBBK.."
    "..KKKKKKKKKKKK.."
    "......KLLK......"
    "....KKKKKKKK...."
    "...KLLLLLLLLK..."
    "..KLLLLEELLLLK.."
    "..KKKKKKKKKKKK.."
    "................"
    "................"
    "................",
    /* GEAR */
    "................"
    "......GGGG......"
    "..GG..GGGG..GG.."
    "..GGGGGGGGGGGG.."
    "...GGG....GGG..."
    "...GG......GG..."
    ".GGGG......GGGG."
    ".GGGG......GGGG."
    ".GGGG......GGGG."
    ".GGGG......GGGG."
    "...GG......GG..."
    "...GGG....GGG..."
    "..GGGGGGGGGGGG.."
    "..GG..GGGG..GG.."
    "......GGGG......"
    "................",
    /* GAMES */
    "................"
    "................"
    "....KKKKKKKK...."
    "..KKLLLLLLLLKK.."
    ".KLLLLLLLLLLLLK."
    ".KLKLLLLLLLRLLK."
    ".KKKKLLLLLRLRLK."
    ".KLKLLLLLLLRLLK."
    ".KLLLLLLLLLLLLK."
    ".KLLLKKKKKKLLLK."
    ".KLLK......KLLK."
    "..KK........KK.."
    "................"
    "................"
    "................"
    "................",
    /* TOOLS */
    "................"
    "...........KK..."
    ".........KKLLK.."
    "........KLLLK..."
    "........KLLK.K.."
    "......K..KLLKLK."
    ".....KLK.KLLLK.."
    "....KLK..KKKK..."
    "...KLK.........."
    "..KLK..........."
    ".KLK............"
    ".KK............."
    "................"
    "................"
    "................"
    "................",
};

struct icon icons[ICON_COUNT];

void icons_init(void) {
    memcpy(icons[ICON_LOGO].px, logo16, sizeof(logo16));
    for (int n = 0; n < ICON_COUNT - 1; ++n)
        for (int i = 0; i < ICON_SIZE * ICON_SIZE; ++i)
            icons[n + 1].px[i] = palette(art[n][i]);
}

const unsigned *about_logo(void) { return logo32; }
