/*
 * DOS-XC /os/runtime/display.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "runtime.h"
#include "usb.h"

#define MAX_ADAPTERS 8
#define MAX_MODES 24

enum { KIND_BOCHS = 1, KIND_VBE, KIND_GOP };
enum { XRES = 1, YRES, BPP, ENABLE, BANK, VIRT_W, VIRT_H, X_OFF, Y_OFF, REGISTERS };
#define VRAM64K 10
#define ENABLED 0x01
#define LFB 0x40
#define NOCLEAR 0x80

struct adapter {
    int kind, is_vga;
    struct pci_function pci;
    volatile u8 *mmio;               /* BAR2, or 0 for the legacy ports */
    u8 *vram;
    u32 vram_size;
    char name[32];
    u32 width, height, pitch;        /* current mode; width 0 in text mode */
    u64 framebuffer;
    u32 preferred_width, preferred_height;
    struct dosxc_display_mode modes[MAX_MODES];
    u16 firmware_mode[MAX_MODES];    /* VBE or GOP number of each mode */
    u32 firmware_pitch[MAX_MODES];
    u64 firmware_base[MAX_MODES];
    int mode_count;
    u16 saved[REGISTERS];            /* the registers before the first mode set */
    int has_saved;
};

static struct adapter adapters[MAX_ADAPTERS];
static int adapter_count, scanned, graphics, owner;
static u16 text_copy[2000];           /* the text screen, put back by bios_text */

static inline void outw(u16 port, u16 v) { __asm__ volatile("outw %0,%1" ::"a"(v), "Nd"(port)); }
static inline u16 inw(u16 port) { u16 v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(port)); return v; }

int display_graphics(void) { return graphics; }
int display_owner(void) { return owner; }

static int bios_video(u16 ax, u16 bx, u16 cx, u16 di) {
    volatile u8 *rpc = (volatile u8 *)0x7040;
    rpc[1] = 0x10;
    *(volatile u16 *)(rpc + 0x10) = ax;
    *(volatile u16 *)(rpc + 0x12) = bx;
    *(volatile u16 *)(rpc + 0x14) = cx;
    *(volatile u16 *)(rpc + 0x16) = di;
    bios_call();
    return *(volatile int *)(rpc + 8) & 0xffff;
}

static void bios_text(void) {
    bios_video(0x0003, 0, 0, 0);
    memcpy((void *)0xb8000, text_copy, sizeof(text_copy));
    unsigned row = 0, column = 0;
    if (ctx->get_cursor) ctx->get_cursor(&row, &column);
    ctx->cursor(row, column);
}

/* ---- Bochs adapters ---------------------------------------------------- */

static u16 dispi_read(const struct adapter *a, unsigned index) {
    if (a->mmio) return *(volatile u16 *)(a->mmio + 0x500 + index * 2);
    outw(0x1ce, (u16)index);
    return inw(0x1cf);
}

static void dispi_write(const struct adapter *a, unsigned index, u16 value) {
    if (a->mmio) { *(volatile u16 *)(a->mmio + 0x500 + index * 2) = value; return; }
    outw(0x1ce, (u16)index);
    outw(0x1cf, value);
}

static void vga_write(const struct adapter *a, u16 port, u8 value) {
    if (a->mmio) *(a->mmio + 0x400 + (port - 0x3c0)) = value;
    else rt_outb(port, value);
}

static u8 vga_read(const struct adapter *a, u16 port) {
    return a->mmio ? *(a->mmio + 0x400 + (port - 0x3c0)) : rt_inb(port);
}

/* The attribute controller must have its palette bit set or the VGA side
   of the adapter shows nothing. */
static void bochs_blank(const struct adapter *a, int blank) {
    vga_write(a, 0x3c2, 0x01);
    (void)vga_read(a, 0x3da);
    vga_write(a, 0x3c0, blank ? 0 : 0x20);
}

static void add_mode(struct adapter *a, u32 width, u32 height, unsigned firmware, u32 pitch, u64 base) {
    if (a->mode_count == MAX_MODES) return;
    for (int i = 0; i < a->mode_count; ++i)
        if (a->modes[i].width == width && a->modes[i].height == height) return;
    int at = a->mode_count++;
    while (at > 0 && (a->modes[at - 1].width > width ||
                      (a->modes[at - 1].width == width && a->modes[at - 1].height > height))) {
        a->modes[at] = a->modes[at - 1];
        a->firmware_mode[at] = a->firmware_mode[at - 1];
        a->firmware_pitch[at] = a->firmware_pitch[at - 1];
        a->firmware_base[at] = a->firmware_base[at - 1];
        --at;
    }
    a->modes[at].width = width;
    a->modes[at].height = height;
    a->firmware_mode[at] = (u16)firmware;
    a->firmware_pitch[at] = pitch;
    a->firmware_base[at] = base;
}

/* The preferred resolution from the EDID block QEMU puts at the start of
   the MMIO BAR. */
static void bochs_edid(struct adapter *a) {
    static const u8 magic[8] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
    if (!a->mmio) return;
    u8 edid[128];
    for (unsigned i = 0; i < sizeof(edid); ++i) edid[i] = a->mmio[i];
    if (memcmp(edid, magic, 8) || (!edid[54] && !edid[55])) return;
    a->preferred_width = edid[56] | (u32)(edid[58] & 0xf0) << 4;
    a->preferred_height = edid[59] | (u32)(edid[61] & 0xf0) << 4;
}

static void found_display(const struct pci_function *f) {
    if (adapter_count == MAX_ADAPTERS || f->vendor != 0x1234 || f->id != 0x1111) return;
    struct adapter *a = &adapters[adapter_count];
    memset(a, 0, sizeof(*a));
    a->kind = KIND_BOCHS;
    a->pci = *f;
    a->is_vga = (pci_read32(f, 8) >> 16) == 0x0300;
    int io = 0;
    u64 vram = pci_bar(f, 0, &io), mmio = 0;
    if (io || !vram) return;
    mmio = pci_bar(f, 2, &io);
    pci_enable(f, 1, 1);
    a->vram = (u8 *)(uptr)vram;
    a->mmio = mmio && !io ? (volatile u8 *)(uptr)mmio : 0;
    if (!a->mmio && !a->is_vga) return;
    if ((dispi_read(a, 0) & 0xfff0) != 0xb0c0) return;
    a->vram_size = (u32)dispi_read(a, VRAM64K) << 16;
    if (!a->vram_size) return;
    bochs_edid(a);
    static const u16 common[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 800}, {1280, 1024},
                                    {1366, 768}, {1440, 900}, {1600, 900}, {1680, 1050}, {1920, 1080}, {1920, 1200},
                                    {2560, 1440}};
    for (unsigned i = 0; i < sizeof(common) / sizeof(*common); ++i)
        if ((u32)common[i][0] * common[i][1] * 4 <= a->vram_size)
            add_mode(a, common[i][0], common[i][1], 0, 0, 0);
    if (a->preferred_width && a->preferred_width * a->preferred_height * 4 <= a->vram_size)
        add_mode(a, a->preferred_width, a->preferred_height, 0, 0, 0);
    if (!a->mode_count) return;
    const char *name = a->is_vga ? "QEMU VGA" : "QEMU display";
    rt_strcpy(a->name, name);
    ++adapter_count;
}

static int bochs_set(struct adapter *a, u32 width, u32 height) {
    if (!a->has_saved) {
        for (unsigned i = 1; i < REGISTERS; ++i) a->saved[i] = dispi_read(a, i);
        a->has_saved = 1;
    }
    bochs_blank(a, 1);
    dispi_write(a, ENABLE, 0);
    dispi_write(a, BPP, 32);
    dispi_write(a, XRES, (u16)width);
    dispi_write(a, YRES, (u16)height);
    dispi_write(a, BANK, 0);
    dispi_write(a, VIRT_W, (u16)width);
    dispi_write(a, VIRT_H, (u16)height);
    dispi_write(a, X_OFF, 0);
    dispi_write(a, Y_OFF, 0);
    dispi_write(a, ENABLE, ENABLED | LFB);
    bochs_blank(a, 0);
    a->width = width;
    a->height = height;
    a->pitch = width * 4;
    a->framebuffer = (u64)(uptr)a->vram;
    return 0;
}

static void bochs_text(struct adapter *a) {
    if (!a->has_saved) return;
    if (a->saved[ENABLE] & ENABLED) {
        bochs_blank(a, 1);
        dispi_write(a, ENABLE, 0);
        for (unsigned i = BPP; i < REGISTERS; ++i)
            if (i != ENABLE) dispi_write(a, i, a->saved[i]);
        dispi_write(a, XRES, a->saved[XRES]);
        dispi_write(a, YRES, a->saved[YRES]);
        dispi_write(a, ENABLE, a->saved[ENABLE] | NOCLEAR);
        bochs_blank(a, 0);
        return;
    }
    dispi_write(a, ENABLE, 0);
    /* Only the primary VGA has a text mode, and turning DISPI off does not
       bring back its VGA registers. */
    if (!a->is_vga) bochs_blank(a, 1);
    else if (ctx->bios_available) bios_text();
}

static void vbe_scan(void) {
    u8 *info = (u8 *)0x78000, *mode = (u8 *)0x78400;
    memset(info, 0, 512);
    memcpy(info, "VBE2", 4);
    if (bios_video(0x4f00, 0, 0, 0) != 0x004f || memcmp(info, "VESA", 4)) return;
    u32 pointer = *(u32 *)(info + 14);
    const u16 *list = (const u16 *)(uptr)((pointer >> 16) * 16 + (pointer & 0xffff));
    static u16 numbers[256];
    unsigned count = 0;
    while (count < 256 && list[count] != 0xffff) { numbers[count] = list[count]; ++count; }
    struct adapter *a = &adapters[0];
    memset(a, 0, sizeof(*a));
    a->kind = KIND_VBE;
    for (unsigned i = 0; i < count; ++i) {
        memset(mode, 0, 256);
        if (bios_video(0x4f01, 0, numbers[i], 0x400) != 0x004f) continue;
        u16 attributes = *(u16 *)mode;
        u16 width = *(u16 *)(mode + 0x12), height = *(u16 *)(mode + 0x14);
        if ((attributes & 0x91) != 0x91 || mode[0x19] != 32 || mode[0x1b] != 6) continue;
        if (mode[0x20] != 16 || mode[0x22] != 8 || mode[0x24] != 0) continue;
        if (width < 640 || height < 480) continue;
        u32 pitch = *(u16 *)(mode + 0x10);
        add_mode(a, width, height, numbers[i], pitch, *(u32 *)(mode + 0x28));
    }
    if (!a->mode_count) return;
    rt_strcpy(a->name, "VESA BIOS display");
    adapter_count = 1;
}

static void gop_scan(void) {
    int total = ctx->display(0, 0, 0, 0, 0, 0);
    struct adapter *a = &adapters[0];
    memset(a, 0, sizeof(*a));
    a->kind = KIND_GOP;
    for (int i = 0; i < total; ++i) {
        unsigned width, height, pitch;
        u64 base;
        if (ctx->display(1, (unsigned)i, &width, &height, &pitch, &base)) continue;
        if (width >= 640 && height >= 480) add_mode(a, width, height, (unsigned)i, pitch, base);
    }
    if (!a->mode_count) return;
    rt_strcpy(a->name, "UEFI display");
    adapter_count = 1;
}

static void scan(void) {
    if (scanned) return;
    scanned = 1;
    adapter_count = 0;
    pci_scan_class(3, 0, found_display);
    pci_scan_class(3, 0x80, found_display);
    if (!adapter_count && ctx->bios_available) vbe_scan();
    if (!adapter_count && !ctx->bios_available && ctx->display) gop_scan();
}

static int set_mode(struct adapter *a, const struct dosxc_display_mode *wanted) {
    int index = -1;
    for (int i = 0; i < a->mode_count; ++i)
        if (a->modes[i].width == wanted->width && a->modes[i].height == wanted->height) index = i;
    if (index < 0) return -DOSXC_EINVAL;
    if (!graphics) {
        terminal_mouse_hide();
        if (ctx->bios_available) memcpy(text_copy, (void *)0xb8000, sizeof(text_copy));
    }
    int r = 0;
    if (a->kind == KIND_BOCHS) r = bochs_set(a, wanted->width, wanted->height);
    else if (a->kind == KIND_VBE) {
        if (bios_video(0x4f02, (u16)(a->firmware_mode[index] | 0x4000), 0, 0) != 0x004f) r = -DOSXC_EIO;
        a->framebuffer = a->firmware_base[index];
        a->pitch = a->firmware_pitch[index];
    } else {
        unsigned width, height, pitch;
        u64 base;
        if (ctx->display(2, a->firmware_mode[index], &width, &height, &pitch, &base)) r = -DOSXC_EIO;
        else { a->framebuffer = base; a->pitch = pitch; }
    }
    if (r) return r;
    a->width = wanted->width;
    a->height = wanted->height;
    graphics = 1;
    owner = current ? current->pid : 0;
    if (current && a->framebuffer < 0x100000000ull) aspace_user(current->space, (uptr)a->framebuffer, (uptr)a->pitch * a->height, 2);
    return 0;
}

void display_text(void) {
    if (!graphics) return;
    for (int i = adapter_count - 1; i >= 0; --i) {
        struct adapter *a = &adapters[i];
        if (!a->width) continue;
        if (a->framebuffer < 0x100000000ull) aspace_user_all((uptr)a->framebuffer, (uptr)a->pitch * a->height, 0);
        if (a->kind == KIND_BOCHS) bochs_text(a);
        else if (a->kind == KIND_VBE) bios_text();
        else ctx->display(3, 0, 0, 0, 0, 0);
        a->width = a->height = a->pitch = 0;
    }
    if (!ctx->bios_available && ctx->display && adapters[0].kind == KIND_BOCHS) ctx->display(3, 0, 0, 0, 0, 0);
    graphics = 0;
    owner = 0;
}

void display_process_finished(int pid) {
    if (graphics && owner == pid) display_text();
}

int display_syscall(int operation, long index, void *buffer, dosxc_size size) {
    scan();
    if (operation == DOSXC_DISPLAY_TEXT) { display_text(); return 0; }
    if (index < 0 || index >= adapter_count) {
        if (operation == DOSXC_DISPLAY_INFO && !buffer) return adapter_count;
        return -DOSXC_ENODEV;
    }
    struct adapter *a = &adapters[index];
    switch (operation) {
    case DOSXC_DISPLAY_INFO: {
        if (!buffer) return adapter_count;
        struct dosxc_display *d = buffer;
        memset(d, 0, sizeof(*d));
        rt_strcpy(d->name, a->name);
        d->width = a->width;
        d->height = a->height;
        d->pitch = a->pitch;
        d->framebuffer = a->framebuffer;
        d->preferred_width = a->preferred_width;
        d->preferred_height = a->preferred_height;
        return adapter_count;
    }
    case DOSXC_DISPLAY_MODES:
        if (buffer)
            for (int i = 0; i < a->mode_count && (dosxc_size)i < size; ++i)
                ((struct dosxc_display_mode *)buffer)[i] = a->modes[i];
        return a->mode_count;
    case DOSXC_DISPLAY_SET:
        return buffer ? set_mode(a, buffer) : -DOSXC_EFAULT;
    }
    return -DOSXC_EINVAL;
}
