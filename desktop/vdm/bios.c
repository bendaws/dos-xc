/*
 * DOS-XC /desktop/vdm/bios.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "vdm.h"
#include <string.h>

u64 virtual_ns;
int vdm_exit_code, vdm_finished;

#define ROM_SEGMENT 0xf000
#define STUB_BASE 0x0100
#define PIT_HZ 1193182u
#define BDA 0x400
#define KEYS_START 0x1e
#define KEYS_END 0x3e

static volatile u32 irq_request;
static u32 irq_service, irq_mask;
static u32 pit_reload[3] = {65536, 18, 0x533};
static u8 pit_access[3] = {3, 3, 3}, pit_flip[3], pit_latched[3];
static u16 pit_latch[3];
static u64 pit_started[3], next_timer;
static u32 speaker_divisor = 0x533;
static u8 port61;
static int speaker_on;

static u64 pit_period_ns(int ch) { return (u64)pit_reload[ch] * 1000000000ull / PIT_HZ; }

static u16 pit_current(int ch) {
    u64 ticks = (virtual_ns - pit_started[ch]) * PIT_HZ / 1000000000ull;
    return (u16)(pit_reload[ch] - ticks % pit_reload[ch]);
}

static void speaker_update(void) {
    int on = (port61 & 3) == 3;
    if (on != speaker_on || on) host_speaker(on ? speaker_divisor : 0);
    speaker_on = on;
}

int bios_interrupt_ready(void) {
    if (!irq_request || !(cpu.flags & F_IF)) return 0;
    u32 wanted = irq_request & ~irq_mask;
    if (!wanted) return 0;
    if (irq_service && (wanted & (0u - wanted)) >= (irq_service & (0u - irq_service))) return 0;
    return 1;
}

void bios_deliver(void) {
    u32 wanted = irq_request & ~irq_mask;
    int line = 0;
    while (!(wanted >> line & 1)) ++line;
    irq_request &= ~(1u << line);
    irq_service |= 1u << line;
    cpu_interrupt(8 + line);
}

static u8 scan_latch;
static int scan_ready;
static u16 scan_word;
static struct { u8 scan; u16 word; } scan_queue[64];
static int scan_head, scan_tail;
static u64 release_at;
static u8 release_scan;

static void scan_push(u8 scan, u16 word) {
    int next = (scan_tail + 1) % 64;
    if (next == scan_head) return;
    scan_queue[scan_tail].scan = scan;
    scan_queue[scan_tail].word = word;
    scan_tail = next;
}

static void buffer_key(u16 word) {
    u16 head = mem_read16(BDA + 0x1a), tail = mem_read16(BDA + 0x1c);
    u16 next = (u16)(tail + 2);
    if (next >= KEYS_END) next = KEYS_START;
    if (next == head) return;
    mem_write16(BDA + tail, word);
    mem_write16(BDA + 0x1c, next);
}

int bios_key_available(void) { return mem_read16(BDA + 0x1a) != mem_read16(BDA + 0x1c); }

u16 bios_key_read(void) {
    u16 head = mem_read16(BDA + 0x1a);
    u16 word = mem_read16(BDA + head);
    head = (u16)(head + 2);
    if (head >= KEYS_END) head = KEYS_START;
    mem_write16(BDA + 0x1a, head);
    return word;
}

static const char scan_lower[] = "\0\0331234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./";
static const char scan_upper[] = "\0\033!@#$%^&*()_+\b\tQWERTYUIOP{}\r\0ASDFGHJKL:\"~\0|ZXCVBNM<>?";

static void key_arrived(int key) {
    static const u8 special[] = {0x48, 0x50, 0x4d, 0x4b, 0x47, 0x4f, 0x52, 0x53, 0x49, 0x51,
                                 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58};
    u8 scan = 0;
    u16 word = 0;
    if (key >= 0x101 && key <= 0x116) {
        scan = special[key - 0x101];
        word = (u16)((key >= 0x115 ? scan + 0x2e : scan) << 8);
    } else {
        if (key == '\n') key = '\r';
        if (key == 127) key = '\b';
        if (key > 0 && key < 27 && key != '\b' && key != '\t' && key != '\r') {
            scan = (u8)(strchr(scan_lower + 2, key + 'a' - 1) - scan_lower);
        } else if (key == ' ') scan = 0x39;
        else {
            for (int i = 1; i < (int)sizeof(scan_lower) - 1 && !scan; ++i)
                if (scan_lower[i] == key || scan_upper[i] == key) scan = (u8)i;
        }
        word = (u16)(scan << 8 | (key & 0xff));
    }
    scan_push(scan, word);
    scan_push((u8)(scan | 0x80), 0);
}

static void keyboard_pump(void) {
    int key;
    while ((key = host_key()) >= 0) key_arrived(key);
    if (irq_request & 2 || irq_service & 2 || scan_head == scan_tail) return;
    if (scan_queue[scan_head].scan & 0x80) {        /* hold each key down for a moment */
        if (!release_at) { release_at = virtual_ns + 50000000ull; release_scan = scan_queue[scan_head].scan; }
        if (virtual_ns < release_at) return;
        release_at = 0;
    }
    scan_latch = scan_queue[scan_head].scan;
    scan_word = scan_queue[scan_head].word;
    scan_head = (scan_head + 1) % 64;
    scan_ready = 1;
    irq_request |= 2;
}

static struct { int visible, min_x, max_x, min_y, max_y; int wheel_seen; } mouse = {0, 0, 639, 0, 199, 0};

static void mouse_position(int *x, int *y, int *buttons) {
    int px, py;
    host_mouse(&px, &py, buttons);
    int scale = video_mouse_scale_x();
    *x = px * scale;
    *y = video_is_graphics() ? py : py;
    if (!video_is_graphics()) { *x = px * 640 / video_cols(); *y = py * 8; }
    if (*x < mouse.min_x) *x = mouse.min_x;
    if (*x > mouse.max_x) *x = mouse.max_x;
    if (*y < mouse.min_y) *y = mouse.min_y;
    if (*y > mouse.max_y) *y = mouse.max_y;
}

static void int33(void) {
    int x, y, buttons;
    switch (AX) {
    case 0x00: AX = 0xffff; BX = 2; mouse.visible = 0; mouse.min_x = 0; mouse.max_x = 639; mouse.min_y = 0; mouse.max_y = 199; break;
    case 0x01: mouse.visible = 1; break;
    case 0x02: mouse.visible = 0; break;
    case 0x03: mouse_position(&x, &y, &buttons); BX = (u16)buttons; CX = (u16)x; DX = (u16)y; break;
    case 0x04: break;
    case 0x05: case 0x06: mouse_position(&x, &y, &buttons); AX = (u16)buttons; BX = 0; CX = (u16)x; DX = (u16)y; break;
    case 0x07: mouse.min_x = (i16)CX; mouse.max_x = (i16)DX; break;
    case 0x08: mouse.min_y = (i16)CX; mouse.max_y = (i16)DX; break;
    case 0x0b: CX = 0; DX = 0; break;
    case 0x0f: case 0x1a: break;
    case 0x21: AX = 0xffff; BX = 2; break;
    case 0x24: BX = 0x0626; CX = 0x0400; break;
    }
}

/* ---- time ----------------------------------------------------------------- */

static u8 bcd(int value) { return (u8)(value / 10 * 16 + value % 10); }

static void civil(i64 days, int *year, int *month, int *day) {
    days += 719468;
    i64 era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    i64 y = (i64)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *day = (int)(doy - (153 * mp + 2) / 5 + 1);
    *month = (int)(mp < 10 ? mp + 3 : mp - 9);
    *year = (int)(y + (*month <= 2));
}

static void int1a(void) {
    i64 now = host_time();
    i64 days = now / 86400;
    int seconds = (int)(now % 86400);
    int year, month, day;
    civil(days, &year, &month, &day);
    switch (AH) {
    case 0x00: {
        u32 ticks = mem_read16(BDA + 0x6c) | (u32)mem_read16(BDA + 0x6e) << 16;
        CX = (u16)(ticks >> 16);
        DX = (u16)ticks;
        AL = mem_read8(BDA + 0x70);
        mem_write8(BDA + 0x70, 0);
        break;
    }
    case 0x01: mem_write16(BDA + 0x6c, DX); mem_write16(BDA + 0x6e, CX); break;
    case 0x02: CH = bcd(seconds / 3600); CL = bcd(seconds / 60 % 60); DH = bcd(seconds % 60); DL = 0; bios_set_return_flag(F_CF, 0); break;
    case 0x04: CH = bcd(year / 100); CL = bcd(year % 100); DH = bcd(month); DL = bcd(day); bios_set_return_flag(F_CF, 0); break;
    default: break;
    }
}

/* ---- BIOS services -------------------------------------------------------- */

void bios_set_return_flag(u32 flag, int on) {
    u32 at = cpu.base[SS] + SP + 4;
    u16 flags = mem_read16(at);
    mem_write16(at, on ? (u16)(flags | flag) : (u16)(flags & ~flag));
}

void bios_wait(void) {
    cpu.ip -= 2;
    cpu.flags |= F_IF;
    cpu.idle = 1;
}

static void int16(void) {
    switch (AH) {
    case 0x00: case 0x10:
        if (!bios_key_available()) { bios_wait(); return; }
        AX = bios_key_read();
        break;
    case 0x01: case 0x11:
        if (!bios_key_available()) {
            bios_set_return_flag(F_ZF, 1);
            AX = 0;
            cpu.flags |= F_IF;
            cpu.idle = 1;
        } else {
            bios_set_return_flag(F_ZF, 0);
            AX = mem_read16(BDA + mem_read16(BDA + 0x1a));
        }
        break;
    case 0x02: case 0x12: AL = mem_read8(BDA + 0x17); if (AH == 0x12) AH = 0; break;
    case 0x05: buffer_key(CX); AL = 0; break;
    default: break;
    }
}

static void int15(void) {
    static u64 until;
    switch (AH) {
    case 0x86:
        if (!until) until = virtual_ns + (((u64)CX << 16 | DX) * 1000);
        if (virtual_ns < until) { bios_wait(); return; }
        until = 0;
        bios_set_return_flag(F_CF, 0);
        break;
    case 0x88: AX = 0; bios_set_return_flag(F_CF, 0); break;
    case 0x4f: bios_set_return_flag(F_CF, 1); break;
    case 0x90: case 0x91: AH = 0; bios_set_return_flag(F_CF, 0); break;
    default: AH = 0x86; bios_set_return_flag(F_CF, 1); break;
    }
}

void bios_trap(int vector) {
    switch (vector) {
    case 0x08: {                                      /* the timer tick, before INT 1Ch */
        u32 ticks = mem_read16(BDA + 0x6c) | (u32)mem_read16(BDA + 0x6e) << 16;
        if (++ticks >= 1573040) { ticks = 0; mem_write8(BDA + 0x70, 1); }
        mem_write16(BDA + 0x6c, (u16)ticks);
        mem_write16(BDA + 0x6e, (u16)(ticks >> 16));
        break;
    }
    case 0x09:
        if (scan_ready && !(scan_latch & 0x80) && scan_word) buffer_key(scan_word);
        scan_ready = 0;
        break;
    case 0x10: video_int10(); break;
    case 0x11: AX = 0x0021; break;
    case 0x12: AX = 640; break;
    case 0x13: AH = 1; bios_set_return_flag(F_CF, 1); break;
    case 0x14: AH = 0x80; break;
    case 0x15: int15(); break;
    case 0x16: int16(); break;
    case 0x17: AH = 0x08; break;
    case 0x1a: int1a(); break;
    case 0x20: dos_terminate(0); break;
    case 0x21: dos_int21(); break;
    case 0x27: dos_terminate(0); break;
    case 0x2f: break;
    case 0x33: int33(); break;
    default: break;
    }
}

void bios_tick(void) {
    if (virtual_ns >= next_timer) {
        u64 period = pit_period_ns(0);
        if (period < 50000) period = 50000;
        next_timer += period;
        if (next_timer + 5 * period < virtual_ns) next_timer = virtual_ns + period;
        if (!(irq_mask & 1)) irq_request |= 1;
    }
    keyboard_pump();
}

u64 bios_next_event(void) { return next_timer; }

/* ---- ports ---------------------------------------------------------------- */

u32 port_in(u16 port, int size) {
    u32 value = 0xff;
    switch (port) {
    case 0x20: value = irq_service; break;
    case 0x21: value = irq_mask; break;
    case 0x40: case 0x41: case 0x42: {
        int ch = port - 0x40;
        u16 count = pit_latched[ch] ? pit_latch[ch] : pit_current(ch);
        int access = pit_access[ch];
        if (access == 1) { value = count & 0xff; pit_latched[ch] = 0; }
        else if (access == 2) { value = count >> 8; pit_latched[ch] = 0; }
        else if (!pit_flip[ch]) { value = count & 0xff; pit_flip[ch] = 1; }
        else { value = count >> 8; pit_flip[ch] = 0; pit_latched[ch] = 0; }
        break;
    }
    case 0x60: value = scan_latch; break;
    case 0x61: value = (port61 & 3) | ((virtual_ns / 15085) & 1 ? 0x10 : 0); break;
    case 0x64: value = 0x14 | (scan_ready ? 1 : 0); break;
    case 0xa0: case 0xa1: value = 0; break;
    default:
        if (port >= 0x3b0 && port <= 0x3df) value = video_port_in(port);
        break;
    }
    if (size == 1) return value & 0xff;
    return size == 2 ? (value & 0xff) | 0xff00 : value | 0xffffff00u;
}

void port_out(u16 port, u32 value, int size) {
    if (size > 1 && port >= 0x3b0 && port <= 0x3df) {
        port_out(port, value & 0xff, 1);
        port_out((u16)(port + 1), value >> 8 & 0xff, 1);
        return;
    }
    u8 v = (u8)value;
    switch (port) {
    case 0x20:
        if (v == 0x20 && irq_service) irq_service &= irq_service - 1;    /* the highest priority one */
        else if ((v & 0xf8) == 0x60) irq_service &= ~(1u << (v & 7));
        break;
    case 0x21: irq_mask = v; break;
    case 0x40: case 0x41: case 0x42: {
        int ch = port - 0x40;
        int access = pit_access[ch];
        u32 reload = pit_reload[ch];
        if (access == 1) reload = (reload & 0xff00) | v;
        else if (access == 2) reload = (reload & 0xff) | (u32)v << 8;
        else if (!pit_flip[ch]) { reload = (reload & 0xff00) | v; pit_flip[ch] = 1; pit_reload[ch] = reload ? reload : 65536; return; }
        else { reload = (reload & 0xff) | (u32)v << 8; pit_flip[ch] = 0; }
        reload &= 0xffff;
        if (!reload) reload = 65536;
        pit_reload[ch] = reload;
        pit_started[ch] = virtual_ns;
        if (ch == 0) next_timer = virtual_ns + pit_period_ns(0);
        if (ch == 2) { speaker_divisor = reload; speaker_update(); }
        break;
    }
    case 0x43: {
        int ch = v >> 6;
        if (ch == 3) break;
        int access = v >> 4 & 3;
        if (!access) { pit_latch[ch] = pit_current(ch); pit_latched[ch] = 1; break; }
        pit_access[ch] = (u8)access;
        pit_flip[ch] = 0;
        break;
    }
    case 0x61: port61 = v; speaker_update(); break;
    default:
        if (port >= 0x3b0 && port <= 0x3df) video_port_out(port, v);
        break;
    }
}

void bios_init(void) {
    u32 at = STUB_BASE;
    u8 *rom = mem + (ROM_SEGMENT << 4);
    for (int n = 0; n < 256; ++n) {
        mem_write16((u32)n * 4, (u16)at);
        mem_write16((u32)n * 4 + 2, ROM_SEGMENT);
        rom[at++] = 0xf1;
        rom[at++] = (u8)n;
        rom[at++] = 0xcf;
    }
    static const u8 timer_stub[] = {0xf1, 0x08, 0xcd, 0x1c, 0x50, 0xb0, 0x20, 0xe6, 0x20, 0x58, 0xcf};
    static const u8 keyboard_stub[] = {0xf1, 0x09, 0x50, 0xb0, 0x20, 0xe6, 0x20, 0x58, 0xcf};
    static const u8 critical_stub[] = {0xb0, 0x03, 0xcf};           /* INT 24h: fail the call */
    memcpy(rom + at, timer_stub, sizeof(timer_stub));
    mem_write16(8 * 4, (u16)at);
    at += sizeof(timer_stub);
    memcpy(rom + at, keyboard_stub, sizeof(keyboard_stub));
    mem_write16(9 * 4, (u16)at);
    at += sizeof(keyboard_stub);
    memcpy(rom + at, critical_stub, sizeof(critical_stub));
    mem_write16(0x24 * 4, (u16)at);
    at += sizeof(critical_stub);
    rom[0xfff0] = 0xea;
    memcpy(rom + 0xfff5, "11/09/26", 8);
    rom[0xfffe] = 0xfc;                                 /* an AT */
    memcpy(rom + 0xfa6e, bios_font8, 8 * 128);
    mem_write16(0x43 * 4, 0xfa6e);
    mem_write16(0x43 * 4 + 2, ROM_SEGMENT);
    mem_write16(0x1f * 4, 0x8000);
    mem_write16(0x1f * 4 + 2, ROM_SEGMENT);
    memcpy(rom + 0x8000, bios_font8 + 128 * 8, 8 * 128);
    mem_write16(BDA + 0x10, 0x0021);
    mem_write16(BDA + 0x13, 640);
    mem_write16(BDA + 0x1a, KEYS_START);
    mem_write16(BDA + 0x1c, KEYS_START);
    mem_write16(BDA + 0x80, KEYS_START);
    mem_write16(BDA + 0x82, KEYS_END);
    next_timer = virtual_ns + pit_period_ns(0);
}
