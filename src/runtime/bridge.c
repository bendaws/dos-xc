/*
 * DOS-XC /runtime/bridge.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * The runtime half of the real-mode USB bridge (src/dos/bridge.asm).
 * bridge_install() copies the bridge to 0x1000 and points INT 13h, 16h and
 * 33h at it; the bridge calls bridge_entry (switch*.asm), which runs
 * bridge_service() on its own stack, with interrupts off, for:
 *   1  INT 13h on a BIOS drive that is a USB disk
 *   2  INT 16h: move USB keystrokes into the BIOS keyboard buffer
 *   3  INT 33h: the mouse driver
 *   4  INT 13h read of the live CD by 16-bit code: patch in the RAM overlay
 *   5  INT 13h write of live CD image sectors: into the RAM overlay
 * It also replaces the BIOS timer tick (INT 08h), which on some BIOSes
 * polls the BIOS's own USB drivers.
 */

#include "usb.h"

struct bridge {
    char magic[4];
    u16 int13, int16, int33, int08, pad0[2];
    u32 old13, old16, old33, entry, root, bits, cr4;
    u8 drives[8];
    u8 busy, live_drive, runtime_active, pad1;
    u32 op, eax, ebx, ecx, edx, esi, edi, ebp;
    u16 ds, es, flags, pad2;
    u32 result;
};
_Static_assert(__builtin_offsetof(struct bridge, op) == 0x38, "bridge layout");
_Static_assert(__builtin_offsetof(struct bridge, result) == 0x60, "bridge layout");

#define BRIDGE ((struct bridge *)0x1000)
#define IVT ((volatile u32 *)0)

extern const u8 bridge_image[], bridge_image_end[];
extern char bridge_entry[];

static u8 *linear(u16 segment, u16 offset) { return (u8 *)(uptr)((u32)segment * 16 + offset); }
static void set_ah(struct bridge *b, u8 ah) { b->eax = (b->eax & ~0xff00u) | (u32)ah << 8; }

static int drive_disk[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
static u8 drive_status[8];

/* 64-bit: identity map of the first 4 GiB for the bridge's long mode (the
   boot loader's tables live in memory 16-bit programs overwrite). */
static u32 build_tables(void) {
#if __SIZEOF_POINTER__ == 8
    u64 *pages = memory_alloc(6 * 4096, 4096, OWNER_KERNEL, 1);
    if (!pages) return 0;
    u64 *pml4 = pages, *pdpt = pages + 512;
    pml4[0] = (u64)(uptr)pdpt | 3;
    for (int g = 0; g < 4; ++g) {
        u64 *pd = pages + 1024 + 512 * g;
        pdpt[g] = (u64)(uptr)pd | 3;
        for (int i = 0; i < 512; ++i) pd[i] = ((u64)g << 30) + ((u64)i << 21) + 0x83;
    }
    return (u32)(uptr)pml4;
#else
    return 0;
#endif
}

void bridge_install(void) {
    uptr size = (uptr)(bridge_image_end - bridge_image);
    if (size > 0x1000 || memcmp(bridge_image, "BRDG", 4)) return;
    struct bridge *b = BRIDGE;
    /* Already installed (memory survives a warm reboot, so look at the vectors). */
    if (!memcmp(b->magic, "BRDG", 4) && IVT[0x13] == (0x01000000u | b->int13)) return;
    u32 root = build_tables();
    if (runtime_bits == 64 && !root) return;
    memcpy(b, bridge_image, size);
    memset((u8 *)b + 0x1000, 0, 0x5000);                  /* its stacks */
    b->entry = (u32)(uptr)bridge_entry;
    b->root = root;
    b->bits = (u32)runtime_bits;
    uptr cr4;
    __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
    b->cr4 = (u32)cr4 & ~(1u << 7);                      /* no global pages: CR3 reloads must flush */
    memset(b->drives, 0xff, sizeof(b->drives));
    /* The live CD (CDVF boot): 16-bit code must see the RAM overlay. */
    b->live_drive = *(volatile u32 *)0x7010 == 0x46564443u ? *(volatile u8 *)0x7014 : 0xff;
    b->runtime_active = 1;
    b->old13 = IVT[0x13];
    b->old16 = IVT[0x16];
    b->old33 = IVT[0x33];
    IVT[0x13] = 0x01000000u | b->int13;
    IVT[0x16] = 0x01000000u | b->int16;
    IVT[0x33] = 0x01000000u | b->int33;
    IVT[0x08] = 0x01000000u | b->int08;
}

void bridge_runtime_active(int active) {
    struct bridge *b = BRIDGE;
    if (!memcmp(b->magic, "BRDG", 4)) b->runtime_active = (u8)(active != 0);
}

/* The live CD: CD LBA (2048-byte blocks) <-> image sector (512 bytes). */
static u32 image_base(void) { return *(volatile u32 *)0x701c; }

static void live_read(struct bridge *b) {
    u8 *dap = linear(b->ds, (u16)b->esi);
    u16 count = *(u16 *)(dap + 2), offset = *(u16 *)(dap + 4), segment = *(u16 *)(dap + 6);
    u64 lba = *(u64 *)(dap + 8);
    u8 *buffer = linear(segment, offset);
    if (lba < image_base()) return;
    for (u32 block = 0; block < count; ++block)
        for (u32 k = 0; k < 4; ++k)
            cache_overlay_read((u32)((lba + block - image_base()) * 4 + k), buffer + block * 2048 + k * 512);
}

static void live_write(struct bridge *b) {
    u8 *dap = linear(b->ds, (u16)b->esi);
    u16 count = *(u16 *)(dap + 2), offset = *(u16 *)(dap + 4), segment = *(u16 *)(dap + 6);
    u32 lba = *(u32 *)(dap + 8);
    u8 *buffer = linear(segment, offset);
    u8 status = 0;
    for (u32 i = 0; i < count; ++i)
        if (cache_overlay_write(lba + i, buffer + i * 512)) { status = 0x03; *(u16 *)(dap + 2) = (u16)i; break; }
    set_ah(b, status);
    b->flags = status != 0;
}

void bridge_set_drive(u8 bios_drive, int usb_disk) {
    struct bridge *b = BRIDGE;
    if (memcmp(b->magic, "BRDG", 4)) return;
    for (int i = 0; i < 8; ++i)
        if (b->drives[i] == bios_drive) {
            if (usb_disk < 0) b->drives[i] = 0xff;
            drive_disk[i] = usb_disk;
            return;
        }
    if (usb_disk < 0) return;
    for (int i = 0; i < 8; ++i)
        if (b->drives[i] == 0xff) {
            drive_disk[i] = usb_disk;
            drive_status[i] = 0;
            b->drives[i] = bios_drive;
            return;
        }
}

/* ---- INT 13h -------------------------------------------------------------- */

#define HEADS 255u
#define SECTORS 63u

static void int13(struct bridge *b) {
    u8 function = (u8)(b->eax >> 8), drive = (u8)b->edx;
    int slot = -1;
    for (int i = 0; i < 8; ++i) if (b->drives[i] == drive) slot = i;
    struct usb_disk_info info;
    int disk = slot >= 0 ? drive_disk[slot] : -1;
    u8 status = 0;
    b->flags = 0;
    if (disk < 0 || usb_disk_info(disk, &info) || !info.present) {
        status = 0xaa;                                     /* drive not ready */
        goto out;
    }
    u64 cylinders = info.sectors / (HEADS * SECTORS);
    if (cylinders > 1024) cylinders = 1024;
    if (!cylinders) cylinders = 1;
    switch (function) {
    case 0x00: break;
    case 0x01: status = drive_status[slot]; b->flags = status != 0; set_ah(b, status); return;
    case 0x02: case 0x03: case 0x04: {
        u32 count = b->eax & 0xff;
        u32 cl = b->ecx & 0xff, ch = (b->ecx >> 8) & 0xff, head = (b->edx >> 8) & 0xff;
        u32 cylinder = ch | (cl & 0xc0) << 2, sector = cl & 0x3f;
        if (!sector || !count) { status = 1; break; }
        u64 lba = ((u64)cylinder * HEADS + head) * SECTORS + sector - 1;
        if (function != 0x04 &&
            usb_disk_io(disk, lba, count, linear(b->es, (u16)b->ebx), function == 0x03)) {
            status = 0x04;
            b->eax &= ~0xffu;
        }
        break;
    }
    case 0x08: {
        u32 c = (u32)cylinders - 1;
        b->eax &= ~0xffffu;
        b->ecx = (b->ecx & ~0xffffu) | (c & 0xff) << 8 | (c >> 2 & 0xc0) | SECTORS;
        b->edx = (b->edx & ~0xffffu) | (HEADS - 1) << 8 | *(volatile u8 *)0x475;
        b->ebx &= ~0xffu;
        break;
    }
    case 0x15:
        set_ah(b, 3);
        b->ecx = (b->ecx & ~0xffffu) | (u32)(info.sectors >> 16 & 0xffff);
        b->edx = (b->edx & ~0xffffu) | (u32)(info.sectors & 0xffff);
        drive_status[slot] = 0;
        return;
    case 0x41:
        if ((u16)b->ebx != 0x55aa) { status = 1; break; }
        b->ebx = (b->ebx & ~0xffffu) | 0xaa55;
        b->ecx = (b->ecx & ~0xffffu) | 1;
        set_ah(b, 0x30);
        drive_status[slot] = 0;
        return;
    case 0x42: case 0x43: case 0x44: {
        u8 *dap = linear(b->ds, (u16)b->esi);
        u16 count = *(u16 *)(dap + 2), offset = *(u16 *)(dap + 4), segment = *(u16 *)(dap + 6);
        u64 lba = *(u64 *)(dap + 8);
        if (function != 0x44 && count &&
            usb_disk_io(disk, lba, count, linear(segment, offset), function == 0x43)) {
            *(u16 *)(dap + 2) = 0;
            status = 0x04;
        }
        break;
    }
    case 0x47: break;
    case 0x48: {
        u8 *p = linear(b->ds, (u16)b->esi);
        u16 size = *(u16 *)p;
        if (size < 26) { status = 1; break; }
        memset(p + 2, 0, (uptr)(size < 30 ? 24 : 28));
        *(u16 *)p = size < 30 ? 26 : 30;
        *(u16 *)(p + 2) = 2 | (info.removable ? 4 : 0);   /* geometry valid, removable */
        *(u32 *)(p + 4) = (u32)cylinders;
        *(u32 *)(p + 8) = HEADS;
        *(u32 *)(p + 12) = SECTORS;
        *(u64 *)(p + 16) = info.sectors;
        *(u16 *)(p + 24) = 512;
        if (size >= 30) *(u32 *)(p + 26) = 0xffffffffu;
        break;
    }
    default: status = 1; break;
    }
out:
    if (slot >= 0) drive_status[slot] = status;
    set_ah(b, status);
    b->flags = status != 0;
}

/* ---- INT 16h: the BIOS keyboard buffer -------------------------------- */

static void keys(void) {
    volatile u16 *head = (volatile u16 *)0x41a, *tail = (volatile u16 *)0x41c;
    u16 start = *(volatile u16 *)0x480, end = *(volatile u16 *)0x482;
    if (start < 0x1e || end <= start || end > 0x100) { start = 0x1e; end = 0x3e; }
    int word;
    while ((word = usb_key_bios()) >= 0) {
        u16 next = (u16)(*tail + 2);
        if (next >= end) next = start;
        if (next == *head) break;                           /* full */
        *(volatile u16 *)(uptr)(0x400 + *tail) = (u16)word;
        *tail = next;
    }
    if (usb_keyboards()) {
        volatile u8 *flags = (volatile u8 *)0x417;
        *flags = (u8)((*flags & 0x80) | usb_shift_flags());
    }
}

/* ---- INT 33h: the mouse ------------------------------------------------- */

static struct {
    int x, y, min_x, max_x, min_y, max_y, visible;      /* visible: 0 shown, < 0 hidden */
    int fraction_x, fraction_y, mickeys_x, mickeys_y;
    u8 buttons;
    u16 presses[3], releases[3];
    int press_x[3], press_y[3], release_x[3], release_y[3];
    int drawn;                                          /* a cursor cell is on screen */
    volatile u16 *cell;
    u16 saved;
} m;

static volatile u16 *text_cell(void) {
    u8 mode = *(volatile u8 *)0x449;
    if (mode != 2 && mode != 3 && mode != 7) return 0;
    u16 columns = *(volatile u16 *)0x44a;
    if (!columns) columns = 80;
    u32 rows = (u32)*(volatile u8 *)0x484 + 1;
    if (rows < 25) rows = 25;
    int column = m.x / 8, row = m.y / 8;
    if (column >= columns || (u32)row >= rows) return 0;
    u16 page_offset = *(volatile u16 *)0x44e;
    u8 *base = (u8 *)(uptr)(mode == 7 ? 0xb0000 : 0xb8000);
    return (volatile u16 *)(base + page_offset + (row * columns + column) * 2);
}

static void hide_cursor(void) {
    if (!m.drawn) return;
    m.drawn = 0;
    /* Put the attribute back unless the program has redrawn the cell. */
    if (*m.cell == (u16)(m.saved ^ 0x7700)) *m.cell = m.saved;
}

static void show_cursor(void) {
    if (m.drawn || m.visible < 0) return;
    volatile u16 *cell = text_cell();
    if (!cell) return;
    m.cell = cell;
    m.saved = *cell;
    *cell = (u16)(m.saved ^ 0x7700);
    m.drawn = 1;
}

static void clamp(void) {
    if (m.x < m.min_x) m.x = m.min_x;
    if (m.x > m.max_x) m.x = m.max_x;
    if (m.y < m.min_y) m.y = m.min_y;
    if (m.y > m.max_y) m.y = m.max_y;
}

static void mouse_reset(void) {
    hide_cursor();
    memset(&m, 0, sizeof(m));
    m.max_x = 639;
    m.max_y = 199;
    u8 mode = *(volatile u8 *)0x449;
    if (mode == 0 || mode == 1) m.max_x = 639;        /* 40-column text: still 640 wide */
    m.x = (m.max_x + 1) / 2;
    m.y = (m.max_y + 1) / 2;
    m.visible = -1;
}

static void mouse_update(void) {
    struct usb_mouse_state s;
    usb_mouse_take(&s);
    if (!s.dx && !s.dy && s.buttons == m.buttons) return;
    int moved = s.dx || s.dy;
    if (moved && m.drawn) hide_cursor();
    m.mickeys_x += s.dx;
    m.mickeys_y += s.dy;
    m.fraction_x += s.dx * 8;
    m.fraction_y += s.dy * 8;
    m.x += m.fraction_x / 8;
    m.fraction_x %= 8;
    m.y += m.fraction_y / 16;
    m.fraction_y %= 16;
    clamp();
    for (int i = 0; i < 3; ++i) {
        u8 bit = (u8)(1 << i);
        if ((s.buttons & bit) && !(m.buttons & bit)) { ++m.presses[i]; m.press_x[i] = m.x; m.press_y[i] = m.y; }
        if (!(s.buttons & bit) && (m.buttons & bit)) { ++m.releases[i]; m.release_x[i] = m.x; m.release_y[i] = m.y; }
    }
    m.buttons = s.buttons;
    if (moved) show_cursor();
}

static void set16(u32 *reg, int value) { *reg = (*reg & ~0xffffu) | (u16)value; }

static void int33(struct bridge *b) {
    u16 function = (u16)b->eax;
    b->result = 0;
    if (!usb_mice() && function != 0x00 && function != 0x21) { b->result = 1; return; }
    if (m.max_x == 0 && function != 0x00 && function != 0x21) mouse_reset();
    mouse_update();
    int button = (u16)b->ebx < 3 ? (u16)b->ebx : 0;
    switch (function) {
    case 0x00: case 0x21:
        if (!usb_mice()) { b->result = 1; return; }
        mouse_reset();
        set16(&b->eax, 0xffff);
        set16(&b->ebx, 2);
        break;
    case 0x01: if (m.visible < 0 && ++m.visible == 0) show_cursor(); break;
    case 0x02: if (m.visible-- == 0) hide_cursor(); break;
    case 0x03: set16(&b->ebx, m.buttons); set16(&b->ecx, m.x); set16(&b->edx, m.y); break;
    case 0x04:
        hide_cursor();
        m.x = (short)b->ecx;
        m.y = (short)b->edx;
        clamp();
        show_cursor();
        break;
    case 0x05:
        set16(&b->eax, m.buttons);
        set16(&b->ebx, m.presses[button]);
        set16(&b->ecx, m.press_x[button]);
        set16(&b->edx, m.press_y[button]);
        m.presses[button] = 0;
        break;
    case 0x06:
        set16(&b->eax, m.buttons);
        set16(&b->ebx, m.releases[button]);
        set16(&b->ecx, m.release_x[button]);
        set16(&b->edx, m.release_y[button]);
        m.releases[button] = 0;
        break;
    case 0x07: {
        int a = (short)b->ecx, c = (short)b->edx;
        m.min_x = a < c ? a : c;
        m.max_x = a < c ? c : a;
        hide_cursor(); clamp(); show_cursor();
        break;
    }
    case 0x08: {
        int a = (short)b->ecx, c = (short)b->edx;
        m.min_y = a < c ? a : c;
        m.max_y = a < c ? c : a;
        hide_cursor(); clamp(); show_cursor();
        break;
    }
    case 0x0b:
        set16(&b->ecx, m.mickeys_x);
        set16(&b->edx, m.mickeys_y);
        m.mickeys_x = m.mickeys_y = 0;
        break;
    case 0x24:                                          /* version 8.05, PS/2 type */
        set16(&b->ebx, 0x0805);
        set16(&b->ecx, 0x0400);
        break;
    default: break;                                     /* handlers, ratios, pages: accepted */
    }
}

/* Called by bridge_entry with interrupts off. */
void bridge_service(void) {
    struct bridge *b = BRIDGE;
    if (usb_busy()) {
        if (b->op == 1) { set_ah(b, 0xaa); b->flags = 1; }
        b->result = b->op == 3;
        return;
    }
    switch (b->op) {
    case 1: int13(b); break;
    case 2: usb_poll(); keys(); break;
    case 3: usb_poll(); int33(b); break;
    case 4: live_read(b); break;
    case 5: live_write(b); break;
    }
}
