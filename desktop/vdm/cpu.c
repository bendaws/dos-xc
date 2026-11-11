/*
 * DOS-XC /desktop/vdm/cpu.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "vdm.h"
#include <string.h>

struct cpu cpu;
u8 *mem;

static int override, op32, ad32, rep_kind, inhibit;
static u32 start_ip;
static u8 parity[256];

#define WRAP(a) ((a) & 0xfffff)
#define IS_VIDEO(a) ((a) >= 0xa0000 && (a) < 0xc0000)

u8 mem_read8(u32 a) {
    a = WRAP(a);
    return IS_VIDEO(a) ? video_read(a) : mem[a];
}

void mem_write8(u32 a, u8 v) {
    a = WRAP(a);
    if (a < 0xa0000) mem[a] = v;
    else if (a < 0xc0000) video_write(a, v);
}

u16 mem_read16(u32 a) {
    a = WRAP(a);
    if (a < 0x9ffff) { u16 v; memcpy(&v, mem + a, 2); return v; }
    return (u16)(mem_read8(a) | mem_read8(a + 1) << 8);
}

void mem_write16(u32 a, u16 v) {
    a = WRAP(a);
    if (a < 0x9ffff) { memcpy(mem + a, &v, 2); return; }
    mem_write8(a, (u8)v);
    mem_write8(a + 1, (u8)(v >> 8));
}

static u32 mem_read32(u32 a) { return mem_read16(a) | (u32)mem_read16(a + 2) << 16; }
static void mem_write32(u32 a, u32 v) { mem_write16(a, (u16)v); mem_write16(a + 2, (u16)(v >> 16)); }

static u32 read_size(u32 a, int s) { return s == 1 ? mem_read8(a) : s == 2 ? mem_read16(a) : mem_read32(a); }
static void write_size(u32 a, int s, u32 v) {
    if (s == 1) mem_write8(a, (u8)v);
    else if (s == 2) mem_write16(a, (u16)v);
    else mem_write32(a, v);
}

static inline u8 fetch8(void) { u8 v = mem[WRAP(cpu.base[CS] + (cpu.ip & 0xffff))]; cpu.ip = (cpu.ip + 1) & 0xffff; return v; }
static inline u16 fetch16(void) { u16 v = fetch8(); return (u16)(v | fetch8() << 8); }
static inline u32 fetch32(void) { u32 v = fetch16(); return v | (u32)fetch16() << 16; }
static inline u32 fetch_size(int s) { return s == 1 ? fetch8() : s == 2 ? fetch16() : fetch32(); }

void cpu_set_seg(int segment, u16 value) { cpu.seg[segment] = value; cpu.base[segment] = (u32)value << 4; }

static inline u32 stack_top(void) { return cpu.base[SS] + SP; }
void cpu_push16(u16 v) { SP = (u16)(SP - 2); mem_write16(stack_top(), v); }
static void push32(u32 v) { SP = (u16)(SP - 4); mem_write32(stack_top(), v); }
static u16 pop16(void) { u16 v = mem_read16(stack_top()); SP = (u16)(SP + 2); return v; }
static u32 pop32(void) { u32 v = mem_read32(stack_top()); SP = (u16)(SP + 4); return v; }
static void push_op(u32 v) { if (op32) push32(v); else cpu_push16((u16)v); }
static u32 pop_op(void) { return op32 ? pop32() : pop16(); }

/* ---- registers and operands --------------------------------------------- */

static inline u32 reg_get(int n, int s) {
    if (s == 1) return n < 4 ? cpu.r[n].b.l : cpu.r[n - 4].b.h;
    return s == 2 ? cpu.r[n].w : cpu.r[n].d;
}

static inline void reg_set(int n, int s, u32 v) {
    if (s == 1) { if (n < 4) cpu.r[n].b.l = (u8)v; else cpu.r[n - 4].b.h = (u8)v; }
    else if (s == 2) cpu.r[n].w = (u16)v;
    else cpu.r[n].d = v;
}

static int mod, regf, rm;
static u32 ea, ea_offset;

static void decode_modrm(void) {
    u8 modrm = fetch8();
    mod = modrm >> 6;
    regf = modrm >> 3 & 7;
    rm = modrm & 7;
    if (mod == 3) return;
    int segment = DS;
    u32 offset = 0;
    if (!ad32) {
        switch (rm) {
        case 0: offset = (u32)BX + SI; break;
        case 1: offset = (u32)BX + DI; break;
        case 2: offset = (u32)BP + SI; segment = SS; break;
        case 3: offset = (u32)BP + DI; segment = SS; break;
        case 4: offset = SI; break;
        case 5: offset = DI; break;
        case 6: if (mod) { offset = BP; segment = SS; } break;
        case 7: offset = BX; break;
        }
        if (!mod && rm == 6) offset = fetch16();
        else if (mod == 1) offset += (u32)(i32)(i8)fetch8();
        else if (mod == 2) offset += fetch16();
        offset &= 0xffff;
    } else {
        if (rm == 4) {
            u8 sib = fetch8();
            int index = sib >> 3 & 7, base = sib & 7;
            if (index != 4) offset = cpu.r[index].d << (sib >> 6);
            if (base == 5 && !mod) offset += fetch32();
            else {
                offset += cpu.r[base].d;
                if (base == ESP || base == EBP) segment = SS;
            }
        } else if (rm == 5 && !mod) offset = fetch32();
        else {
            offset = cpu.r[rm].d;
            if (rm == EBP) segment = SS;
        }
        if (mod == 1) offset += (u32)(i32)(i8)fetch8();
        else if (mod == 2) offset += fetch32();
    }
    ea_offset = offset;
    ea = cpu.base[override >= 0 ? override : segment] + offset;
}

static u32 rm_get(int s) { return mod == 3 ? reg_get(rm, s) : read_size(ea, s); }
static void rm_set(int s, u32 v) { if (mod == 3) reg_set(rm, s, v); else write_size(ea, s, v); }

/* ---- arithmetic --------------------------------------------------------- */

static inline u32 mask_of(int s) { return s == 1 ? 0xff : s == 2 ? 0xffff : 0xffffffffu; }
static inline u32 sign_of(int s) { return s == 1 ? 0x80 : s == 2 ? 0x8000 : 0x80000000u; }

static inline void set_szp(u32 r, int s) {
    r &= mask_of(s);
    cpu.flags &= ~(u32)(F_ZF | F_SF | F_PF);
    if (!r) cpu.flags |= F_ZF;
    if (r & sign_of(s)) cpu.flags |= F_SF;
    if (parity[r & 255]) cpu.flags |= F_PF;
}

static u32 do_add(u32 a, u32 b, u32 carry, int s) {
    u32 m = mask_of(s);
    a &= m; b &= m;
    u64 wide = (u64)a + b + carry;
    u32 r = (u32)wide & m;
    cpu.flags &= ~(u32)(F_CF | F_AF | F_OF);
    if (wide >> (s * 8)) cpu.flags |= F_CF;
    if ((a ^ b ^ r) & 0x10) cpu.flags |= F_AF;
    if (~(a ^ b) & (a ^ r) & sign_of(s)) cpu.flags |= F_OF;
    set_szp(r, s);
    return r;
}

static u32 do_sub(u32 a, u32 b, u32 borrow, int s) {
    u32 m = mask_of(s);
    a &= m; b &= m;
    u32 r = (a - b - borrow) & m;
    cpu.flags &= ~(u32)(F_CF | F_AF | F_OF);
    if ((u64)a < (u64)b + borrow) cpu.flags |= F_CF;
    if ((a ^ b ^ r) & 0x10) cpu.flags |= F_AF;
    if ((a ^ b) & (a ^ r) & sign_of(s)) cpu.flags |= F_OF;
    set_szp(r, s);
    return r;
}

static u32 do_logic(u32 r, int s) {
    cpu.flags &= ~(u32)(F_CF | F_OF | F_AF);
    set_szp(r, s);
    return r & mask_of(s);
}

/* The eight operations of opcodes 00-3F and the 80-83 group. */
static u32 alu(int op, u32 a, u32 b, int s) {
    switch (op) {
    case 0: return do_add(a, b, 0, s);
    case 1: return do_logic(a | b, s);
    case 2: return do_add(a, b, cpu.flags & F_CF, s);
    case 3: return do_sub(a, b, cpu.flags & F_CF, s);
    case 4: return do_logic(a & b, s);
    case 5: return do_sub(a, b, 0, s);
    case 6: return do_logic(a ^ b, s);
    default: do_sub(a, b, 0, s); return a;
    }
}

static u32 do_inc(u32 a, int s) { u32 c = cpu.flags & F_CF; u32 r = do_add(a, 1, 0, s); cpu.flags = (cpu.flags & ~(u32)F_CF) | c; return r; }
static u32 do_dec(u32 a, int s) { u32 c = cpu.flags & F_CF; u32 r = do_sub(a, 1, 0, s); cpu.flags = (cpu.flags & ~(u32)F_CF) | c; return r; }

static u32 do_shift(int op, u32 v, unsigned count, int s) {
    int bits = s * 8;
    u32 m = mask_of(s), top = sign_of(s);
    v &= m;
    count &= 31;
    if (!count) return v;
    u32 c;
    switch (op) {
    case 0:
        count %= bits;
        if (count) v = ((v << count) | (v >> (bits - count))) & m;
        cpu.flags = (cpu.flags & ~(u32)F_CF) | (v & 1 ? F_CF : 0);
        if (((v & top) != 0) != ((v & 1) != 0)) cpu.flags |= F_OF; else cpu.flags &= ~(u32)F_OF;
        return v;
    case 1:
        count %= bits;
        if (count) v = ((v >> count) | (v << (bits - count))) & m;
        cpu.flags = (cpu.flags & ~(u32)F_CF) | (v & top ? F_CF : 0);
        if (((v & top) != 0) != ((v & (top >> 1)) != 0)) cpu.flags |= F_OF; else cpu.flags &= ~(u32)F_OF;
        return v;
    case 2:
        count %= bits + 1;
        for (unsigned i = 0; i < count; ++i) {
            c = cpu.flags & F_CF;
            cpu.flags = (cpu.flags & ~(u32)F_CF) | (v & top ? F_CF : 0);
            v = ((v << 1) | c) & m;
        }
        if (((v & top) != 0) != ((cpu.flags & F_CF) != 0)) cpu.flags |= F_OF; else cpu.flags &= ~(u32)F_OF;
        return v;
    case 3:
        count %= bits + 1;
        for (unsigned i = 0; i < count; ++i) {
            c = cpu.flags & F_CF;
            cpu.flags = (cpu.flags & ~(u32)F_CF) | (v & 1);
            v = (v >> 1) | (c ? top : 0);
        }
        if (((v & top) != 0) != ((v & (top >> 1)) != 0)) cpu.flags |= F_OF; else cpu.flags &= ~(u32)F_OF;
        return v;
    case 4: case 6:
        c = count <= (unsigned)bits ? (v >> (bits - count)) & 1 : 0;
        v = count < 32 ? (v << count) & m : 0;
        cpu.flags &= ~(u32)(F_CF | F_OF | F_AF);
        if (c) cpu.flags |= F_CF;
        if (((v & top) != 0) != (c != 0)) cpu.flags |= F_OF;
        break;
    case 5:
        c = (v >> (count - 1)) & 1;
        cpu.flags &= ~(u32)(F_CF | F_OF | F_AF);
        if (v & top) cpu.flags |= F_OF;
        v >>= count;
        if (c) cpu.flags |= F_CF;
        break;
    default: {
        i32 x = s == 1 ? (i8)v : s == 2 ? (i16)v : (i32)v;
        c = (u32)(x >> (count - 1)) & 1;
        cpu.flags &= ~(u32)(F_CF | F_OF | F_AF);
        v = (u32)(x >> count) & m;
        if (c) cpu.flags |= F_CF;
        break;
    }
    }
    set_szp(v, s);
    return v;
}

static int condition(int cc) {
    u32 f = cpu.flags;
    int r;
    switch (cc >> 1) {
    case 0: r = (f & F_OF) != 0; break;
    case 1: r = (f & F_CF) != 0; break;
    case 2: r = (f & F_ZF) != 0; break;
    case 3: r = (f & (F_CF | F_ZF)) != 0; break;
    case 4: r = (f & F_SF) != 0; break;
    case 5: r = (f & F_PF) != 0; break;
    case 6: r = ((f & F_SF) != 0) != ((f & F_OF) != 0); break;
    default: r = (f & F_ZF) || ((f & F_SF) != 0) != ((f & F_OF) != 0); break;
    }
    return cc & 1 ? !r : r;
}

/* ---- interrupts --------------------------------------------------------- */

void cpu_interrupt(int vector) {
    u32 slot = (u32)vector * 4;
    cpu_push16((u16)cpu.flags);
    cpu_push16(cpu.seg[CS]);
    cpu_push16((u16)cpu.ip);
    cpu.flags &= ~(u32)(F_IF | F_TF);
    cpu_set_seg(CS, mem_read16(slot + 2));
    cpu.ip = mem_read16(slot);
}

static void divide_error(void) { cpu.ip = start_ip; cpu_interrupt(0); }

static void multiply(int s, u32 value, int is_signed) {
    u32 hi_sig;
    if (s == 1) {
        u32 r = is_signed ? (u32)((i32)(i8)AL * (i32)(i8)value) : (u32)AL * (value & 0xff);
        AX = (u16)r;
        hi_sig = is_signed ? ((i8)AL != (i16)AX) : AH != 0;
    } else if (s == 2) {
        u32 r = is_signed ? (u32)((i32)(i16)AX * (i32)(i16)value) : (u32)AX * (value & 0xffff);
        AX = (u16)r;
        DX = (u16)(r >> 16);
        hi_sig = is_signed ? ((i32)r != (i32)(i16)r) : DX != 0;
    } else {
        u64 r = is_signed ? (u64)((i64)(i32)cpu.r[EAX].d * (i64)(i32)value) : (u64)cpu.r[EAX].d * value;
        cpu.r[EAX].d = (u32)r;
        cpu.r[EDX].d = (u32)(r >> 32);
        hi_sig = is_signed ? ((i64)r != (i64)(i32)r) : cpu.r[EDX].d != 0;
    }
    cpu.flags &= ~(u32)(F_CF | F_OF);
    if (hi_sig) cpu.flags |= F_CF | F_OF;
}

static void divide(int s, u32 value, int is_signed) {
    if (s == 1) {
        value &= 0xff;
        if (!value) { divide_error(); return; }
        if (is_signed) {
            i32 a = (i16)AX, b = (i8)value, q = a / b;
            if (q > 127 || q < -128) { divide_error(); return; }
            AL = (u8)q; AH = (u8)(a % b);
        } else {
            u32 a = AX, q = a / value;
            if (q > 255) { divide_error(); return; }
            AL = (u8)q; AH = (u8)(a % value);
        }
    } else if (s == 2) {
        value &= 0xffff;
        if (!value) { divide_error(); return; }
        u32 a = (u32)DX << 16 | AX;
        if (is_signed) {
            i32 b = (i16)value;
            i64 q = (i64)(i32)a / b;
            if (q > 32767 || q < -32768) { divide_error(); return; }
            AX = (u16)q; DX = (u16)((i32)a % b);
        } else {
            u32 q = a / value;
            if (q > 0xffff) { divide_error(); return; }
            AX = (u16)q; DX = (u16)(a % value);
        }
    } else {
        if (!value) { divide_error(); return; }
        u64 a = (u64)cpu.r[EDX].d << 32 | cpu.r[EAX].d;
        if (is_signed) {
            i64 q = (i64)a / (i32)value;
            if (q > 0x7fffffffll || q < -0x80000000ll) { divide_error(); return; }
            cpu.r[EAX].d = (u32)q; cpu.r[EDX].d = (u32)((i64)a % (i32)value);
        } else {
            u64 q = a / value;
            if (q > 0xffffffffull) { divide_error(); return; }
            cpu.r[EAX].d = (u32)q; cpu.r[EDX].d = (u32)(a % value);
        }
    }
}

static inline u32 count_get(void) { return ad32 ? cpu.r[ECX].d : CX; }
static inline void count_set(u32 v) { if (ad32) cpu.r[ECX].d = v; else CX = (u16)v; }
static inline u32 index_get(int r) { return ad32 ? cpu.r[r].d : cpu.r[r].w; }
static inline void index_step(int r, int s) {
    int step = cpu.flags & F_DF ? -s : s;
    if (ad32) cpu.r[r].d += (u32)step; else cpu.r[r].w = (u16)(cpu.r[r].w + step);
}

/* Kinds: 0 MOVS, 1 CMPS, 2 STOS, 3 LODS, 4 SCAS, 5 INS, 6 OUTS. */
static void string_op(int kind, int s) {
    u32 source = cpu.base[override >= 0 ? override : DS];
    u32 dest = cpu.base[ES];
    int compare = kind == 1 || kind == 4;
    u32 n = rep_kind ? count_get() : 1;
    if (rep_kind && !n) return;
    if (rep_kind) cpu.count += n / 4;
    while (n) {
        switch (kind) {
        case 0: write_size(dest + index_get(EDI), s, read_size(source + index_get(ESI), s)); index_step(ESI, s); index_step(EDI, s); break;
        case 1: do_sub(read_size(source + index_get(ESI), s), read_size(dest + index_get(EDI), s), 0, s); index_step(ESI, s); index_step(EDI, s); break;
        case 2: write_size(dest + index_get(EDI), s, reg_get(EAX, s)); index_step(EDI, s); break;
        case 3: reg_set(EAX, s, read_size(source + index_get(ESI), s)); index_step(ESI, s); break;
        case 4: do_sub(reg_get(EAX, s), read_size(dest + index_get(EDI), s), 0, s); index_step(EDI, s); break;
        case 5: write_size(dest + index_get(EDI), s, port_in(DX, s)); index_step(EDI, s); break;
        case 6: port_out(DX, read_size(source + index_get(ESI), s), s); index_step(ESI, s); break;
        }
        if (!rep_kind) return;
        --n;
        count_set(n);
        if (compare && (rep_kind == 2) != ((cpu.flags & F_ZF) != 0)) return;
    }
}

/* ---- BCD ---------------------------------------------------------------- */

static void adjust_add(int subtract) {
    u8 old = AL;
    int carry = (cpu.flags & F_CF) != 0, af = (cpu.flags & F_AF) != 0;
    cpu.flags &= ~(u32)(F_CF | F_AF);
    if ((old & 15) > 9 || af) { AL = (u8)(subtract ? AL - 6 : AL + 6); cpu.flags |= F_AF; }
    if (old > 0x99 || carry) { AL = (u8)(subtract ? AL - 0x60 : AL + 0x60); cpu.flags |= F_CF; }
    set_szp(AL, 1);
}

static void adjust_ascii(int subtract) {
    if ((AL & 15) > 9 || (cpu.flags & F_AF)) {
        AX = (u16)(subtract ? AX - 6 : AX + 6);
        AH = (u8)(subtract ? AH - 1 : AH + 1);
        cpu.flags |= F_AF | F_CF;
    } else cpu.flags &= ~(u32)(F_AF | F_CF);
    AL &= 15;
}

/* ---- far transfers and the two-byte opcodes ------------------------------ */

static void jump_near(i32 distance) { cpu.ip = (cpu.ip + (u32)distance) & (op32 ? 0xffffffffu : 0xffff); }

static void two_byte(int s) {
    u8 op = fetch8();
    if (op >= 0x80 && op <= 0x8f) {
        i32 d = op32 ? (i32)fetch32() : (i16)fetch16();
        if (condition(op & 15)) jump_near(d);
        return;
    }
    if (op >= 0x90 && op <= 0x9f) { decode_modrm(); rm_set(1, condition(op & 15)); return; }
    if (op >= 0xc8 && op <= 0xcf) {
        u32 v = cpu.r[op & 7].d;
        cpu.r[op & 7].d = v >> 24 | (v >> 8 & 0xff00) | (v << 8 & 0xff0000) | v << 24;
        return;
    }
    switch (op) {
    case 0x00: case 0x02: case 0x03: decode_modrm(); break;     /* protected mode only */
    case 0x01: decode_modrm(); if (regf == 4) rm_set(2, 0); break;    /* SMSW: real mode */
    case 0x06: case 0x08: case 0x09: case 0x0b: break;
    case 0x20: case 0x21: case 0x22: case 0x23: decode_modrm(); if (op == 0x20) reg_set(rm, 4, 0); break;
    case 0xa0: push_op(cpu.seg[FS]); break;
    case 0xa1: cpu_set_seg(FS, (u16)pop_op()); break;
    case 0xa8: push_op(cpu.seg[GS]); break;
    case 0xa9: cpu_set_seg(GS, (u16)pop_op()); break;
    case 0xa2: cpu.r[EAX].d = 0; cpu.r[EBX].d = 0; cpu.r[ECX].d = 0; cpu.r[EDX].d = 0; break;
    case 0xa3: case 0xab: case 0xb3: case 0xbb: case 0xba: {
        decode_modrm();
        int action;
        u32 bit;
        if (op == 0xba) { action = regf - 4; bit = fetch8(); }
        else {
            action = op == 0xa3 ? 0 : op == 0xab ? 1 : op == 0xb3 ? 2 : 3;
            bit = reg_get(regf, s);
            if (mod != 3) {
                i32 sbit = s == 2 ? (i16)bit : (i32)bit;
                ea += (u32)((sbit >> (s == 2 ? 4 : 5)) * s);
            }
        }
        bit &= s * 8 - 1;
        u32 v = rm_get(s);
        cpu.flags = (cpu.flags & ~(u32)F_CF) | (v >> bit & 1);
        if (action == 1) rm_set(s, v | 1u << bit);
        else if (action == 2) rm_set(s, v & ~(1u << bit));
        else if (action == 3) rm_set(s, v ^ 1u << bit);
        break;
    }
    case 0xa4: case 0xa5: case 0xac: case 0xad: {
        decode_modrm();
        unsigned count = (op & 1 ? CL : fetch8()) & 31;
        if (!count) break;
        u32 v = rm_get(s), other = reg_get(regf, s);
        int bits = s * 8;
        u32 r, c;
        if (op < 0xa8) {
            c = (v >> (bits - count)) & 1;
            r = (v << count) | (other >> (bits - count));
        } else {
            c = (v >> (count - 1)) & 1;
            r = (v >> count) | (other << (bits - count));
        }
        rm_set(s, r);
        cpu.flags = (cpu.flags & ~(u32)F_CF) | c;
        set_szp(r, s);
        break;
    }
    case 0xaf: {
        decode_modrm();
        u32 a = reg_get(regf, s), b = rm_get(s);
        if (s == 2) {
            i32 r = (i32)(i16)a * (i32)(i16)b;
            reg_set(regf, 2, (u32)r);
            cpu.flags = (cpu.flags & ~(u32)(F_CF | F_OF)) | (r != (i16)r ? F_CF | F_OF : 0);
        } else {
            i64 r = (i64)(i32)a * (i32)b;
            reg_set(regf, 4, (u32)r);
            cpu.flags = (cpu.flags & ~(u32)(F_CF | F_OF)) | (r != (i32)r ? F_CF | F_OF : 0);
        }
        break;
    }
    case 0xb0: case 0xb1: {
        int size = op == 0xb0 ? 1 : s;
        decode_modrm();
        u32 d = rm_get(size), a = reg_get(EAX, size);
        do_sub(a, d, 0, size);
        if ((a & mask_of(size)) == (d & mask_of(size))) rm_set(size, reg_get(regf, size));
        else reg_set(EAX, size, d);
        break;
    }
    case 0xb2: case 0xb4: case 0xb5: {
        decode_modrm();
        u32 offset = read_size(ea, s);
        reg_set(regf, s, offset);
        cpu_set_seg(op == 0xb2 ? SS : op == 0xb4 ? FS : GS, mem_read16(ea + (u32)s));
        break;
    }
    case 0xb6: decode_modrm(); reg_set(regf, s, rm_get(1) & 0xff); break;
    case 0xb7: decode_modrm(); reg_set(regf, s, rm_get(2) & 0xffff); break;
    case 0xbe: decode_modrm(); reg_set(regf, s, (u32)(i32)(i8)rm_get(1)); break;
    case 0xbf: decode_modrm(); reg_set(regf, s, (u32)(i32)(i16)rm_get(2)); break;
    case 0xbc: case 0xbd: {
        decode_modrm();
        u32 v = rm_get(s) & mask_of(s);
        if (!v) { cpu.flags |= F_ZF; break; }
        cpu.flags &= ~(u32)F_ZF;
        int bit = op == 0xbc ? 0 : s * 8 - 1;
        while (!(v >> bit & 1)) bit += op == 0xbc ? 1 : -1;
        reg_set(regf, s, (u32)bit);
        break;
    }
    case 0xc0: case 0xc1: {
        int size = op == 0xc0 ? 1 : s;
        decode_modrm();
        u32 d = rm_get(size), source = reg_get(regf, size);
        reg_set(regf, size, d);
        rm_set(size, do_add(d, source, 0, size));
        break;
    }
    default: cpu.ip = start_ip; cpu_interrupt(6); break;
    }
}

static void step(void) {
    start_ip = cpu.ip;
    override = -1;
    op32 = ad32 = rep_kind = 0;
    u8 op;
    for (;;) {
        op = fetch8();
        switch (op) {
        case 0x26: override = ES; continue;
        case 0x2e: override = CS; continue;
        case 0x36: override = SS; continue;
        case 0x3e: override = DS; continue;
        case 0x64: override = FS; continue;
        case 0x65: override = GS; continue;
        case 0x66: op32 = 1; continue;
        case 0x67: ad32 = 1; continue;
        case 0xf0: continue;
        case 0xf2: rep_kind = 1; continue;
        case 0xf3: rep_kind = 2; continue;
        }
        break;
    }
    int s = op32 ? 4 : 2;

    if (op < 0x40 && (op & 7) < 6) {
        int action = op >> 3, form = op & 7;
        int size = form & 1 ? s : 1;
        if (form < 4) {
            decode_modrm();
            if (form < 2) {
                u32 r = alu(action, rm_get(size), reg_get(regf, size), size);
                if (action != 7) rm_set(size, r);
            } else {
                u32 r = alu(action, reg_get(regf, size), rm_get(size), size);
                if (action != 7) reg_set(regf, size, r);
            }
        } else {
            u32 r = alu(action, reg_get(EAX, size), fetch_size(size), size);
            if (action != 7) reg_set(EAX, size, r);
        }
        return;
    }
    if (op >= 0x40 && op <= 0x4f) {
        int n = op & 7;
        reg_set(n, s, op < 0x48 ? do_inc(reg_get(n, s), s) : do_dec(reg_get(n, s), s));
        return;
    }
    if (op >= 0x50 && op <= 0x57) { push_op(reg_get(op & 7, s)); return; }
    if (op >= 0x58 && op <= 0x5f) { reg_set(op & 7, s, pop_op()); return; }
    if (op >= 0x70 && op <= 0x7f) {
        i32 d = (i8)fetch8();
        if (condition(op & 15)) jump_near(d);
        return;
    }
    if (op >= 0x91 && op <= 0x97) {
        u32 t = reg_get(EAX, s);
        reg_set(EAX, s, reg_get(op & 7, s));
        reg_set(op & 7, s, t);
        return;
    }
    if (op >= 0xb0 && op <= 0xb7) { reg_set(op & 7, 1, fetch8()); return; }
    if (op >= 0xb8 && op <= 0xbf) { reg_set(op & 7, s, fetch_size(s)); return; }
    if (op >= 0xd8 && op <= 0xdf) { decode_modrm(); return; }  /* no coprocessor */

    switch (op) {
    case 0x06: push_op(cpu.seg[ES]); break;
    case 0x07: cpu_set_seg(ES, (u16)pop_op()); break;
    case 0x0e: push_op(cpu.seg[CS]); break;
    case 0x0f: two_byte(s); break;
    case 0x16: push_op(cpu.seg[SS]); break;
    case 0x17: cpu_set_seg(SS, (u16)pop_op()); inhibit = 1; break;
    case 0x1e: push_op(cpu.seg[DS]); break;
    case 0x1f: cpu_set_seg(DS, (u16)pop_op()); break;
    case 0x27: adjust_add(0); break;
    case 0x2f: adjust_add(1); break;
    case 0x37: adjust_ascii(0); break;
    case 0x3f: adjust_ascii(1); break;
    case 0x60: {
        u32 sp = reg_get(ESP, s);
        for (int i = 0; i < 8; ++i) push_op(i == ESP ? sp : reg_get(i, s));
        break;
    }
    case 0x61:
        for (int i = 7; i >= 0; --i) {
            u32 v = pop_op();
            if (i != ESP) reg_set(i, s, v);
        }
        break;
    case 0x62: decode_modrm(); break;                  /* BOUND: never out of range */
    case 0x68: push_op(fetch_size(s)); break;
    case 0x6a: push_op((u32)(i32)(i8)fetch8()); break;
    case 0x69: case 0x6b: {
        decode_modrm();
        i32 imm = op == 0x6b ? (i8)fetch8() : s == 2 ? (i16)fetch16() : (i32)fetch32();
        u32 v = rm_get(s);
        if (s == 2) {
            i32 r = (i32)(i16)v * imm;
            reg_set(regf, 2, (u32)r);
            cpu.flags = (cpu.flags & ~(u32)(F_CF | F_OF)) | (r != (i16)r ? F_CF | F_OF : 0);
        } else {
            i64 r = (i64)(i32)v * imm;
            reg_set(regf, 4, (u32)r);
            cpu.flags = (cpu.flags & ~(u32)(F_CF | F_OF)) | (r != (i32)r ? F_CF | F_OF : 0);
        }
        break;
    }
    case 0x6c: string_op(5, 1); break;
    case 0x6d: string_op(5, s); break;
    case 0x6e: string_op(6, 1); break;
    case 0x6f: string_op(6, s); break;
    case 0x80: case 0x82: {
        decode_modrm();
        u32 imm = fetch8(), r = alu(regf, rm_get(1), imm, 1);
        if (regf != 7) rm_set(1, r);
        break;
    }
    case 0x81: {
        decode_modrm();
        u32 imm = fetch_size(s), r = alu(regf, rm_get(s), imm, s);
        if (regf != 7) rm_set(s, r);
        break;
    }
    case 0x83: {
        decode_modrm();
        u32 imm = (u32)(i32)(i8)fetch8(), r = alu(regf, rm_get(s), imm, s);
        if (regf != 7) rm_set(s, r);
        break;
    }
    case 0x84: decode_modrm(); do_logic(rm_get(1) & reg_get(regf, 1), 1); break;
    case 0x85: decode_modrm(); do_logic(rm_get(s) & reg_get(regf, s), s); break;
    case 0x86: case 0x87: {
        int size = op & 1 ? s : 1;
        decode_modrm();
        u32 a = rm_get(size);
        rm_set(size, reg_get(regf, size));
        reg_set(regf, size, a);
        break;
    }
    case 0x88: decode_modrm(); rm_set(1, reg_get(regf, 1)); break;
    case 0x89: decode_modrm(); rm_set(s, reg_get(regf, s)); break;
    case 0x8a: decode_modrm(); reg_set(regf, 1, rm_get(1)); break;
    case 0x8b: decode_modrm(); reg_set(regf, s, rm_get(s)); break;
    case 0x8c: decode_modrm(); rm_set(mod == 3 ? s : 2, cpu.seg[regf & 7]); break;
    case 0x8e: {
        decode_modrm();
        cpu_set_seg(regf & 7, (u16)rm_get(2));
        if ((regf & 7) == SS) inhibit = 1;
        break;
    }
    case 0x8d: decode_modrm(); reg_set(regf, s, ea_offset); break;
    case 0x8f: {
        u32 v = pop_op();
        decode_modrm();
        rm_set(s, v);
        break;
    }
    case 0x90: break;
    case 0x98: if (op32) cpu.r[EAX].d = (u32)(i32)(i16)AX; else AX = (u16)(i16)(i8)AL; break;
    case 0x99: if (op32) cpu.r[EDX].d = cpu.r[EAX].d & 0x80000000u ? 0xffffffffu : 0; else DX = AX & 0x8000 ? 0xffff : 0; break;
    case 0x9a: {
        u32 target = fetch_size(s);
        u16 segment = fetch16();
        push_op(cpu.seg[CS]);
        push_op(cpu.ip);
        cpu_set_seg(CS, segment);
        cpu.ip = target;
        break;
    }
    case 0x9b: break;
    case 0x9c: push_op((cpu.flags & 0x7fd7) | 2); break;
    case 0x9d: {
        u32 v = pop_op();
        u32 keep = cpu.flags & ~(u32)0x7fd5;
        cpu.flags = keep | (v & 0x7fd5) | 2;
        break;
    }
    case 0x9e: cpu.flags = (cpu.flags & ~(u32)0xd5) | (AH & 0xd5) | 2; break;
    case 0x9f: AH = (u8)((cpu.flags & 0xd5) | 2); break;
    case 0xa0: AL = mem_read8(cpu.base[override >= 0 ? override : DS] + (ad32 ? fetch32() : fetch16())); break;
    case 0xa1: { u32 a = cpu.base[override >= 0 ? override : DS] + (ad32 ? fetch32() : fetch16()); reg_set(EAX, s, read_size(a, s)); break; }
    case 0xa2: { u32 a = cpu.base[override >= 0 ? override : DS] + (ad32 ? fetch32() : fetch16()); mem_write8(a, AL); break; }
    case 0xa3: { u32 a = cpu.base[override >= 0 ? override : DS] + (ad32 ? fetch32() : fetch16()); write_size(a, s, reg_get(EAX, s)); break; }
    case 0xa4: string_op(0, 1); break;
    case 0xa5: string_op(0, s); break;
    case 0xa6: string_op(1, 1); break;
    case 0xa7: string_op(1, s); break;
    case 0xa8: do_logic(AL & fetch8(), 1); break;
    case 0xa9: do_logic(reg_get(EAX, s) & fetch_size(s), s); break;
    case 0xaa: string_op(2, 1); break;
    case 0xab: string_op(2, s); break;
    case 0xac: string_op(3, 1); break;
    case 0xad: string_op(3, s); break;
    case 0xae: string_op(4, 1); break;
    case 0xaf: string_op(4, s); break;
    case 0xc0: case 0xc1: {
        int size = op == 0xc0 ? 1 : s;
        decode_modrm();
        unsigned n = fetch8();
        rm_set(size, do_shift(regf, rm_get(size), n, size));
        break;
    }
    case 0xd0: case 0xd1: { int size = op == 0xd0 ? 1 : s; decode_modrm(); rm_set(size, do_shift(regf, rm_get(size), 1, size)); break; }
    case 0xd2: case 0xd3: { int size = op == 0xd2 ? 1 : s; decode_modrm(); rm_set(size, do_shift(regf, rm_get(size), CL, size)); break; }
    case 0xc2: { u16 n = fetch16(); cpu.ip = pop_op(); SP = (u16)(SP + n); break; }
    case 0xc3: cpu.ip = pop_op(); break;
    case 0xc4: case 0xc5: {
        decode_modrm();
        u32 offset = read_size(ea, s);
        reg_set(regf, s, offset);
        cpu_set_seg(op == 0xc4 ? ES : DS, mem_read16(ea + (u32)s));
        break;
    }
    case 0xc6: decode_modrm(); rm_set(1, fetch8()); break;
    case 0xc7: decode_modrm(); rm_set(s, fetch_size(s)); break;
    case 0xc8: {
        u16 size = fetch16();
        u8 level = fetch8() & 31;
        push_op(reg_get(EBP, s));
        u32 frame = reg_get(ESP, s);
        for (int i = 1; i < level; ++i) {
            BP = (u16)(BP - s);
            push_op(read_size(cpu.base[SS] + BP, s));
        }
        if (level) push_op(frame);
        reg_set(EBP, s, frame);
        SP = (u16)(SP - size);
        break;
    }
    case 0xc9: SP = BP; reg_set(EBP, s, pop_op()); break;
    case 0xca: { u16 n = fetch16(); cpu.ip = pop_op(); cpu_set_seg(CS, (u16)pop_op()); SP = (u16)(SP + n); break; }
    case 0xcb: cpu.ip = pop_op(); cpu_set_seg(CS, (u16)pop_op()); break;
    case 0xcc: cpu_interrupt(3); break;
    case 0xcd: cpu_interrupt(fetch8()); break;
    case 0xce: if (cpu.flags & F_OF) cpu_interrupt(4); break;
    case 0xcf: {
        cpu.ip = pop_op();
        cpu_set_seg(CS, (u16)pop_op());
        u32 v = pop_op();
        cpu.flags = (cpu.flags & ~(u32)0x7fd5) | (v & 0x7fd5) | 2;
        break;
    }
    case 0xd4: {
        u8 base = fetch8();
        if (!base) { divide_error(); break; }
        AH = (u8)(AL / base);
        AL = (u8)(AL % base);
        set_szp(AL, 1);
        break;
    }
    case 0xd5: { u8 base = fetch8(); AL = (u8)(AL + AH * base); AH = 0; set_szp(AL, 1); break; }
    case 0xd6: AL = cpu.flags & F_CF ? 0xff : 0; break;
    case 0xd7: AL = mem_read8(cpu.base[override >= 0 ? override : DS] + (u16)(BX + AL)); break;
    case 0xe0: case 0xe1: case 0xe2: {
        i32 d = (i8)fetch8();
        u32 c = count_get() - 1;
        count_set(c);
        int zf = (cpu.flags & F_ZF) != 0;
        if ((ad32 ? c : (u16)c) && (op == 0xe2 || (op == 0xe1 ? zf : !zf))) jump_near(d);
        break;
    }
    case 0xe3: { i32 d = (i8)fetch8(); if (!count_get()) jump_near(d); break; }
    case 0xe4: AL = (u8)port_in(fetch8(), 1); break;
    case 0xe5: reg_set(EAX, s, port_in(fetch8(), s)); break;
    case 0xe6: port_out(fetch8(), AL, 1); break;
    case 0xe7: port_out(fetch8(), reg_get(EAX, s), s); break;
    case 0xec: AL = (u8)port_in(DX, 1); break;
    case 0xed: reg_set(EAX, s, port_in(DX, s)); break;
    case 0xee: port_out(DX, AL, 1); break;
    case 0xef: port_out(DX, reg_get(EAX, s), s); break;
    case 0xe8: {
        i32 d = op32 ? (i32)fetch32() : (i16)fetch16();
        push_op(cpu.ip);
        jump_near(d);
        break;
    }
    case 0xe9: { i32 d = op32 ? (i32)fetch32() : (i16)fetch16(); jump_near(d); break; }
    case 0xea: { u32 target = fetch_size(s); u16 segment = fetch16(); cpu_set_seg(CS, segment); cpu.ip = target; break; }
    case 0xeb: { i32 d = (i8)fetch8(); jump_near(d); break; }
    case 0xf1: bios_trap(fetch8()); break;              /* our BIOS stubs */
    case 0xf4: cpu.halted = 1; break;
    case 0xf5: cpu.flags ^= F_CF; break;
    case 0xf6: case 0xf7: {
        int size = op & 1 ? s : 1;
        decode_modrm();
        switch (regf) {
        case 0: case 1: do_logic(rm_get(size) & fetch_size(size), size); break;
        case 2: rm_set(size, ~rm_get(size)); break;
        case 3: {
            u32 v = rm_get(size);
            u32 r = do_sub(0, v, 0, size);
            rm_set(size, r);
            break;
        }
        case 4: multiply(size, rm_get(size), 0); break;
        case 5: multiply(size, rm_get(size), 1); break;
        case 6: divide(size, rm_get(size), 0); break;
        case 7: divide(size, rm_get(size), 1); break;
        }
        break;
    }
    case 0xf8: cpu.flags &= ~(u32)F_CF; break;
    case 0xf9: cpu.flags |= F_CF; break;
    case 0xfa: cpu.flags &= ~(u32)F_IF; break;
    case 0xfb: cpu.flags |= F_IF; inhibit = 1; break;
    case 0xfc: cpu.flags &= ~(u32)F_DF; break;
    case 0xfd: cpu.flags |= F_DF; break;
    case 0xfe: {
        decode_modrm();
        u32 v = rm_get(1);
        if (regf == 0) rm_set(1, do_inc(v, 1));
        else if (regf == 1) rm_set(1, do_dec(v, 1));
        break;
    }
    case 0xff: {
        decode_modrm();
        switch (regf) {
        case 0: rm_set(s, do_inc(rm_get(s), s)); break;
        case 1: rm_set(s, do_dec(rm_get(s), s)); break;
        case 2: { u32 target = rm_get(s); push_op(cpu.ip); cpu.ip = target & (op32 ? 0xffffffffu : 0xffff); break; }
        case 3: {
            u32 target = read_size(ea, s);
            u16 segment = mem_read16(ea + (u32)s);
            push_op(cpu.seg[CS]);
            push_op(cpu.ip);
            cpu_set_seg(CS, segment);
            cpu.ip = target;
            break;
        }
        case 4: cpu.ip = rm_get(s) & (op32 ? 0xffffffffu : 0xffff); break;
        case 5: {
            u32 target = read_size(ea, s);
            cpu_set_seg(CS, mem_read16(ea + (u32)s));
            cpu.ip = target;
            break;
        }
        case 6: push_op(rm_get(s)); break;
        default: cpu.ip = start_ip; cpu_interrupt(6); break;
        }
        break;
    }
    default: cpu.ip = start_ip; cpu_interrupt(6); break;
    }
}

void cpu_reset(void) {
    memset(&cpu, 0, sizeof(cpu));
    cpu.flags = 2 | F_IF;
    for (int i = 0; i < 256; ++i) {
        int bits = 0;
        for (int b = i; b; b >>= 1) bits += b & 1;
        parity[i] = !(bits & 1);
    }
}

void cpu_run(int instructions) {
    cpu.idle = 0;
    while (instructions-- > 0 && !cpu.idle && !vdm_finished) {
        if (inhibit) inhibit = 0;
        else if (bios_interrupt_ready()) {
            cpu.halted = 0;
            bios_deliver();
        }
        if (cpu.halted) { cpu.idle = 1; break; }
        step();
        ++cpu.count;
    }
}
