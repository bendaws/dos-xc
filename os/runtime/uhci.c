/*
 * DOS-XC /os/runtime/uhci.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "usb.h"

struct td { volatile u32 link, status, token, buffer; u32 pad[4]; };
struct qh { volatile u32 head, element; u32 pad[2]; };

#define T 1u
#define Q 2u
#define VF 4u
#define TD_ACTIVE (1u << 23)
#define TD_STALLED (1u << 22)
#define TD_ERRORS (0x76u << 16)
#define PID_SETUP 0x2du
#define PID_IN 0x69u
#define PID_OUT 0xe1u

struct udev { struct qh *control; struct qh *endpoints[USB_ENDPOINTS]; };
struct upipe { struct qh *qh; struct td *td; };

struct uhci {
    u16 io, command;
    int ports;
    u32 *frames;
    struct qh *interrupt_anchor, *control_anchor, *bulk_anchor;
};

static inline void outw(u16 port, u16 v) { __asm__ volatile("outw %0,%1" ::"a"(v), "Nd"(port)); }
static inline u16 inw(u16 port) { u16 v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(port)); return v; }
static inline void outl(u16 port, u32 v) { __asm__ volatile("outl %0,%1" ::"a"(v), "Nd"(port)); }

static struct qh *new_qh(void) {
    struct qh *q = dma_alloc(sizeof(struct qh), 16);
    if (q) { q->head = T; q->element = T; }
    return q;
}

static void link_qh(struct qh *anchor, struct qh *q) {
    q->head = anchor->head;
    __asm__ volatile("" ::: "memory");
    anchor->head = dma_address(q) | Q;
}

/* Anchors are linked anchor -> its endpoint QHs -> next anchor, so an
   endpoint QH is inserted right after its anchor and removed by walking. */
static void unlink_qh(struct qh *anchor, struct qh *q) {
    struct qh *p = anchor;
    for (int guard = 0; guard < 256; ++guard) {
        if (p->head & T) return;
        struct qh *n = (struct qh *)(uptr)(p->head & ~15u);
        if (n == q) { p->head = q->head; break; }
        p = n;
    }
    cpu_sleep(2000000);
}

static u32 token(struct usb_device *d, u32 pid, int endpoint, int toggle, u32 length) {
    return pid | (u32)(d->address & 0x7f) << 8 | (u32)endpoint << 15 | (u32)toggle << 19 |
           ((length ? length - 1 : 0x7ffu) & 0x7ff) << 21;
}

static u32 status_bits(struct usb_device *d, int spd) {
    return TD_ACTIVE | 3u << 27 | (d->speed == USB_SPEED_LOW ? 1u << 26 : 0) | (spd ? 1u << 29 : 0);
}

static int actual(const struct td *t) {
    u32 n = (t->status + 1) & 0x7ff;
    return (int)n;
}

/* Run TDS[0..count) on Q.  Returns the bytes moved by the data TDs
   [DATA_FIRST, DATA_END) or an error; *LAST_DONE is the last TD that
   completed (for the data toggles). */
static int run(struct qh *q, struct td **tds, int count, int data_first, int data_end, u32 timeout_ms, int *last_done) {
    for (int i = 0; i < count; ++i) tds[i]->link = i + 1 < count ? dma_address(tds[i + 1]) | VF : T;
    __asm__ volatile("" ::: "memory");
    q->element = dma_address(tds[0]);
    u64 end = cpu_nanoseconds() + (u64)timeout_ms * 1000000ull;
    int result = 0, done = -1;
    for (;;) {
        int i, finished = 0;
        for (i = 0; i < count; ++i) {
            u32 st = tds[i]->status;
            if (st & TD_ACTIVE) break;
            if (st & TD_ERRORS) {
                result = st & TD_STALLED ? USB_ESTALL : st & (1u << 20) ? USB_EBABBLE : USB_EIO;
                finished = 1;
                break;
            }
            u32 max = ((tds[i]->token >> 21) + 1) & 0x7ff;
            if ((tds[i]->token & 0xff) == PID_IN && i >= data_first && i < data_end && (u32)actual(tds[i]) < max) {
                ++i;
                finished = 1;
                break;
            }
        }
        done = i - 1;
        if (finished || i == count) break;
        if (cpu_nanoseconds() > end) { result = USB_ETIMEOUT; break; }
    }
    q->element = T;
    cpu_sleep(1000000);
    if (last_done) *last_done = done;
    if (result) return result;
    int moved = 0;
    for (int i = data_first; i < data_end && i <= done; ++i) moved += actual(tds[i]);
    return moved;
}

static struct td *new_td(void) { return dma_alloc(sizeof(struct td), 16); }

/* Firmware can stop a controller behind our back: start it again. */
static void keep_running(struct uhci *u) {
    if (inw(u->io) & 1) return;
    outl((u16)(u->io + 8), dma_address(u->frames));
    outw((u16)(u->io + 6), 0);
    outw((u16)(u->io + 4), 0);
    outw((u16)(u->io + 2), 0x3f);
    outw(u->io, 0xc1);
}

/* What the controller was doing, for the notice before the prompt. */
static void report_timeout(struct uhci *u, const struct td *first) {
    static int reports;
    if (reports >= 2) return;
    ++reports;
    char text[80];
    uptr n = 0;
    struct { const char *label; u32 value; int digits; } fields[] = {
        {"UHCI timeout cmd ", inw(u->io), 4}, {" sts ", inw((u16)(u->io + 2)), 4},
        {" frm ", inw((u16)(u->io + 6)), 4}, {" td ", first->status, 8}, {" pci ", u->command, 4},
    };
    for (unsigned f = 0; f < sizeof(fields) / sizeof(*fields); ++f) {
        for (const char *p = fields[f].label; *p; ++p) text[n++] = *p;
        for (int d = fields[f].digits - 1; d >= 0; --d) text[n++] = "0123456789abcdef"[(fields[f].value >> (4 * d)) & 15];
    }
    text[n] = 0;
    volume_post_notice(text);
}

static int uhci_control(struct usb_device *d, const struct usb_setup *setup, void *data) {
    struct udev *x = d->hc_data;
    if (!x || d->gone) return USB_ENODEV;
    keep_running(d->hc->data);
    static u8 *setup_buffer;
    if (!setup_buffer && !(setup_buffer = dma_alloc(8, 16))) return USB_ENOMEM;
    memcpy(setup_buffer, setup, 8);
    int in = setup->type & 0x80;
    u32 mps = d->max_packet0 ? d->max_packet0 : 8;
    int packets = (int)((setup->length + mps - 1) / mps);
    int count = packets + 2;
    if (count > 66) return USB_EIO;
    struct td *tds[66];
    for (int i = 0; i < count; ++i)
        if (!(tds[i] = new_td())) { for (int j = 0; j < i; ++j) dma_free(tds[j], sizeof(struct td)); return USB_ENOMEM; }
    tds[0]->status = status_bits(d, 0);
    tds[0]->token = token(d, PID_SETUP, 0, 0, 8);
    tds[0]->buffer = dma_address(setup_buffer);
    u32 address = dma_address(data), left = setup->length;
    for (int i = 0; i < packets; ++i) {
        u32 n = left < mps ? left : mps;
        tds[1 + i]->status = status_bits(d, in);
        tds[1 + i]->token = token(d, in ? PID_IN : PID_OUT, 0, (i + 1) & 1, n);
        tds[1 + i]->buffer = address;
        address += n;
        left -= n;
    }
    struct td *status = tds[count - 1];
    status->status = status_bits(d, 0) | 1u << 24;
    status->token = token(d, setup->length && in ? PID_OUT : PID_IN, 0, 1, 0);
    int r = run(x->control, tds, count, 1, 1 + packets, 2000, 0);
    if (r == USB_ETIMEOUT) report_timeout(d->hc->data, tds[0]);
    if (r >= 0 && in && packets) {
        if (status->status & TD_ACTIVE) {
            status->status = status_bits(d, 0);
            struct td *only[1] = {status};
            int s = run(x->control, only, 1, 0, 0, 2000, 0);
            if (s < 0) r = s;
        }
    }
    for (int i = 0; i < count; ++i) dma_free(tds[i], sizeof(struct td));
    return r;
}

static int uhci_open(struct usb_device *d) {
    struct uhci *u = d->hc->data;
    struct udev *x = memory_alloc(sizeof(*x), 16, OWNER_KERNEL, 1);
    if (!x) return -1;
    d->hc_data = x;
    x->control = new_qh();
    if (!x->control) return -1;
    link_qh(u->control_anchor, x->control);
    return 0;
}

static int uhci_address(struct usb_device *d, int address) {
    if (usb_control(d, 0x00, 5, (u16)address, 0, 0, 0) < 0) return -1;
    cpu_sleep(2000000);
    d->address = address;
    return 0;
}

static int uhci_update_control(struct usb_device *d) { (void)d; return 0; }

static int uhci_configure(struct usb_device *d, const struct usb_endpoint *eps, int count) {
    struct uhci *u = d->hc->data;
    struct udev *x = d->hc_data;
    for (int i = 0; i < count; ++i) {
        if ((eps[i].attributes & 3) != 2) continue;
        int index = usb_endpoint_index(eps[i].address);
        if (x->endpoints[index]) continue;
        struct qh *q = new_qh();
        if (!q) return -1;
        x->endpoints[index] = q;
        link_qh(u->bulk_anchor, q);
        d->toggle[index] = 0;
    }
    return 0;
}

static int uhci_bulk(struct usb_device *d, const struct usb_endpoint *ep, void *data, u32 length, u32 timeout) {
    struct udev *x = d->hc_data;
    if (!x || d->gone) return USB_ENODEV;
    int index = usb_endpoint_index(ep->address);
    struct qh *q = x->endpoints[index];
    if (!q) return USB_EIO;
    u32 mps = ep->max_packet ? ep->max_packet : 64;
    int in = ep->address & 0x80;
    int result = 0;
    u32 address = dma_address(data), left = length, total = 0;
    static struct td *tds[256];
    do {
        int count = 0;
        u32 start = address;
        while (count < 256 && (left || (!length && !count))) {
            u32 n = left < mps ? left : mps;
            struct td *t = tds[count] ? tds[count] : (tds[count] = new_td());
            if (!t) return USB_ENOMEM;
            t->status = status_bits(d, in);
            t->token = token(d, in ? PID_IN : PID_OUT, ep->address & 15, (d->toggle[index] + count) & 1, n);
            t->buffer = address;
            address += n;
            left -= n;
            ++count;
            if (!n) break;
        }
        int done = -1;
        int r = run(q, tds, count, 0, count, timeout, &done);
        d->toggle[index] = (u8)((d->toggle[index] + done + 1) & 1);
        if (r < 0) { result = r; break; }
        total += (u32)r;
        if ((u32)r < address - start) break;
    } while (left);
    return result ? result : (int)total;
}

static int uhci_reset_endpoint(struct usb_device *d, const struct usb_endpoint *ep) {
    d->toggle[usb_endpoint_index(ep->address)] = 0;
    return 0;
}

static void queue_interrupt(struct usb_pipe *pipe, struct upipe *p) {
    struct usb_device *d = pipe->device;
    int index = usb_endpoint_index(pipe->endpoint.address);
    struct td *t = p->td;
    t->link = T;
    t->buffer = dma_address(pipe->buffer);
    t->token = token(d, PID_IN, pipe->endpoint.address & 15, d->toggle[index] & 1, pipe->endpoint.max_packet);
    __asm__ volatile("" ::: "memory");
    t->status = status_bits(d, 0);
    p->qh->element = dma_address(t);
}

static int uhci_interrupt_open(struct usb_pipe *pipe) {
    struct uhci *u = pipe->device->hc->data;
    struct upipe *p = memory_alloc(sizeof(*p), 16, OWNER_KERNEL, 1);
    if (!p) return -1;
    pipe->buffer = dma_alloc(pipe->endpoint.max_packet ? pipe->endpoint.max_packet : 8, 16);
    p->qh = new_qh();
    p->td = new_td();
    if (!pipe->buffer || !p->qh || !p->td) return -1;
    pipe->device->toggle[usb_endpoint_index(pipe->endpoint.address)] = 0;
    pipe->hc_data = p;
    queue_interrupt(pipe, p);
    link_qh(u->interrupt_anchor, p->qh);
    return 0;
}

static int uhci_interrupt_poll(struct usb_pipe *pipe) {
    struct upipe *p = pipe->hc_data;
    if (!p) return USB_EIO;
    keep_running(pipe->device->hc->data);
    u32 s = p->td->status;
    if (s & TD_ACTIVE) return USB_EAGAIN;
    int index = usb_endpoint_index(pipe->endpoint.address);
    int r;
    if (s & TD_ERRORS) r = USB_EIO;
    else { r = actual(p->td); pipe->device->toggle[index] ^= 1; }
    queue_interrupt(pipe, p);
    return r;
}

static void uhci_interrupt_close(struct usb_pipe *pipe) {
    struct uhci *u = pipe->device->hc->data;
    struct upipe *p = pipe->hc_data;
    if (!p) return;
    unlink_qh(u->interrupt_anchor, p->qh);
    dma_free(p->qh, sizeof(struct qh));
    dma_free(p->td, sizeof(struct td));
    dma_free(pipe->buffer, pipe->endpoint.max_packet ? pipe->endpoint.max_packet : 8);
    memory_free(p, OWNER_KERNEL);
    pipe->hc_data = 0;
}

static void uhci_close(struct usb_device *d) {
    struct uhci *u = d->hc->data;
    struct udev *x = d->hc_data;
    if (!x) return;
    if (x->control) { unlink_qh(u->control_anchor, x->control); dma_free(x->control, sizeof(struct qh)); }
    for (int i = 0; i < USB_ENDPOINTS; ++i)
        if (x->endpoints[i]) { unlink_qh(u->bulk_anchor, x->endpoints[i]); dma_free(x->endpoints[i], sizeof(struct qh)); }
    memory_free(x, OWNER_KERNEL);
    d->hc_data = 0;
}

#define PORTSC(u, p) ((u16)((u)->io + 0x10 + 2 * ((p) - 1)))
#define PORT_CLEAR 0x000au

static int uhci_port_count(struct usb_hc *hc) { return ((struct uhci *)hc->data)->ports; }

static int uhci_port_status(struct usb_hc *hc, int port) {
    struct uhci *u = hc->data;
    u16 v = inw(PORTSC(u, port));
    int status = 0;
    if (v & 1) status |= USB_PORT_CONNECTED;
    if (v & 4) status |= USB_PORT_ENABLED;
    if (v & 2) { status |= USB_PORT_CHANGED; outw(PORTSC(u, port), (u16)((v & ~PORT_CLEAR) | 2)); }
    return status | (v & 0x100 ? USB_SPEED_LOW : USB_SPEED_FULL) << 8;
}

static int uhci_port_reset(struct usb_hc *hc, int port) {
    struct uhci *u = hc->data;
    u16 v = inw(PORTSC(u, port));
    outw(PORTSC(u, port), (u16)((v & ~PORT_CLEAR) | 0x200));
    cpu_sleep(50000000);
    v = inw(PORTSC(u, port));
    outw(PORTSC(u, port), (u16)(v & ~(PORT_CLEAR | 0x200)));
    cpu_sleep(1000000);
    for (int i = 0; i < 10; ++i) {
        v = inw(PORTSC(u, port));
        if (!(v & 1)) return -1;
        if (v & 4) break;
        outw(PORTSC(u, port), (u16)((v & ~PORT_CLEAR) | 4 | (v & PORT_CLEAR)));
        cpu_sleep(10000000);
    }
    v = inw(PORTSC(u, port));
    outw(PORTSC(u, port), (u16)(v | PORT_CLEAR));
    if (!(v & 4)) return -1;
    cpu_sleep(10000000);
    return v & 0x100 ? USB_SPEED_LOW : USB_SPEED_FULL;
}

static void uhci_port_disable(struct usb_hc *hc, int port) {
    struct uhci *u = hc->data;
    u16 v = inw(PORTSC(u, port));
    outw(PORTSC(u, port), (u16)(v & ~(PORT_CLEAR | 4)));
}

static void uhci_shutdown(struct usb_hc *hc) {
    struct uhci *u = hc->data;
    outw(u->io, 0);
    cpu_sleep(2000000);
    outw(u->io, 2);
}

static const struct usb_hc_ops ops = {
    "UHCI", uhci_port_count, uhci_port_status, uhci_port_reset, uhci_port_disable,
    uhci_open, uhci_address, uhci_update_control, uhci_control, uhci_configure, uhci_bulk,
    uhci_reset_endpoint, uhci_interrupt_open, uhci_interrupt_poll, uhci_interrupt_close,
    0, uhci_close, 0, uhci_shutdown,
};

void uhci_probe(const struct pci_function *f) {
    int io;
    u64 bar = pci_bar(f, 4, &io);
    if (!bar || !io || bar > 0xffff) return;
    pci_write16(f, 0xc0, 0x8f00);                   /* legacy support (SMI) off */
    pci_enable(f, 0, 1);
    struct uhci *u = memory_alloc(sizeof(*u), 16, OWNER_KERNEL, 1);
    if (!u) return;
    u->io = (u16)bar;
    u->command = pci_read16(f, 4);
    outw(u->io, 0);
    for (int i = 0; i < 100 && !(inw((u16)(u->io + 2)) & 0x20); ++i) cpu_sleep(100000);
    outw(u->io, 4);
    cpu_sleep(20000000);
    outw(u->io, 0);
    outw(u->io, 2);
    for (int i = 0; i < 100 && (inw(u->io) & 2); ++i) cpu_sleep(100000);
    outw((u16)(u->io + 4), 0);
    u->frames = dma_alloc(4096, 4096);
    u->interrupt_anchor = new_qh();
    u->control_anchor = new_qh();
    u->bulk_anchor = new_qh();
    if (!u->frames || !u->interrupt_anchor || !u->control_anchor || !u->bulk_anchor) return;
    u->interrupt_anchor->head = dma_address(u->control_anchor) | Q;
    u->control_anchor->head = dma_address(u->bulk_anchor) | Q;
    u->bulk_anchor->head = T;
    for (int i = 0; i < 1024; ++i) u->frames[i] = dma_address(u->interrupt_anchor) | Q;
    outl((u16)(u->io + 8), dma_address(u->frames));
    outw((u16)(u->io + 6), 0);
    outw((u16)(u->io + 2), 0x3f);                    /* clear status */
    outw(u->io, 0xc1);                               /* run, configured, 64-byte max packet */
    u->ports = 0;
    for (int p = 1; p <= 8; ++p) {
        u16 v = inw(PORTSC(u, p));
        if (!(v & 0x80) || v == 0xffff) break;
        u->ports = p;
    }
    if (!u->ports) u->ports = 2;
    usb_add_hc(&ops, u, u->ports);
}
