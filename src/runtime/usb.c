/*
 * DOS-XC /runtime/usb.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * USB devices: enumeration, hubs and hot plug, boot-protocol keyboards and
 * mice, and bulk-only mass storage (SCSI).  The host controller drivers do
 * the transfers; this file never touches controller registers.
 *
 * Hot plug is found by polling: root hub ports every 50 ms, external hubs
 * through their status-change endpoints.  A device that goes away is
 * removed with everything behind it, and its disks are unmounted.
 */

#include "usb.h"

#define MAX_HCS 16
#define MAX_DEVICES 64
#define MAX_DEPTH_TREE 5

static struct usb_hc hcs[MAX_HCS];
static struct usb_device devices[MAX_DEVICES];
static int started, busy;
static u64 last_tick, last_ports;
static u8 *control_buffer;            /* DMA bounce for control data */

int usb_active(void) { return started; }

#define USB_DEBUG 0     /* 1: trace enumeration on the console */
static void trace(const char *what, u64 value) {
#if USB_DEBUG
    rt_print("[usb] "); rt_print(what); rt_print(" "); rt_print_hex(value); rt_print("\r\n");
#else
    (void)what; (void)value;
#endif
}

struct usb_hc *usb_add_hc(const struct usb_hc_ops *ops, void *data, int ports) {
    for (int i = 0; i < MAX_HCS; ++i) {
        if (hcs[i].used) continue;
        struct usb_hc *hc = &hcs[i];
        memset(hc, 0, sizeof(*hc));
        hc->used = 1;
        hc->ops = ops;
        hc->data = data;
        hc->ports = ports > USB_MAX_PORTS ? USB_MAX_PORTS : ports;
        return hc;
    }
    return 0;
}

static int new_address(struct usb_hc *hc) {
    for (int a = 1; a < 128; ++a)
        if (!(hc->addresses[a / 8] & (1 << (a % 8)))) {
            hc->addresses[a / 8] |= (u8)(1 << (a % 8));
            return a;
        }
    return -1;
}
static void free_address(struct usb_hc *hc, int a) {
    if (a > 0 && a < 128) hc->addresses[a / 8] &= (u8)~(1 << (a % 8));
}

int usb_control(struct usb_device *d, u8 type, u8 request, u16 value, u16 index, void *data, u16 length) {
    if (d->gone) return USB_ENODEV;
    if (length > 4096) return USB_EIO;
    struct usb_setup setup = {type, request, value, index, length};
    if (length && !(type & 0x80)) memcpy(control_buffer, data, length);
    int r = d->hc->ops->control(d, &setup, length ? control_buffer : 0);
    if (r > length) r = length;
    if (r > 0 && (type & 0x80) && data) memcpy(data, control_buffer, (uptr)r);
    return r;
}

static int get_descriptor(struct usb_device *d, u8 type, u8 index, void *buffer, u16 length) {
    int r = USB_EIO;
    for (int attempt = 0; attempt < 3; ++attempt) {
        r = usb_control(d, 0x80, 6, (u16)(type << 8 | index), 0, buffer, length);
        if (r >= 0 || r == USB_ENODEV) break;
        cpu_sleep(10000000);
    }
    return r;
}

/* ---- keyboards -------------------------------------------------------- */

#define KEY_QUEUE 64
static struct { int key; u16 bios; } keys[KEY_QUEUE];
static unsigned key_head, key_tail;
static u8 modifiers, caps_lock, num_lock = 1, scroll_lock;
static u8 held_usage;
static u64 held_since, held_next;

struct keyboard {
    struct usb_pipe pipe;
    u8 previous[8];
    u8 leds;
};

/* HID usage -> PC set-1 scan code, unshifted and shifted ASCII. */
static const u8 usage_scan[0x65] = {
    [0x04] = 0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19,
    0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c,
    [0x1e] = 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
    [0x28] = 0x1c, 0x01, 0x0e, 0x0f, 0x39, 0x0c, 0x0d, 0x1a, 0x1b, 0x2b, 0x2b, 0x27, 0x28, 0x29, 0x33, 0x34, 0x35,
    [0x39] = 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,
    [0x47] = 0x46, [0x49] = 0x52, 0x47, 0x49, 0x53, 0x4f, 0x51, 0x4d, 0x4b, 0x50, 0x48, 0x45,
    [0x54] = 0x35, 0x37, 0x4a, 0x4e, 0x1c, 0x4f, 0x50, 0x51, 0x4b, 0x4c, 0x4d, 0x47, 0x48, 0x49, 0x52, 0x53, 0x56,
};
static const char usage_plain[0x65] =
    "\0\0\0\0abcdefghijklmnopqrstuvwxyz1234567890\r\x1b\b\t -=[]\\\\;'`,./";
static const char usage_shift[0x65] =
    "\0\0\0\0ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\r\x1b\b\t _+{}||:\"~<>?";

static void push_key(int key, u16 bios) {
    unsigned next = (key_tail + 1) % KEY_QUEUE;
    if (next == key_head) return;
    keys[key_tail].key = key;
    keys[key_tail].bios = bios;
    key_tail = next;
}

/* One key press (or repeat) of USAGE with the current modifiers. */
static void key_press(u8 usage) {
    int shift = (modifiers & 0x22) != 0, ctrl = (modifiers & 0x11) != 0, alt = (modifiers & 0x44) != 0;
    u8 scan = usage < sizeof(usage_scan) ? usage_scan[usage] : 0;
    if (!scan) return;
    /* Keypad: digits with Num Lock (and no Shift), navigation without. */
    if (usage >= 0x54 && usage <= 0x63) {
        static const char pad[] = "/*-+\r1234567890.";
        char c = pad[usage - 0x54];
        int digits = usage >= 0x59 && num_lock != shift;
        if (usage < 0x59 || digits) {
            push_key(c == '\r' ? '\n' : c, (u16)(scan << 8 | (u8)c));
            return;
        }
        static const u8 nav[] = {0x4f, 0x50, 0x51, 0x4b, 0, 0x4d, 0x47, 0x48, 0x49, 0x52, 0x53};
        static const u8 nav_usage[] = {0x4d, 0x51, 0x4e, 0x50, 0, 0x4f, 0x4a, 0x52, 0x4b, 0x49, 0x4c};
        unsigned i = usage - 0x59u;
        if (!nav[i]) return;
        usage = nav_usage[i];
        scan = nav[i];
    }
    switch (usage) {
    case 0x39: caps_lock ^= 1; return;
    case 0x53: num_lock ^= 1; return;
    case 0x47: scroll_lock ^= 1; return;
    }
    if (usage >= 0x3a && usage <= 0x45) {         /* F1..F12 */
        int n = usage - 0x3a;
        u8 bios_scan;
        if (n < 10) bios_scan = (u8)(alt ? 0x68 + n : ctrl ? 0x5e + n : shift ? 0x54 + n : 0x3b + n);
        else bios_scan = (u8)((alt ? 0x8b : ctrl ? 0x89 : shift ? 0x87 : 0x85) + n - 10);
        push_key(0x10b + n, (u16)(bios_scan << 8));
        return;
    }
    if (usage >= 0x49 && usage <= 0x52) {         /* navigation block */
        static const int code[] = {KEY_INSERT, KEY_HOME, KEY_PAGE_UP, KEY_DELETE, KEY_END, KEY_PAGE_DOWN,
                                   KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP};
        static const u8 ctrl_scan[] = {0x92, 0x77, 0x84, 0x93, 0x75, 0x76, 0x74, 0x73, 0x91, 0x8d};
        int i = usage - 0x49;
        push_key(code[i], (u16)((ctrl ? ctrl_scan[i] : scan) << 8));
        return;
    }
    char c = usage < sizeof(usage_plain) ? usage_plain[usage] : 0;
    if (!c) return;
    if (usage >= 0x04 && usage <= 0x1d) {
        int upper = shift != caps_lock;
        char letter = upper ? (char)(c - 32) : c;
        if (alt) { push_key(-1, (u16)(scan << 8)); return; }
        if (ctrl) { push_key(c & 31, (u16)(scan << 8 | (c & 31))); return; }
        push_key(letter, (u16)(scan << 8 | (u8)letter));
        return;
    }
    if (alt) {
        u8 alt_scan = usage >= 0x1e && usage <= 0x27 ? (u8)(0x78 + usage - 0x1e) : scan;
        push_key(-1, (u16)(alt_scan << 8));
        return;
    }
    if (shift) c = usage_shift[usage];
    if (usage == 0x28) { push_key('\n', ctrl ? 0x1c0a : 0x1c0d); return; }
    if (usage == 0x2a) { push_key('\b', ctrl ? 0x0e7f : 0x0e08); return; }
    if (usage == 0x2b) { push_key('\t', shift ? 0x0f00 : 0x0f09); return; }
    if (ctrl && (c == '[' || c == '\\' || c == ']')) c &= 31;
    push_key((u8)c, (u16)(scan << 8 | (u8)c));
}

static void keyboard_leds(struct usb_device *d, struct keyboard *k) {
    u8 leds = (u8)((num_lock ? 1 : 0) | (caps_lock ? 2 : 0) | (scroll_lock ? 4 : 0));
    if (leds == k->leds) return;
    k->leds = leds;
    usb_control(d, 0x21, 9, 0x0200, d->interface, &leds, 1);
}

static void keyboard_report(struct usb_device *d, struct keyboard *k, const u8 *report, int length) {
    if (length < 3) return;
    if (report[2] == 1) return;                   /* rollover error: keep the old state */
    modifiers = report[0];
    for (int i = 2; i < 8 && i < length; ++i) {
        u8 u = report[i];
        if (u < 4) continue;
        int was = 0;
        for (int j = 2; j < 8; ++j) if (k->previous[j] == u) was = 1;
        if (!was) {
            key_press(u);
            held_usage = u;
            held_since = cpu_nanoseconds();
            held_next = held_since + 500000000ull;
        }
    }
    int still = 0;
    for (int i = 2; i < 8 && i < length; ++i) if (report[i] == held_usage) still = 1;
    if (!still) held_usage = 0;
    memset(k->previous, 0, sizeof(k->previous));
    memcpy(k->previous, report, (uptr)(length < 8 ? length : 8));
    keyboard_leds(d, k);
}

static void key_repeat(void) {
    if (!held_usage || held_usage == 0x39 || held_usage == 0x53 || held_usage == 0x47) return;
    u64 now = cpu_nanoseconds();
    if (now < held_next) return;
    key_press(held_usage);
    held_next = now + 33000000ull;
}

int usb_key_read(void) {
    while (key_head != key_tail) {
        int key = keys[key_head].key;
        key_head = (key_head + 1) % KEY_QUEUE;
        if (key >= 0) return key;
    }
    return -1;
}

int usb_key_bios(void) {
    if (key_head == key_tail) return -1;
    int word = keys[key_head].bios;
    key_head = (key_head + 1) % KEY_QUEUE;
    return word;
}

u8 usb_shift_flags(void) {
    return (u8)((modifiers & 0x20 ? 1 : 0) | (modifiers & 0x02 ? 2 : 0) | (modifiers & 0x11 ? 4 : 0) |
                (modifiers & 0x44 ? 8 : 0) | (scroll_lock ? 16 : 0) | (num_lock ? 32 : 0) | (caps_lock ? 64 : 0));
}

/* ---- mice ------------------------------------------------------------- */

static struct usb_mouse_state mouse;
static int mice, keyboards;
int usb_keyboards(void) { return keyboards; }
int usb_mice(void) { return mice; }
struct mouse { struct usb_pipe pipe; };

static void mouse_report(const u8 *r, int length) {
    if (length < 3) return;
    mouse.buttons = r[0] & 7;
    mouse.dx += (signed char)r[1];
    mouse.dy += (signed char)r[2];
    if (length >= 4) mouse.wheel += (signed char)r[3];
}

int usb_mouse_take(struct usb_mouse_state *out) {
    *out = mouse;
    out->present = mice > 0;
    mouse.dx = mouse.dy = mouse.wheel = 0;
    return mice > 0;
}

/* ---- mass storage (bulk-only transport, SCSI) ------------------------- */

struct storage {
    struct usb_device *device;
    struct usb_endpoint in, out;
    u8 lun;
    u32 tag, block;
    u64 sectors;
    int present, removable, readonly, ejected, index;
    u64 next_check;
    u8 *cbw, *csw, *data;             /* DMA buffers */
};
#define DATA_BUFFER (64u * 512)
static struct storage *disks[USB_DISKS];

static void clear_halt(struct usb_device *d, const struct usb_endpoint *e) {
    usb_control(d, 0x02, 1, 0, e->address, 0, 0);     /* CLEAR_FEATURE(ENDPOINT_HALT) */
    d->hc->ops->reset_endpoint(d, e);
}

static void reset_recovery(struct storage *s) {
    struct usb_device *d = s->device;
    usb_control(d, 0x21, 0xff, 0, d->interface, 0, 0); /* Bulk-Only Mass Storage Reset */
    clear_halt(d, &s->in);
    clear_halt(d, &s->out);
}

/* One SCSI command: 0 good, 1 CHECK CONDITION, < 0 transport failure. */
static int scsi(struct storage *s, const u8 *cdb, int cdb_length, u32 length, int in, u32 timeout) {
    struct usb_device *d = s->device;
    if (d->gone) return USB_ENODEV;
    u8 *w = s->cbw;
    memset(w, 0, 31);
    w[0] = 'U'; w[1] = 'S'; w[2] = 'B'; w[3] = 'C';
    u32 tag = ++s->tag;
    memcpy(w + 4, &tag, 4);
    memcpy(w + 8, &length, 4);
    w[12] = in ? 0x80 : 0;
    w[13] = s->lun;
    w[14] = (u8)cdb_length;
    memcpy(w + 15, cdb, (uptr)cdb_length);
    int r = d->hc->ops->bulk(d, &s->out, w, 31, 2000);
    if (r == USB_ESTALL) { reset_recovery(s); return USB_EIO; }
    if (r < 0) return r;
    if (length) {
        r = d->hc->ops->bulk(d, in ? &s->in : &s->out, s->data, length, timeout);
        if (r == USB_ESTALL) clear_halt(d, in ? &s->in : &s->out);
        else if (r < 0) { if (r != USB_ENODEV) reset_recovery(s); return r; }
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        r = d->hc->ops->bulk(d, &s->in, s->csw, 13, timeout);
        if (r != USB_ESTALL) break;
        clear_halt(d, &s->in);
    }
    if (r < 0) { if (r != USB_ENODEV) reset_recovery(s); return r; }
    u32 signature, csw_tag;
    memcpy(&signature, s->csw, 4);
    memcpy(&csw_tag, s->csw + 4, 4);
    if (r < 13 || signature != 0x53425355u || csw_tag != tag || s->csw[12] > 1) {
        reset_recovery(s);
        return USB_EIO;
    }
    return s->csw[12];
}

/* TEST UNIT READY with REQUEST SENSE: 1 ready, 0 no medium, < 0 error. */
static int unit_ready(struct storage *s) {
    for (int attempt = 0; attempt < 6; ++attempt) {
        u8 tur[6] = {0};
        int r = scsi(s, tur, 6, 0, 0, 2000);
        if (r < 0) return r;
        if (r == 0) return 1;
        u8 sense[6] = {0x03, 0, 0, 0, 18, 0};
        r = scsi(s, sense, 6, 18, 1, 2000);
        if (r < 0) return r;
        u8 key = s->data[2] & 15, asc = s->data[12];
        if (key == 2 && asc == 0x3a) return 0;            /* MEDIUM NOT PRESENT */
        if (key != 6 && key != 2) return 0;               /* not UNIT ATTENTION / becoming ready */
        cpu_sleep(100000000);
    }
    return 0;
}

static int read_capacity(struct storage *s) {
    u8 cdb[10] = {0x25};
    if (scsi(s, cdb, 10, 8, 1, 2000)) return -1;
    const u8 *b = s->data;
    u32 last = (u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3];
    s->block = (u32)b[4] << 24 | (u32)b[5] << 16 | (u32)b[6] << 8 | b[7];
    s->sectors = (u64)last + 1;
    if (last == 0xffffffffu) {
        u8 cdb16[16] = {0x9e, 0x10};
        cdb16[13] = 32;
        if (scsi(s, cdb16, 16, 32, 1, 2000)) return -1;
        u64 l = 0;
        for (int i = 0; i < 8; ++i) l = l << 8 | b[i];
        s->sectors = l + 1;
        s->block = (u32)b[8] << 24 | (u32)b[9] << 16 | (u32)b[10] << 8 | b[11];
    }
    if (s->block != 512 || !s->sectors) return -1;
    /* MODE SENSE(6): write protect. */
    u8 sense[6] = {0x1a, 0, 0x3f, 0, 192, 0};
    if (scsi(s, sense, 6, 192, 1, 2000) == 0) s->readonly = (s->data[2] & 0x80) != 0;
    return 0;
}

static void storage_check(struct storage *s) {
    int ready = unit_ready(s);
    int present = ready > 0 && !read_capacity(s);
    if (present == s->present) return;
    s->present = present;
    volumes_usb_changed(s->index, present);
}

static int storage_attach(struct usb_device *d, const struct usb_endpoint *in, const struct usb_endpoint *out) {
    int index = -1;
    for (int i = 0; i < USB_DISKS; ++i) if (!disks[i]) { index = i; break; }
    if (index < 0) return -1;
    struct storage *s = memory_alloc(sizeof(*s), 16, OWNER_KERNEL, 1);
    if (!s) return -1;
    s->device = d;
    s->in = *in;
    s->out = *out;
    s->index = index;
    s->cbw = dma_alloc(64, 64);
    s->csw = dma_alloc(64, 64);
    s->data = dma_alloc(DATA_BUFFER, 4096);
    if (!s->cbw || !s->csw || !s->data) return -1;
    u8 lun = 0;
    if (usb_control(d, 0xa1, 0xfe, 0, d->interface, &lun, 1) < 0) lun = 0;   /* GET MAX LUN (may stall) */
    s->lun = 0;
    u8 inquiry[6] = {0x12, 0, 0, 0, 36, 0};
    if (scsi(s, inquiry, 6, 36, 1, 2000) < 0) return -1;
    if ((s->data[0] & 0x1f) != 0) return -1;              /* direct-access block devices only */
    s->removable = (s->data[1] & 0x80) != 0;
    d->class_data = s;
    disks[index] = s;
    storage_check(s);
    s->next_check = cpu_nanoseconds() + 2000000000ull;
    return 0;
}

static void storage_detach(struct storage *s) {
    disks[s->index] = 0;
    if (s->present) volumes_usb_changed(s->index, 0);
    dma_free(s->cbw, 64);
    dma_free(s->csw, 64);
    dma_free(s->data, DATA_BUFFER);
    memory_free(s, OWNER_KERNEL);
}

int usb_disk_info(int disk, struct usb_disk_info *out) {
    if (disk < 0 || disk >= USB_DISKS || !disks[disk]) return -1;
    struct storage *s = disks[disk];
    out->sectors = s->sectors;
    out->present = s->present && !s->ejected;
    out->removable = s->removable;
    out->readonly = s->readonly;
    out->serial = (u32)s->device->vendor << 16 | s->device->product;
    return 0;
}

int usb_disk_io(int disk, u64 lba, unsigned count, void *buffer, int write) {
    if (disk < 0 || disk >= USB_DISKS || !disks[disk]) return -1;
    struct storage *s = disks[disk];
    if (!s->present || s->ejected) return -1;
    if (write && s->readonly) return -1;
    int was_busy = busy;
    busy = 1;                           /* keeps the real-mode bridge out */
    int result = 0;
    u8 *bytes = buffer;
    while (count && !result) {
        unsigned n = count > DATA_BUFFER / 512 ? DATA_BUFFER / 512 : count;
        u8 cdb[16] = {0};
        int length;
        if (lba + n > 0xffffffffull) {
            cdb[0] = write ? 0x8a : 0x88;
            for (int i = 0; i < 8; ++i) cdb[2 + i] = (u8)(lba >> (56 - 8 * i));
            cdb[10] = (u8)(n >> 24); cdb[11] = (u8)(n >> 16); cdb[12] = (u8)(n >> 8); cdb[13] = (u8)n;
            length = 16;
        } else {
            cdb[0] = write ? 0x2a : 0x28;
            cdb[2] = (u8)(lba >> 24); cdb[3] = (u8)(lba >> 16); cdb[4] = (u8)(lba >> 8); cdb[5] = (u8)lba;
            cdb[7] = (u8)(n >> 8); cdb[8] = (u8)n;
            length = 10;
        }
        if (write) memcpy(s->data, bytes, (uptr)n * 512);
        int r = -1;
        for (int attempt = 0; attempt < 3 && r; ++attempt) {
            r = scsi(s, cdb, length, n * 512, !write, 10000);
            if (r == USB_ENODEV) break;
            if (r == 1) {                       /* CHECK CONDITION: take the sense data, retry */
                u8 sense[6] = {0x03, 0, 0, 0, 18, 0};
                scsi(s, sense, 6, 18, 1, 2000);
            }
        }
        if (r) { result = -1; break; }
        if (!write) memcpy(bytes, s->data, (uptr)n * 512);
        bytes += n * 512;
        lba += n;
        count -= n;
    }
    busy = was_busy;
    return result;
}

int usb_disk_eject(int disk) {
    if (disk < 0 || disk >= USB_DISKS || !disks[disk]) return -1;
    struct storage *s = disks[disk];
    s->ejected = 1;
    u8 sync[10] = {0x35};                         /* SYNCHRONIZE CACHE(10) */
    scsi(s, sync, 10, 0, 0, 10000);
    if (s->removable) {
        u8 stop[6] = {0x1b, 0, 0, 0, 0x02, 0};    /* START STOP UNIT: eject */
        scsi(s, stop, 6, 0, 0, 5000);
    }
    return 0;
}

/* ---- hubs ------------------------------------------------------------- */

struct hub {
    struct usb_pipe pipe;
    int ports, super, power_wait;
    struct usb_device *child[USB_MAX_PORTS];
    u8 failed[USB_MAX_PORTS];
};

static int enumerate(struct usb_hc *hc, struct usb_device *parent, int port, int speed);
static void remove_device(struct usb_device *d);

static int hub_port_status(struct usb_device *hub, int port, u32 *status) {
    return usb_control(hub, 0xa3, 0, 0, (u16)port, status, 4) == 4 ? 0 : -1;
}

static int hub_port_reset(struct usb_device *d, struct hub *h, int port) {
    u32 status;
    if (usb_control(d, 0x23, 3, 4, (u16)port, 0, 0) < 0) return -1;   /* SET_FEATURE(PORT_RESET) */
    for (int i = 0; i < 50; ++i) {
        cpu_sleep(10000000);
        if (hub_port_status(d, port, &status)) return -1;
        if (!(status & 0x10)) break;
    }
    usb_control(d, 0x23, 1, 20, (u16)port, 0, 0);                      /* C_PORT_RESET */
    if (h->super) usb_control(d, 0x23, 1, 29, (u16)port, 0, 0);       /* C_BH_PORT_RESET */
    if (hub_port_status(d, port, &status) || !(status & 1) || !(status & 2)) return -1;
    cpu_sleep(10000000);
    if (h->super) return USB_SPEED_SUPER;
    return status & 0x200 ? USB_SPEED_LOW : status & 0x400 ? USB_SPEED_HIGH : USB_SPEED_FULL;
}

static void hub_port_changed(struct usb_device *d, struct hub *h, int port) {
    u32 status;
    if (hub_port_status(d, port, &status)) return;
    u16 change = (u16)(status >> 16);
    static const u8 clear[] = {16, 17, 18, 19, 20};
    for (int i = 0; i < 5; ++i)
        if (change & (1 << i)) usb_control(d, 0x23, 1, clear[i], (u16)port, 0, 0);
    if (h->super) {
        if (change & 0x20) usb_control(d, 0x23, 1, 29, (u16)port, 0, 0);
        if (change & 0x40) usb_control(d, 0x23, 1, 25, (u16)port, 0, 0);
        if (change & 0x80) usb_control(d, 0x23, 1, 26, (u16)port, 0, 0);
    }
    int connected = status & 1;
    struct usb_device **child = &h->child[port - 1];
    if (*child && (!connected || (change & 1))) {
        remove_device(*child);
        *child = 0;
    }
    if (!connected) { h->failed[port - 1] = 0; return; }
    if (*child || h->failed[port - 1]) return;
    cpu_sleep(100000000);                         /* debounce */
    if (hub_port_status(d, port, &status) || !(status & 1)) return;
    int speed = hub_port_reset(d, h, port);
    if (speed < 0 || enumerate(d->hc, d, port, speed) < 0) h->failed[port - 1] = 1;
}

static int hub_attach(struct usb_device *d, const struct usb_endpoint *ep, int protocol) {
    struct hub *h = memory_alloc(sizeof(*h), 16, OWNER_KERNEL, 1);
    if (!h) return -1;
    d->class_data = h;
    h->super = d->speed == USB_SPEED_SUPER;
    u8 desc[16];
    int r = usb_control(d, 0xa0, 6, (u16)((h->super ? 0x2a : 0x29) << 8), 0, desc, sizeof(desc));
    trace("hub descriptor", (u64)(unsigned)r);
    if (r < 7) return -1;
    h->ports = desc[2] > USB_MAX_PORTS ? USB_MAX_PORTS : desc[2];
    d->hub_ports = (u8)h->ports;
    int think = (desc[3] >> 5) & 3;
    if (d->hc->ops->hub_info && d->hc->ops->hub_info(d, h->ports, think, protocol == 2) < 0) return -1;
    if (h->super) usb_control(d, 0x20, 12, (u16)d->depth, 0, 0, 0);   /* SET_HUB_DEPTH */
    for (int p = 1; p <= h->ports; ++p) usb_control(d, 0x23, 3, 8, (u16)p, 0, 0);   /* PORT_POWER */
    cpu_sleep((u64)(desc[5] * 2 + 20) * 1000000ull);
    h->pipe.device = d;
    h->pipe.endpoint = *ep;
    if (d->hc->ops->interrupt_open(&h->pipe) < 0) { trace("hub pipe failed", 0); return -1; }
    /* Pick up what is already connected. */
    for (int p = 1; p <= h->ports; ++p) hub_port_changed(d, h, p);
    return 0;
}

static void hub_poll(struct usb_device *d, struct hub *h) {
    int r = d->hc->ops->interrupt_poll(&h->pipe);
    if (r <= 0) return;
    u32 bits = 0;
    for (int i = 0; i < r && i < 4; ++i) bits |= (u32)h->pipe.buffer[i] << (8 * i);
    for (int p = 1; p <= h->ports; ++p)
        if (bits & (1u << p)) hub_port_changed(d, h, p);
}

/* ---- enumeration ------------------------------------------------------ */

/* HID functions (up to two per device, e.g. a keyboard + mouse receiver). */
static struct { struct usb_device *device; int kind; void *data; } functions[MAX_DEVICES * 2];

static void add_function(struct usb_device *d, int kind, void *data) {
    for (unsigned i = 0; i < sizeof(functions) / sizeof(*functions); ++i)
        if (!functions[i].device) {
            functions[i].device = d;
            functions[i].kind = kind;
            functions[i].data = data;
            return;
        }
}


static struct usb_device *new_device(void) {
    for (int i = 0; i < MAX_DEVICES; ++i)
        if (!devices[i].used) {
            memset(&devices[i], 0, sizeof(devices[i]));
            devices[i].used = 1;
            return &devices[i];
        }
    return 0;
}

struct interface_choice {
    int kind;
    u8 number, protocol;
    struct usb_endpoint in, out;
};

static void parse_configuration(const u8 *c, int length, struct interface_choice *choices, int *count) {
    int current = -1;
    *count = 0;
    for (int at = 0; at + 2 <= length && c[at] >= 2; at += c[at]) {
        if (c[at + 1] == 4 && c[at] >= 9) {           /* interface */
            current = -1;
            if (c[at + 3] != 0 || *count >= 4) continue;          /* alternate setting 0 only */
            u8 cls = c[at + 5], sub = c[at + 6], proto = c[at + 7];
            int kind = USB_CLASS_NONE;
            if (cls == 9) kind = USB_CLASS_HUB;
            else if (cls == 3 && sub == 1 && proto == 1) kind = USB_CLASS_KEYBOARD;
            else if (cls == 3 && sub == 1 && proto == 2) kind = USB_CLASS_MOUSE;
            else if (cls == 8 && proto == 0x50 && (sub == 6 || sub == 5 || sub == 2)) kind = USB_CLASS_STORAGE;
            if (!kind) continue;
            current = (*count)++;
            memset(&choices[current], 0, sizeof(choices[current]));
            choices[current].kind = kind;
            choices[current].number = c[at + 2];
            choices[current].protocol = proto;
        } else if (c[at + 1] == 5 && c[at] >= 7 && current >= 0) {   /* endpoint */
            struct usb_endpoint e = {c[at + 2], (u8)(c[at + 3] & 3), (u16)(c[at + 4] | c[at + 5] << 8), c[at + 6]};
            struct interface_choice *ch = &choices[current];
            if (e.address & 0x80) { if (!ch->in.address) ch->in = e; }
            else if (!ch->out.address) ch->out = e;
        }
    }
}

/* A dead USB keyboard leaves nobody able to type a diagnostic command, so
   say what happened before the prompt. */
static void device_notice(struct usb_hc *hc, int port, const char *what, const char *step, int error) {
    char text[80];
    uptr n = 0;
    const char *parts[6] = {"USB ", what, " on ", hc->ops->name, " port ", 0};
    for (int i = 0; i < 5; ++i)
        for (const char *p = parts[i]; *p && n < 60; ++p) text[n++] = *p;
    text[n++] = (char)('0' + port / 10 % 10);
    if (text[n - 1] == '0') --n;
    text[n++] = (char)('0' + port % 10);
    if (step) {
        const char *middle = " failed at ";
        for (const char *p = middle; *p && n < 72; ++p) text[n++] = *p;
        for (const char *p = step; *p && n < 72; ++p) text[n++] = *p;
        if (error < 0 && n < 76) { text[n++] = ' '; text[n++] = '-'; text[n++] = (char)('0' - error % 10); }
    }
    text[n] = 0;
    volume_post_notice(text);
}

static int enumerate(struct usb_hc *hc, struct usb_device *parent, int port, int speed) {
    const char *step = "open";
    int error = 0;
    trace("enumerate port/speed", (u64)port << 8 | (u64)speed);
    if (parent && parent->depth >= MAX_DEPTH_TREE) return -1;
    struct usb_device *d = new_device();
    if (!d) return -1;
    d->hc = hc;
    d->parent = parent;
    d->port = port;
    d->speed = speed;
    d->depth = parent ? parent->depth + 1 : 0;
    if (parent) {
        d->route = parent->route | (u32)(port > 15 ? 15 : port) << (4 * (parent->depth));
        if (speed <= USB_SPEED_FULL) {
            if (parent->speed == USB_SPEED_HIGH) { d->tt_hub = parent; d->tt_port = port; }
            else { d->tt_hub = parent->tt_hub; d->tt_port = parent->tt_port; }
        }
        ((struct hub *)parent->class_data)->child[port - 1] = d;
    } else {
        hc->root[port - 1] = d;
    }
    d->max_packet0 = speed == USB_SPEED_SUPER ? 512 : speed == USB_SPEED_HIGH ? 64 : 8;
    u8 desc[18];
    int address;
    if (hc->ops->open(d) < 0) { trace("open failed", 0); goto fail; }
    step = "first descriptor";
    { int r8 = get_descriptor(d, 1, 0, desc, 8); trace("descriptor8", (u64)(unsigned)r8); error = r8; if (r8 < 8) goto fail; }
    u16 mps = speed == USB_SPEED_SUPER ? (u16)(1u << desc[7]) : desc[7];
    if (mps != d->max_packet0 && mps >= 8) {
        d->max_packet0 = mps;
        if (hc->ops->update_control(d) < 0) goto fail;
    }
    step = "address";
    address = new_address(hc);
    if (address < 0 || hc->ops->address(d, address) < 0) { free_address(hc, address); goto fail; }
    d->address = address;
    cpu_sleep(5000000);
    step = "device descriptor";
    if ((error = get_descriptor(d, 1, 0, desc, 18)) < 18) goto fail;
    d->vendor = (u16)(desc[8] | desc[9] << 8);
    d->product = (u16)(desc[10] | desc[11] << 8);
    int device_protocol = desc[6];
    u8 head[9];
    step = "configuration";
    if ((error = get_descriptor(d, 2, 0, head, 9)) < 9) goto fail;
    int total = head[2] | head[3] << 8;
    if (total > 1024) total = 1024;
    static u8 config[1024];
    if ((error = get_descriptor(d, 2, 0, config, (u16)total)) < total) goto fail;
    struct interface_choice choices[4];
    int count;
    parse_configuration(config, total, choices, &count);
    trace("config interfaces", (u64)count);
    if (usb_control(d, 0x00, 9, config[5], 0, 0, 0) < 0) { trace("set config failed", 0); goto fail; }
    struct usb_endpoint eps[8];
    int ep_count = 0;
    for (int i = 0; i < count; ++i) {
        if (choices[i].in.address && ep_count < 8) eps[ep_count++] = choices[i].in;
        if (choices[i].kind == USB_CLASS_STORAGE && choices[i].out.address && ep_count < 8) eps[ep_count++] = choices[i].out;
    }
    if (ep_count && hc->ops->configure(d, eps, ep_count) < 0) goto fail;
    /* Class drivers: one storage or hub function, or keyboard and mouse. */
    int used = 0;
    for (int i = 0; i < count; ++i) {
        struct interface_choice *ch = &choices[i];
        d->interface = ch->number;
        if (ch->kind == USB_CLASS_HUB && ch->in.address && !used) {
            d->kind = USB_CLASS_HUB;
            if (hub_attach(d, &ch->in, device_protocol) < 0) goto fail;
            used = 1;
            break;
        }
        if (ch->kind == USB_CLASS_STORAGE && ch->in.address && ch->out.address && !used) {
            d->kind = USB_CLASS_STORAGE;
            if (storage_attach(d, &ch->in, &ch->out) < 0) goto fail;
            used = 1;
            break;
        }
        if ((ch->kind == USB_CLASS_KEYBOARD || ch->kind == USB_CLASS_MOUSE) && ch->in.address) {
            usb_control(d, 0x21, 0x0b, 0, ch->number, 0, 0);          /* SET_PROTOCOL(boot) */
            usb_control(d, 0x21, 0x0a, 0, ch->number, 0, 0);          /* SET_IDLE(0) */
            struct usb_pipe *pipe;
            if (ch->kind == USB_CLASS_KEYBOARD) {
                struct keyboard *k = memory_alloc(sizeof(*k), 16, OWNER_KERNEL, 1);
                if (!k) goto fail;
                k->leds = 0xff;
                pipe = &k->pipe;
            } else {
                struct mouse *m = memory_alloc(sizeof(*m), 16, OWNER_KERNEL, 1);
                if (!m) goto fail;
                pipe = &m->pipe;
            }
            pipe->device = d;
            pipe->endpoint = ch->in;
            step = "input pipe";
            if (hc->ops->interrupt_open(pipe) < 0) goto fail;
            if (!d->kind) d->kind = ch->kind;
            if (ch->kind == USB_CLASS_KEYBOARD) {
                ++keyboards;
                keyboard_leds(d, (struct keyboard *)pipe);
                device_notice(hc, port, "keyboard ready", 0, 0);
            }
            else ++mice;
            add_function(d, ch->kind, pipe);
            used = 1;
        }
    }
    trace("device ready kind", (u64)d->kind);
    return 0;
fail:
    trace("enumerate failed", 0);
    device_notice(hc, port, "device", step, error);
    remove_device(d);
    return -1;
}

static void remove_device(struct usb_device *d) {
    if (!d || !d->used) return;
    d->gone = 1;
    if (d->kind == USB_CLASS_HUB && d->class_data) {
        struct hub *h = d->class_data;
        for (int p = 0; p < USB_MAX_PORTS; ++p)
            if (h->child[p]) { remove_device(h->child[p]); h->child[p] = 0; }
        if (h->pipe.hc_data) d->hc->ops->interrupt_close(&h->pipe);
        memory_free(h, OWNER_KERNEL);
    } else if (d->kind == USB_CLASS_STORAGE && d->class_data) {
        storage_detach(d->class_data);
    }
    for (unsigned i = 0; i < sizeof(functions) / sizeof(*functions); ++i) {
        if (functions[i].device != d) continue;
        struct usb_pipe *pipe = functions[i].data;
        if (pipe->hc_data) d->hc->ops->interrupt_close(pipe);
        if (functions[i].kind == USB_CLASS_MOUSE && mice) --mice;
        if (functions[i].kind == USB_CLASS_KEYBOARD) { held_usage = 0; if (keyboards) --keyboards; }
        memory_free(pipe, OWNER_KERNEL);
        functions[i].device = 0;
    }
    d->class_data = 0;
    d->hc->ops->close(d);
    free_address(d->hc, d->address);
    if (d->parent) {
        struct hub *h = d->parent->class_data;
        if (h && d->port >= 1 && d->port <= USB_MAX_PORTS && h->child[d->port - 1] == d) h->child[d->port - 1] = 0;
    } else if (d->port >= 1 && d->port <= USB_MAX_PORTS && d->hc->root[d->port - 1] == d) {
        d->hc->root[d->port - 1] = 0;
    }
    d->used = 0;
}

/* ---- polling ---------------------------------------------------------- */

static u8 root_failed[MAX_HCS][USB_MAX_PORTS];

static void root_ports(struct usb_hc *hc, int index) {
    for (int p = 1; p <= hc->ports; ++p) {
        int status = hc->ops->port_status(hc, p);
        int connected = status & USB_PORT_CONNECTED;
        struct usb_device *d = hc->root[p - 1];
        if (d && (!connected || (status & USB_PORT_CHANGED))) {
            remove_device(d);
            d = 0;
        }
        if (!connected) { root_failed[index][p - 1] = 0; continue; }
        if (d || root_failed[index][p - 1]) continue;
        cpu_sleep(100000000);                     /* debounce */
        if (!(hc->ops->port_status(hc, p) & USB_PORT_CONNECTED)) continue;
        int speed = hc->ops->port_reset(hc, p);
        if (speed == 0) continue;                 /* handed to a companion controller */
        if (speed < 0 || enumerate(hc, 0, p, speed) < 0) root_failed[index][p - 1] = 1;
    }
}

void usb_poll(void) {
    if (!started || busy) return;
    busy = 1;
    for (int i = 0; i < MAX_HCS; ++i)
        if (hcs[i].used && hcs[i].ops->poll) hcs[i].ops->poll(&hcs[i]);
    u64 now = cpu_nanoseconds();
    if (now - last_ports >= 50000000ull) {
        last_ports = now;
        for (int i = 0; i < MAX_HCS; ++i) if (hcs[i].used) root_ports(&hcs[i], i);
    }
    for (int i = 0; i < MAX_DEVICES; ++i) {
        struct usb_device *d = &devices[i];
        if (d->used && !d->gone && d->kind == USB_CLASS_HUB && d->class_data) hub_poll(d, d->class_data);
    }
    for (unsigned i = 0; i < sizeof(functions) / sizeof(*functions); ++i) {
        struct usb_device *d = functions[i].device;
        if (!d || d->gone) continue;
        struct usb_pipe *pipe = functions[i].data;
        int r = d->hc->ops->interrupt_poll(pipe);
        if (r <= 0) continue;
        if (functions[i].kind == USB_CLASS_KEYBOARD) keyboard_report(d, functions[i].data, pipe->buffer, r);
        else mouse_report(pipe->buffer, r);
    }
    key_repeat();
    for (int i = 0; i < USB_DISKS; ++i) {
        struct storage *s = disks[i];
        if (!s || !s->removable || s->ejected || now < s->next_check) continue;
        s->next_check = now + 2000000000ull;
        storage_check(s);
    }
    busy = 0;
}

void usb_tick(void) {
    if (!started) return;
    u64 now = cpu_nanoseconds();
    if (now - last_tick < 4000000ull) return;
    last_tick = now;
    usb_poll();
}

static void found_controller(const struct pci_function *f) {
    switch (f->prog_if) {
    case 0x00: uhci_probe(f); break;
    case 0x10: ohci_probe(f); break;
    case 0x20: ehci_probe(f); break;
    case 0x30: xhci_probe(f); break;
    }
}

static const struct pci_function *pending[MAX_HCS];
static struct pci_function pending_store[MAX_HCS];
static int pending_count;
static void collect(const struct pci_function *f) {
    if (pending_count < MAX_HCS) {
        pending_store[pending_count] = *f;
        pending[pending_count] = &pending_store[pending_count];
        ++pending_count;
    }
}

/* Take over the controllers from the BIOS.  Every firmware hand-off comes
   first, because the firmware's handler may stop the other controllers as
   it shuts its USB support down.  The companion controllers (UHCI/OHCI)
   then start before EHCI, which then hands them its full- and
   low-speed devices; xHCI last. */
void usb_init(void) {
    if (!ctx->bios_available || started) return;
    control_buffer = dma_alloc(4096, 4096);
    if (!control_buffer) return;
    num_lock = (*(volatile u8 *)0x417 & 0x20) != 0;
    pci_scan_class(0x0c, 0x03, collect);
    for (int i = 0; i < pending_count; ++i) {
        if (pending[i]->prog_if == 0x10) ohci_handoff(pending[i]);
        else if (pending[i]->prog_if == 0x20) ehci_handoff(pending[i]);
        else if (pending[i]->prog_if == 0x30) xhci_handoff(pending[i]);
    }
    static const u8 order[] = {0x00, 0x10, 0x20, 0x30};
    for (int o = 0; o < 4; ++o)
        for (int i = 0; i < pending_count; ++i)
            if (pending[i]->prog_if == order[o]) found_controller(pending[i]);
    int any = 0;
    for (int i = 0; i < MAX_HCS; ++i) any |= hcs[i].used;
    trace("controllers found", (u64)pending_count);
    if (!any) return;
    started = 1;
    bridge_install();
    /* Enumerate what is plugged in now (port debounce included). */
    last_ports = 0;
    busy = 1;
    for (int i = 0; i < MAX_HCS; ++i) if (hcs[i].used && hcs[i].ops->poll) hcs[i].ops->poll(&hcs[i]);
    for (int pass = 0; pass < 2; ++pass)          /* EHCI hands ports to companions on the first pass */
        for (int i = 0; i < MAX_HCS; ++i) if (hcs[i].used) root_ports(&hcs[i], i);
    busy = 0;
    last_ports = cpu_nanoseconds();
}

void usb_shutdown(void) {
    if (!started) return;
    for (int i = 0; i < MAX_HCS; ++i)
        if (hcs[i].used && hcs[i].ops->shutdown) hcs[i].ops->shutdown(&hcs[i]);
    started = 0;
}

int usb_busy(void) { return busy; }
