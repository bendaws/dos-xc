/*
 * DOS-XC /desktop/vdm/video.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "vdm.h"
#include "../fonts.h"
#include <string.h>

u8 bios_font8[256 * 8];

enum { TEXT, CGA4, CGA2, PLANAR, VGA256, MODEX };

#define BDA 0x400
#define MAX_W 640
#define MAX_H 480
#define TEXT_BASE 0xb8000
#define PAGE_SIZE_TEXT 4096

static int mode_number = 3, kind = TEXT, width_px = 640, height_px = 400, cols = 80, rows = 25;
static int claimed, dirty, active_page, published;
static int pub_graphics, pub_w, pub_h, pub_row = -1, pub_col, pub_cursor_on;
static u32 pixels[MAX_W * MAX_H];
static u8 planes[4][65536], latch[4];
static u8 attr_regs[21], attr_index, attr_flip, seq[8], seq_index, gc[9], gc_index;
static u8 crtc[32], crtc_index, misc_output = 0x67, cga_select, cga_mode;
static u8 dac[256][3], dac_write, dac_read, dac_sub_write, dac_sub_read;
static u32 rgb[256];
static int cursor_visible = 1;

static const u32 ega16[16] = {0x000000, 0x0000aa, 0x00aa00, 0x00aaaa, 0xaa0000, 0xaa00aa, 0xaa5500, 0xaaaaaa,
                              0x555555, 0x5555ff, 0x55ff55, 0x55ffff, 0xff5555, 0xff55ff, 0xffff55, 0xffffff};

static void dac_changed(int i) {
    rgb[i] = (u32)(dac[i][0] * 255 / 63) << 16 | (u32)(dac[i][1] * 255 / 63) << 8 | (u32)(dac[i][2] * 255 / 63);
    dirty = 1;
}

static void dac_set(int i, u8 r, u8 g, u8 b) { dac[i][0] = r; dac[i][1] = g; dac[i][2] = b; dac_changed(i); }

static void default_palette(void) {
    for (int i = 0; i < 16; ++i) dac_set(i, (u8)((ega16[i] >> 16 & 255) * 63 / 255), (u8)((ega16[i] >> 8 & 255) * 63 / 255), (u8)((ega16[i] & 255) * 63 / 255));
    for (int i = 16; i < 32; ++i) { u8 v = (u8)((i - 16) * 63 / 15); dac_set(i, v, v, v); }
    for (int i = 32; i < 248; ++i) {
        int hue = (i - 32) % 24, level = (i - 32) / 24 % 9;
        int v = 63 - level * 7;
        int section = hue / 4, f = (hue % 4) * v / 4;
        int r = section == 0 || section == 5 ? v : section == 1 ? v - f : section == 4 ? f : 0;
        int g = section == 1 || section == 2 ? v : section == 0 ? f : section == 3 ? v - f : 0;
        int b = section == 3 || section == 4 ? v : section == 2 ? f : section == 5 ? v - f : 0;
        dac_set(i, (u8)r, (u8)g, (u8)b);
    }
    for (int i = 248; i < 256; ++i) dac_set(i, 0, 0, 0);
}

static int ror8(int v, int n) { n &= 7; return (v >> n | v << (8 - n)) & 0xff; }

/* ---- the screen memory ------------------------------------------------------- */

static void planar_write(u32 offset, u8 value) {
    int mode = gc[5] & 3, rotate = gc[3] & 7, function = gc[3] >> 3 & 3;
    u8 mask = gc[8];
    for (int p = 0; p < 4; ++p) {
        if (!(seq[2] >> p & 1)) continue;
        u8 data = 0, bit = (u8)(1 << p), use_mask = mask;
        switch (mode) {
        case 1: planes[p][offset] = latch[p]; continue;
        case 0:
            data = (u8)ror8(value, rotate);
            if (gc[1] & bit) data = gc[0] & bit ? 0xff : 0;
            break;
        case 2: data = value & bit ? 0xff : 0; break;
        default:
            use_mask = (u8)(mask & ror8(value, rotate));
            data = gc[0] & bit ? 0xff : 0;
            break;
        }
        switch (function) {
        case 1: data &= latch[p]; break;
        case 2: data |= latch[p]; break;
        case 3: data ^= latch[p]; break;
        }
        planes[p][offset] = (u8)((data & use_mask) | (latch[p] & ~use_mask));
    }
    dirty = 1;
}

static u8 planar_read(u32 offset) {
    for (int p = 0; p < 4; ++p) latch[p] = planes[p][offset];
    if (!(gc[5] & 8)) return latch[gc[4] & 3];
    u8 result = 0xff;
    for (int p = 0; p < 4; ++p) {
        if (!(gc[7] >> p & 1)) continue;
        u8 want = gc[2] >> p & 1 ? 0xff : 0;
        result &= (u8)~(latch[p] ^ want);
    }
    return result;
}

u8 video_read(u32 a) {
    if ((kind == PLANAR || kind == MODEX) && a < 0xb0000) return planar_read(a - 0xa0000);
    return mem[a];
}

void video_write(u32 a, u8 value) {
    if ((kind == PLANAR || kind == MODEX) && a < 0xb0000) {
        planar_write(a - 0xa0000, value);
        claimed = 1;
        return;
    }
    if (mem[a] == value) return;
    mem[a] = value;
    dirty = 1;
    if (a >= TEXT_BASE ? kind != VGA256 : kind == VGA256) claimed = 1;
}

int video_claimed(void) { return claimed; }
int video_cursor_row(void) { return mem_read8(BDA + 0x51 + active_page * 2); }
int video_cursor_col(void) { return mem_read8(BDA + 0x50 + active_page * 2); }
int video_is_graphics(void) { return kind != TEXT; }
int video_mouse_scale_x(void) { return width_px < 640 ? 2 : 1; }
int video_cols(void) { return cols; }

static void set_cursor(int page, int row, int col) {
    mem_write8(BDA + 0x50 + page * 2, (u8)col);
    mem_write8(BDA + 0x51 + page * 2, (u8)row);
}

static u32 page_base(int page) { return TEXT_BASE + (u32)page * (cols == 40 ? 2048u : 4096u); }

/* ---- pixels ------------------------------------------------------------------- */

static void put_pixel(int x, int y, int color) {
    if (x < 0 || y < 0 || x >= width_px || y >= height_px) return;
    dirty = 1;
    switch (kind) {
    case CGA4: {
        u32 at = TEXT_BASE + (u32)(y / 2) * 80 + (y & 1 ? 0x2000 : 0) + (u32)(x / 4);
        int shift = 6 - (x & 3) * 2;
        u8 mask = (u8)(3 << shift), bits = (u8)((color & 3) << shift);
        mem[at] = color & 0x80 ? (u8)(mem[at] ^ bits) : (u8)((mem[at] & ~mask) | bits);
        break;
    }
    case CGA2: {
        u32 at = TEXT_BASE + (u32)(y / 2) * 80 + (y & 1 ? 0x2000 : 0) + (u32)(x / 8);
        u8 bit = (u8)(0x80 >> (x & 7));
        if (color & 0x80) mem[at] ^= (u8)(color & 1 ? bit : 0);
        else mem[at] = (u8)(color & 1 ? mem[at] | bit : mem[at] & ~bit);
        break;
    }
    case VGA256: mem[0xa0000 + (u32)y * 320 + (u32)x] = (u8)color; break;
    case PLANAR: {
        u32 at = (u32)y * (crtc[0x13] * 2u) + (u32)(x / 8);
        u8 bit = (u8)(0x80 >> (x & 7));
        for (int p = 0; p < 4; ++p) {
            if (color & 0x80) { if (color >> p & 1) planes[p][at] ^= bit; }
            else planes[p][at] = (u8)(color >> p & 1 ? planes[p][at] | bit : planes[p][at] & ~bit);
        }
        break;
    }
    case MODEX: planes[x & 3][(u32)y * (crtc[0x13] * 2u) + (u32)(x / 4)] = (u8)color; break;
    }
}

static int get_pixel(int x, int y) {
    if (x < 0 || y < 0 || x >= width_px || y >= height_px) return 0;
    switch (kind) {
    case CGA4: return mem[TEXT_BASE + (u32)(y / 2) * 80 + (y & 1 ? 0x2000 : 0) + (u32)(x / 4)] >> (6 - (x & 3) * 2) & 3;
    case CGA2: return mem[TEXT_BASE + (u32)(y / 2) * 80 + (y & 1 ? 0x2000 : 0) + (u32)(x / 8)] >> (7 - (x & 7)) & 1;
    case VGA256: return mem[0xa0000 + (u32)y * 320 + (u32)x];
    case PLANAR: {
        u32 at = (u32)y * (crtc[0x13] * 2u) + (u32)(x / 8);
        int c = 0;
        for (int p = 0; p < 4; ++p) c |= (planes[p][at] >> (7 - (x & 7)) & 1) << p;
        return c;
    }
    case MODEX: return planes[x & 3][(u32)y * (crtc[0x13] * 2u) + (u32)(x / 4)];
    }
    return 0;
}

static void draw_glyph(int x, int y, int ch, int color, int background) {
    const u8 *glyph = bios_font8 + (u8)ch * 8;
    for (int row = 0; row < 8; ++row)
        for (int col = 0; col < 8; ++col)
            if (glyph[row] & (0x80 >> col)) put_pixel(x + col, y + row, color);
            else if (background >= 0) put_pixel(x + col, y + row, background);
}

/* ---- text output --------------------------------------------------------------- */

static void clear_cells(int page, int r0, int c0, int r1, int c1, u8 attr) {
    u32 base = page_base(page);
    for (int r = r0; r <= r1; ++r)
        for (int c = c0; c <= c1; ++c) {
            mem[base + (u32)r * cols * 2 + (u32)c * 2] = ' ';
            mem[base + (u32)r * cols * 2 + (u32)c * 2 + 1] = attr;
        }
}

static void scroll_cells(int page, int r0, int c0, int r1, int c1, int n, u8 attr, int up) {
    if (kind == TEXT) {
        u32 base = page_base(page);
        int span = (c1 - c0 + 1) * 2;
        if (!n || n > r1 - r0) { clear_cells(page, r0, c0, r1, c1, attr); dirty = 1; return; }
        if (up) {
            for (int r = r0; r <= r1 - n; ++r)
                memmove(mem + base + (u32)r * cols * 2 + c0 * 2, mem + base + (u32)(r + n) * cols * 2 + c0 * 2, (size_t)span);
            clear_cells(page, r1 - n + 1, c0, r1, c1, attr);
        } else {
            for (int r = r1; r >= r0 + n; --r)
                memmove(mem + base + (u32)r * cols * 2 + c0 * 2, mem + base + (u32)(r - n) * cols * 2 + c0 * 2, (size_t)span);
            clear_cells(page, r0, c0, r0 + n - 1, c1, attr);
        }
        dirty = 1;
        return;
    }
    int y0 = r0 * 8, y1 = (r1 + 1) * 8 - 1, x0 = c0 * 8, x1 = (c1 + 1) * 8 - 1;
    int total = y1 - y0 + 1, by = n ? n * 8 : total;
    if (by > total) by = total;
    int bg = attr;
    if (up) {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) put_pixel(x, y, y + by <= y1 ? get_pixel(x, y + by) : bg);
    } else {
        for (int y = y1; y >= y0; --y)
            for (int x = x0; x <= x1; ++x) put_pixel(x, y, y - by >= y0 ? get_pixel(x, y - by) : bg);
    }
}

static void write_at_cursor(int page, int ch, int attr, int use_attr) {
    int r = mem_read8(BDA + 0x51 + page * 2), c = mem_read8(BDA + 0x50 + page * 2);
    if (kind == TEXT) {
        u32 at = page_base(page) + (u32)r * cols * 2 + (u32)c * 2;
        mem[at] = (u8)ch;
        if (use_attr) mem[at + 1] = (u8)attr;
    } else draw_glyph(c * 8, r * 8, ch, attr, attr & 0x80 ? -1 : 0);
    dirty = 1;
}

static void teletype(int ch, int page, int attr) {
    int r = mem_read8(BDA + 0x51 + page * 2), c = mem_read8(BDA + 0x50 + page * 2);
    switch (ch) {
    case 7: return;
    case 8: if (c) --c; break;
    case 13: c = 0; break;
    case 10: ++r; break;
    default:
        write_at_cursor(page, ch, attr, kind != TEXT);
        if (++c >= cols) { c = 0; ++r; }
        break;
    }
    if (r >= rows) {
        u32 at = kind == TEXT ? page_base(page) + (u32)(rows - 1) * cols * 2 + 1 : 0;
        scroll_cells(page, 0, 0, rows - 1, cols - 1, 1, kind == TEXT ? mem[at] : 0, 1);
        r = rows - 1;
    }
    set_cursor(page, r, c);
}

/* The DOS console.  Until the screen is claimed, text goes to the terminal
   (and quietly into the screen memory, in case the program claims it later). */
void video_put_char(int ch) {
    int was_dirty = dirty;
    teletype(ch, active_page, 7);
    if (claimed) return;
    dirty = was_dirty;
    char c = (char)ch;
    host_output(&c, 1);
}

/* ---- modes ---------------------------------------------------------------------- */

void video_set_mode(int number) {
    int no_clear = number & 0x80;
    number &= 0x7f;
    int w, h, c, r, k;
    switch (number) {
    case 0: case 1: k = TEXT; w = 320; h = 200; c = 40; r = 25; break;
    case 2: case 3: case 7: k = TEXT; w = 640; h = 400; c = 80; r = 25; break;
    case 4: case 5: k = CGA4; w = 320; h = 200; c = 40; r = 25; break;
    case 6: k = CGA2; w = 640; h = 200; c = 80; r = 25; break;
    case 0x0d: k = PLANAR; w = 320; h = 200; c = 40; r = 25; break;
    case 0x0e: k = PLANAR; w = 640; h = 200; c = 80; r = 25; break;
    case 0x10: k = PLANAR; w = 640; h = 350; c = 80; r = 25; break;
    case 0x11: case 0x12: k = PLANAR; w = 640; h = 480; c = 80; r = 30; break;
    case 0x13: k = VGA256; w = 320; h = 200; c = 40; r = 25; break;
    default: return;
    }
    mode_number = number;
    kind = k;
    width_px = w;
    height_px = h;
    cols = c;
    rows = r;
    memset(gc, 0, sizeof(gc));
    gc[5] = number == 0x13 ? 0x40 : 0;
    gc[6] = k == TEXT ? 0x0e : k == VGA256 ? 0x05 : 0x05;
    gc[7] = 0x0f;
    gc[8] = 0xff;
    seq[2] = 0x0f;
    seq[4] = number == 0x13 ? 0x0e : 0x06;
    crtc[0x13] = (u8)(w < 640 ? 40 : 80);
    crtc[0x0c] = crtc[0x0d] = 0;
    for (int i = 0; i < 16; ++i) attr_regs[i] = i < 8 ? (u8)i : (u8)(i + 48);
    attr_regs[0x10] = number == 0x13 ? 0x41 : 0x01;
    attr_regs[0x12] = 0x0f;
    default_palette();
    if (!no_clear) {
        if (k == TEXT) {
            for (u32 i = 0; i < 0x8000; i += 2) { mem[TEXT_BASE + i] = ' '; mem[TEXT_BASE + i + 1] = 7; }
        } else {
            memset(mem + 0xa0000, 0, 0x10000);
            memset(mem + TEXT_BASE, 0, 0x8000);
            memset(planes, 0, sizeof(planes));
        }
    }
    active_page = 0;
    for (int p = 0; p < 8; ++p) set_cursor(p, 0, 0);
    mem_write8(BDA + 0x49, (u8)number);
    mem_write16(BDA + 0x4a, (u16)c);
    mem_write16(BDA + 0x4c, c == 80 ? 4096 : 2048);
    mem_write16(BDA + 0x4e, 0);
    mem_write8(BDA + 0x60, 7);
    mem_write8(BDA + 0x61, 6);
    mem_write8(BDA + 0x62, 0);
    mem_write16(BDA + 0x63, 0x3d4);
    mem_write8(BDA + 0x84, (u8)(r - 1));
    mem_write8(BDA + 0x85, k == TEXT ? 16 : 8);
    cursor_visible = 1;
    claimed = 1;
    dirty = 1;
}

void video_init(void) {
    for (int ch = 0; ch < 256; ++ch)
        for (int row = 0; row < 8; ++row) {
            unsigned bits = csilla_rows[ch * CSILLA_HEIGHT + row + 1];
            u8 out = 0;
            for (int col = 0; col < 8; ++col)
                if (bits >> col & 1) out |= (u8)(0x80 >> col);
            bios_font8[ch * 8 + row] = out;
        }
    video_set_mode(3);
    claimed = 0;
    dirty = 0;
}

/* ---- INT 10h -------------------------------------------------------------------- */

static void dac_block(int writing) {
    u32 at = linear(cpu.seg[ES], DX);
    for (int i = 0; i < CX; ++i) {
        int n = (BX + i) & 255;
        if (writing) dac_set(n, mem_read8(at) & 63, mem_read8(at + 1) & 63, mem_read8(at + 2) & 63);
        else { mem_write8(at, dac[n][0]); mem_write8(at + 1, dac[n][1]); mem_write8(at + 2, dac[n][2]); }
        at += 3;
    }
}

static void int10_palette(void) {
    switch (AL) {
    case 0x00: if (BL < 16) { attr_regs[BL] = BH; dirty = 1; } break;
    case 0x01: attr_regs[0x11] = BH; break;
    case 0x02: for (int i = 0; i < 16; ++i) attr_regs[i] = mem_read8(linear(cpu.seg[ES], (u16)(DX + i))); attr_regs[0x11] = mem_read8(linear(cpu.seg[ES], (u16)(DX + 16))); dirty = 1; break;
    case 0x03: break;
    case 0x07: BH = BL < 16 ? attr_regs[BL] : 0; break;
    case 0x09: for (int i = 0; i < 16; ++i) mem_write8(linear(cpu.seg[ES], (u16)(DX + i)), attr_regs[i]); mem_write8(linear(cpu.seg[ES], (u16)(DX + 16)), attr_regs[0x11]); break;
    case 0x10: dac_set(BL, DH & 63, CH & 63, CL & 63); break;
    case 0x12: dac_block(1); break;
    case 0x15: DH = dac[BL][0]; CH = dac[BL][1]; CL = dac[BL][2]; break;
    case 0x17: dac_block(0); break;
    case 0x1a: BH = 0; BL = 0; break;
    }
}

static void int10_font(void) {
    if (AL != 0x30) return;
    cpu_set_seg(ES, 0xf000);
    BP = BH == 3 || BH == 2 || BH == 5 || BH == 6 ? 0xfa6e : BH == 4 ? 0x8000 : mem_read16(0x1f * 4);
    CX = 8;
    DL = (u8)(rows - 1);
}

void video_int10(void) {
    int page = BH < 8 ? BH : 0;
    switch (AH) {
    case 0x00: video_set_mode(AL); AL = 0x20; break;
    case 0x01:
        mem_write8(BDA + 0x60, CL);
        mem_write8(BDA + 0x61, CH);
        cursor_visible = !(CH & 0x20);
        break;
    case 0x02: claimed = 1; if (DH < rows || DL < cols) set_cursor(page, DH, DL); break;
    case 0x03: DH = mem_read8(BDA + 0x51 + page * 2); DL = mem_read8(BDA + 0x50 + page * 2); CH = mem_read8(BDA + 0x61); CL = mem_read8(BDA + 0x60); break;
    case 0x05: if (AL < 8) { active_page = AL; mem_write8(BDA + 0x62, AL); dirty = 1; } break;
    case 0x06: case 0x07: claimed = 1; scroll_cells(active_page, CH, CL, DH < rows ? DH : rows - 1, DL < cols ? DL : cols - 1, AL, BH, AH == 6); break;
    case 0x08: {
        if (kind != TEXT) { AX = 0; break; }
        u32 at = page_base(page) + (u32)mem_read8(BDA + 0x51 + page * 2) * cols * 2 + (u32)mem_read8(BDA + 0x50 + page * 2) * 2;
        AL = mem[at];
        AH = mem[at + 1];
        break;
    }
    case 0x09: case 0x0a: {
        claimed = 1;
        int r = mem_read8(BDA + 0x51 + page * 2), c = mem_read8(BDA + 0x50 + page * 2);
        for (int i = 0; i < CX; ++i) {
            set_cursor(page, r + (c + i) / cols, (c + i) % cols);
            write_at_cursor(page, AL, BL, AH == 9);
        }
        set_cursor(page, r, c);
        break;
    }
    case 0x0b:
        if (BH == 0) cga_select = (u8)((cga_select & 0xe0) | (BL & 0x1f));
        else if (BH == 1) cga_select = (u8)((cga_select & 0xdf) | (BL & 1 ? 0x20 : 0));
        dirty = 1;
        break;
    case 0x0c: claimed = 1; put_pixel(CX, DX, AL); break;
    case 0x0d: AL = (u8)get_pixel(CX, DX); break;
    case 0x0e:
        if (claimed) teletype(AL, active_page, kind == TEXT ? 7 : BL);
        else video_put_char(AL);
        break;
    case 0x0f: AL = (u8)mode_number; AH = (u8)cols; BH = (u8)active_page; break;
    case 0x10: int10_palette(); break;
    case 0x11: int10_font(); break;
    case 0x12:
        if (BL == 0x10) { BH = 0; BL = 3; CX = 9; }   /* the switches of a color monitor */
        break;
    case 0x13: {
        claimed = 1;
        int r = mem_read8(BDA + 0x51 + page * 2), c = mem_read8(BDA + 0x50 + page * 2);
        set_cursor(page, DH, DL);
        u32 at = linear(cpu.seg[ES], BP);
        for (int i = 0; i < CX; ++i) {
            int ch = mem_read8(at++), attr = BL;
            if (AL & 2) attr = mem_read8(at++);
            teletype(ch, page, attr);
        }
        if (!(AL & 1)) set_cursor(page, r, c);
        break;
    }
    case 0x1a: if (AL == 0) { AL = 0x1a; BX = 8; } break;
    case 0x4f: AX = 0x014f; break;
    default: break;
    }
}

/* ---- ports ---------------------------------------------------------------------- */

u32 video_port_in(u16 port) {
    switch (port) {
    case 0x3c0: return attr_index;
    case 0x3c1: return attr_index < 21 ? attr_regs[attr_index] : 0;
    case 0x3c4: return seq_index;
    case 0x3c5: return seq_index < 8 ? seq[seq_index] : 0;
    case 0x3c7: return 0;
    case 0x3c8: return dac_write;
    case 0x3c9: {
        u8 v = dac[dac_read][dac_sub_read];
        if (++dac_sub_read == 3) { dac_sub_read = 0; ++dac_read; }
        return v;
    }
    case 0x3cc: return misc_output;
    case 0x3ce: return gc_index;
    case 0x3cf: return gc_index < 9 ? gc[gc_index] : 0;
    case 0x3d4: case 0x3b4: return crtc_index;
    case 0x3d5: case 0x3b5: return crtc_index < 32 ? crtc[crtc_index] : 0;
    case 0x3d8: return cga_mode;
    case 0x3d9: return cga_select;
    case 0x3da: case 0x3ba: {
        attr_flip = 0;
        u64 at = virtual_ns % 14285714ull;
        return (at > 13000000ull ? 8u : 0) | ((virtual_ns / 31778) & 1 ? 1u : 0);
    }
    }
    return 0xff;
}

void video_port_out(u16 port, u32 value) {
    u8 v = (u8)value;
    switch (port) {
    case 0x3c0:
        if (!attr_flip) attr_index = v & 0x1f;
        else if (attr_index < 21) { attr_regs[attr_index] = v; dirty = 1; }
        attr_flip = !attr_flip;
        break;
    case 0x3c4: seq_index = v & 7; break;
    case 0x3c5:
        seq[seq_index] = v;
        if (seq_index == 4 && !(v & 8) && kind == VGA256) {
            for (u32 i = 0; i < 64000; ++i) planes[i & 3][i >> 2] = mem[0xa0000 + i];
            kind = MODEX;
            dirty = 1;
        } else if (seq_index == 4 && (v & 8) && kind == MODEX) {
            kind = VGA256;
            dirty = 1;
        }
        break;
    case 0x3c7: dac_read = v; dac_sub_read = 0; break;
    case 0x3c8: dac_write = v; dac_sub_write = 0; break;
    case 0x3c9:
        dac[dac_write][dac_sub_write] = v & 63;
        if (++dac_sub_write == 3) { dac_sub_write = 0; dac_changed(dac_write); ++dac_write; }
        break;
    case 0x3c2: misc_output = v; break;
    case 0x3ce: gc_index = v & 15; break;
    case 0x3cf: if (gc_index < 9) gc[gc_index] = v; break;
    case 0x3d4: case 0x3b4: crtc_index = v & 31; break;
    case 0x3d5: case 0x3b5:
        crtc[crtc_index] = v;
        if (crtc_index == 0x0c || crtc_index == 0x0d) dirty = 1;
        if (crtc_index == 0x0e || crtc_index == 0x0f) {
            unsigned at = (unsigned)crtc[0x0e] << 8 | crtc[0x0f];
            set_cursor(active_page, (int)at / cols, (int)at % cols);
        }
        break;
    case 0x3d8: cga_mode = v; dirty = 1; break;
    case 0x3d9: cga_select = v; dirty = 1; break;
    }
}

/* ---- publishing ------------------------------------------------------------------- */

static u32 attr_color(int c) {
    int index = attr_regs[c & 15] & 0x3f;
    return rgb[index];
}

static void render(void) {
    int w = width_px, h = height_px;
    u32 *out = pixels;
    switch (kind) {
    case VGA256:
        for (int i = 0; i < 64000; ++i) out[i] = rgb[mem[0xa0000 + i]];
        break;
    case CGA4: {
        static const u8 sets[3][4] = {{0, 2, 4, 6}, {0, 3, 5, 7}, {0, 3, 4, 7}};
        const u8 *set = sets[mode_number == 5 ? 2 : cga_select & 0x20 ? 1 : 0];
        int bright = cga_select & 0x10 ? 8 : 0;
        for (int y = 0; y < 200; ++y)
            for (int x = 0; x < 320; ++x) {
                int v = mem[TEXT_BASE + (u32)(y / 2) * 80 + (y & 1 ? 0x2000 : 0) + (u32)(x / 4)] >> (6 - (x & 3) * 2) & 3;
                *out++ = v ? ega16[set[v] | bright] : ega16[cga_select & 15];
            }
        break;
    }
    case CGA2: {
        u32 color = ega16[cga_select & 15 ? cga_select & 15 : 15];
        for (int y = 0; y < 200; ++y)
            for (int x = 0; x < 640; ++x) {
                int v = mem[TEXT_BASE + (u32)(y / 2) * 80 + (y & 1 ? 0x2000 : 0) + (u32)(x / 8)] >> (7 - (x & 7)) & 1;
                *out++ = v ? color : 0;
            }
        break;
    }
    case PLANAR: {
        u32 stride = crtc[0x13] * 2u, start = (u32)crtc[0x0c] << 8 | crtc[0x0d];
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                u32 at = (start + (u32)y * stride + (u32)(x / 8)) & 0xffff;
                int shift = 7 - (x & 7), c = 0;
                for (int p = 0; p < 4; ++p) c |= (planes[p][at] >> shift & 1) << p;
                *out++ = attr_color(c);
            }
        break;
    }
    case MODEX: {
        u32 stride = crtc[0x13] * 2u, start = (u32)crtc[0x0c] << 8 | crtc[0x0d];
        for (int y = 0; y < 200; ++y)
            for (int x = 0; x < 320; ++x) {
                u32 at = (start + (u32)y * stride + (u32)(x / 4)) & 0xffff;
                *out++ = rgb[planes[x & 3][at]];
            }
        break;
    }
    }
}

void video_present(void) {
    if (!claimed) return;
    int graphics = kind != TEXT;
    int cursor_on = cursor_visible && !graphics;
    int row = video_cursor_row(), col = video_cursor_col();
    int pw = graphics ? width_px : cols, ph = graphics ? height_px : rows;
    if (!published || graphics != pub_graphics || pw != pub_w || ph != pub_h || row != pub_row || col != pub_col || cursor_on != pub_cursor_on) {
        host_publish(graphics, pw, ph, pixels, mem + page_base(active_page), row, col, cursor_on);
        published = 1;
        pub_graphics = graphics;
        pub_w = pw;
        pub_h = ph;
        pub_row = row;
        pub_col = col;
        pub_cursor_on = cursor_on;
        dirty = 1;
    }
    if (dirty) {
        dirty = 0;
        if (graphics) render();
        host_frame();
    }
}
