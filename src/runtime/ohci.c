/*
 * DOS-XC /runtime/ohci.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * OHCI (USB 1.1) host controller driver, polled.  Each endpoint has an
 * endpoint descriptor (ED) on the control, bulk or interrupt list; every
 * ED's queue ends with a dummy TD (its tail).  Completion is seen when the
 * controller has moved the ED's head up to the tail.
 */

#include "usb.h"

struct ed { volatile u32 flags, tail, head, next; };
struct td { volatile u32 flags, buffer, next, end; u32 start, length, pad[2]; };

#define ED_SKIP (1u << 14)
#define ED_HALTED 1u

struct odev { struct ed *control; struct ed *endpoints[USB_ENDPOINTS]; };
struct opipe { struct ed *ed; struct td *td; };

struct ohci {
    volatile u8 *regs;
    int ports;
    u32 *hcca;
    struct ed *control_head, *bulk_head, *interrupt_head;
};

static u32 r32(struct ohci *o, u32 off) { return *(volatile u32 *)(o->regs + off); }
static void w32(struct ohci *o, u32 off, u32 v) { *(volatile u32 *)(o->regs + off) = v; }

static struct ed *new_ed(void) { return dma_alloc(sizeof(struct ed), 16); }
static struct td *new_td(void) { return dma_alloc(sizeof(struct td), 32); }

static u32 ed_flags(struct usb_device *d, int endpoint, int direction, u32 max_packet) {
    return (u32)(d->address & 0x7f) | (u32)endpoint << 7 | (u32)direction << 11 |
           (d->speed == USB_SPEED_LOW ? 1u << 13 : 0) | max_packet << 16;
}

/* An ED with an empty queue (its dummy tail TD). */
static struct ed *make_ed(u32 flags) {
    struct ed *e = new_ed();
    struct td *dummy = new_td();
    if (!e || !dummy) return 0;
    e->flags = flags;
    e->tail = dma_address(dummy);
    e->head = dma_address(dummy);
    return e;
}

static void wait_frame(struct ohci *o) {
    w32(o, 0x0c, 1u << 2);                       /* clear SF */
    for (int i = 0; i < 20 && !(r32(o, 0x0c) & (1u << 2)); ++i) cpu_sleep(200000);
}

static void link_ed(struct ed *head, struct ed *e) {
    e->next = head->next;
    __asm__ volatile("" ::: "memory");
    head->next = dma_address(e);
}

static void unlink_ed(struct ohci *o, struct ed *head, struct ed *e) {
    e->flags |= ED_SKIP;
    wait_frame(o);
    for (struct ed *p = head; p; p = (struct ed *)(uptr)(p->next & ~15u)) {
        if ((struct ed *)(uptr)(p->next & ~15u) == e) { p->next = e->next; break; }
        if (!p->next) break;
    }
    wait_frame(o);
}

static void free_ed(struct ed *e) {
    if (!e) return;
    dma_free((void *)(uptr)(e->tail & ~15u), sizeof(struct td));
    dma_free(e, sizeof(struct ed));
}

/* Queue N TDs (pids/buffers/toggles given) on ED and start the list.
   Returns the TDs actually used (the old tail becomes the first). */
struct piece { u32 pid, address, length, toggle; };

static int queue(struct ed *e, const struct piece *pieces, int count, struct td **tds) {
    struct td *t = (struct td *)(uptr)(e->tail & ~15u);
    for (int i = 0; i < count; ++i) {
        struct td *next = new_td();
        if (!next) return -1;
        const struct piece *p = &pieces[i];
        tds[i] = t;
        t->start = p->address;
        t->length = p->length;
        t->buffer = p->length ? p->address : 0;
        t->end = p->length ? p->address + p->length - 1 : 0;
        t->next = dma_address(next);
        __asm__ volatile("" ::: "memory");
        t->flags = 1u << 18 | p->pid << 19 | 7u << 21 | p->toggle << 24 | 0xfu << 28;
        t = next;
    }
    __asm__ volatile("" ::: "memory");
    e->tail = dma_address(t);
    return 0;
}

static int td_moved(const struct td *t) {
    if (!t->length) return 0;
    return t->buffer ? (int)(t->buffer - t->start) : (int)t->length;
}

static int complete(struct ohci *o, struct ed *e, struct td **tds, int count, u32 timeout_ms) {
    u64 end = cpu_nanoseconds() + (u64)timeout_ms * 1000000ull;
    int result = 0;
    for (;;) {
        w32(o, 0x0c, 2);                          /* WDH: let the done queue move on */
        if ((e->head & ~15u) == (e->tail & ~15u)) break;
        if (e->head & ED_HALTED) { result = USB_EIO; break; }
        if (cpu_nanoseconds() > end) { result = USB_ETIMEOUT; break; }
    }
    if (result) {
        /* Drop the rest of the queue; keep the toggle carry. */
        e->flags |= ED_SKIP;
        wait_frame(o);
        u32 carry = e->head & 2;
        for (int i = 0; i < count; ++i) {
            u32 cc = tds[i]->flags >> 28;
            if (cc == 4) result = USB_ESTALL;
            else if (cc == 8 || cc == 9) result = result == USB_ETIMEOUT ? result : USB_EIO;
        }
        e->head = (e->tail & ~15u) | carry;
        e->flags &= ~ED_SKIP;
    }
    int moved = 0;
    for (int i = 0; i < count && !result; ++i) {
        u32 cc = tds[i]->flags >> 28;
        if (cc == 9) {                            /* data underrun: short packet (allowed by R) */
            moved += td_moved(tds[i]);
            break;
        }
        if (cc == 4) { result = USB_ESTALL; break; }
        if (cc) { result = cc == 8 ? USB_EBABBLE : USB_EIO; break; }
        moved += td_moved(tds[i]);
    }
    for (int i = 0; i < count; ++i) dma_free(tds[i], sizeof(struct td));
    return result ? result : moved;
}

static int ohci_control(struct usb_device *d, const struct usb_setup *setup, void *data) {
    struct ohci *o = d->hc->data;
    struct odev *x = d->hc_data;
    if (!x || d->gone) return USB_ENODEV;
    static u8 *setup_buffer;
    if (!setup_buffer && !(setup_buffer = dma_alloc(8, 32))) return USB_ENOMEM;
    memcpy(setup_buffer, setup, 8);
    int in = setup->type & 0x80;
    struct piece pieces[3];
    int count = 0;
    pieces[count++] = (struct piece){0, dma_address(setup_buffer), 8, 2};
    if (setup->length) pieces[count++] = (struct piece){in ? 2u : 1u, dma_address(data), setup->length, 3};
    pieces[count++] = (struct piece){setup->length && in ? 1u : 2u, 0, 0, 3};
    struct td *tds[3];
    if (queue(x->control, pieces, count, tds)) return USB_ENOMEM;
    w32(o, 0x08, 2);                               /* ControlListFilled */
    int r = complete(o, x->control, tds, count, 2000);
    if (r >= 8) r -= 8;                            /* the setup stage is not data */
    else if (r > 0) r = 0;
    if (r > setup->length) r = setup->length;
    return r;
}

static int ohci_open(struct usb_device *d) {
    struct ohci *o = d->hc->data;
    struct odev *x = memory_alloc(sizeof(*x), 16, OWNER_KERNEL, 1);
    if (!x) return -1;
    d->hc_data = x;
    x->control = make_ed(ed_flags(d, 0, 0, d->max_packet0));
    if (!x->control) return -1;
    link_ed(o->control_head, x->control);
    return 0;
}

static int ohci_address(struct usb_device *d, int address) {
    if (usb_control(d, 0x00, 5, (u16)address, 0, 0, 0) < 0) return -1;
    cpu_sleep(2000000);
    struct odev *x = d->hc_data;
    d->address = address;
    x->control->flags = (x->control->flags & ~0x7fu) | (u32)address;
    return 0;
}

static int ohci_update_control(struct usb_device *d) {
    struct odev *x = d->hc_data;
    x->control->flags = (x->control->flags & ~(0x7ffu << 16)) | (u32)d->max_packet0 << 16;
    return 0;
}

static int ohci_configure(struct usb_device *d, const struct usb_endpoint *eps, int count) {
    struct ohci *o = d->hc->data;
    struct odev *x = d->hc_data;
    for (int i = 0; i < count; ++i) {
        if ((eps[i].attributes & 3) != 2) continue;
        int index = usb_endpoint_index(eps[i].address);
        if (x->endpoints[index]) continue;
        struct ed *e = make_ed(ed_flags(d, eps[i].address & 15, eps[i].address & 0x80 ? 2 : 1, eps[i].max_packet & 0x7ff));
        if (!e) return -1;
        x->endpoints[index] = e;
        link_ed(o->bulk_head, e);
    }
    return 0;
}

static int ohci_bulk(struct usb_device *d, const struct usb_endpoint *ep, void *data, u32 length, u32 timeout) {
    struct ohci *o = d->hc->data;
    struct odev *x = d->hc_data;
    if (!x || d->gone) return USB_ENODEV;
    struct ed *e = x->endpoints[usb_endpoint_index(ep->address)];
    if (!e) return USB_EIO;
    struct piece pieces[16];
    int count = 0;
    u32 address = dma_address(data), left = length, pid = ep->address & 0x80 ? 2 : 1;
    do {
        /* A TD may cross one page boundary: up to the end of the next page. */
        u32 room = 8192 - (address & 0xfff);
        u32 chunk = left < room ? left : room;
        if (chunk < left) chunk -= chunk % (ep->max_packet ? ep->max_packet : 64);
        if (count == 16) return USB_EIO;
        pieces[count++] = (struct piece){pid, address, chunk, 0};
        address += chunk;
        left -= chunk;
    } while (left);
    struct td *tds[16];
    if (queue(e, pieces, count, tds)) return USB_ENOMEM;
    w32(o, 0x08, 4);                               /* BulkListFilled */
    return complete(o, e, tds, count, timeout);
}

static int ohci_reset_endpoint(struct usb_device *d, const struct usb_endpoint *ep) {
    struct odev *x = d->hc_data;
    if (!x) return -1;
    struct ed *e = x->endpoints[usb_endpoint_index(ep->address)];
    if (e) e->head &= ~3u;                         /* toggle DATA0, not halted */
    return 0;
}

static void queue_interrupt(struct usb_pipe *pipe, struct opipe *p) {
    struct piece piece = {2, dma_address(pipe->buffer), pipe->endpoint.max_packet, 0};
    queue(p->ed, &piece, 1, &p->td);
}

static int ohci_interrupt_open(struct usb_pipe *pipe) {
    struct usb_device *d = pipe->device;
    struct ohci *o = d->hc->data;
    struct opipe *p = memory_alloc(sizeof(*p), 16, OWNER_KERNEL, 1);
    if (!p) return -1;
    pipe->buffer = dma_alloc(pipe->endpoint.max_packet ? pipe->endpoint.max_packet : 8, 32);
    p->ed = make_ed(ed_flags(d, pipe->endpoint.address & 15, 2, pipe->endpoint.max_packet & 0x7ff));
    if (!pipe->buffer || !p->ed) return -1;
    pipe->hc_data = p;
    queue_interrupt(pipe, p);
    link_ed(o->interrupt_head, p->ed);
    return 0;
}

static int ohci_interrupt_poll(struct usb_pipe *pipe) {
    struct ohci *o = pipe->device->hc->data;
    struct opipe *p = pipe->hc_data;
    if (!p) return USB_EIO;
    w32(o, 0x0c, 2);
    struct ed *e = p->ed;
    int halted = e->head & ED_HALTED;
    if (!halted && (e->head & ~15u) != (e->tail & ~15u)) return USB_EAGAIN;
    u32 cc = p->td->flags >> 28;
    int r = (cc == 0 || cc == 9) ? td_moved(p->td) : USB_EIO;
    dma_free(p->td, sizeof(struct td));
    if (halted) e->head = (e->tail & ~15u) | (e->head & 2);
    queue_interrupt(pipe, p);
    return r;
}

static void ohci_interrupt_close(struct usb_pipe *pipe) {
    struct ohci *o = pipe->device->hc->data;
    struct opipe *p = pipe->hc_data;
    if (!p) return;
    unlink_ed(o, o->interrupt_head, p->ed);
    if ((p->ed->head & ~15u) != (p->ed->tail & ~15u)) dma_free(p->td, sizeof(struct td));
    free_ed(p->ed);
    dma_free(pipe->buffer, pipe->endpoint.max_packet ? pipe->endpoint.max_packet : 8);
    memory_free(p, OWNER_KERNEL);
    pipe->hc_data = 0;
}

static void ohci_close(struct usb_device *d) {
    struct ohci *o = d->hc->data;
    struct odev *x = d->hc_data;
    if (!x) return;
    if (x->control) { unlink_ed(o, o->control_head, x->control); free_ed(x->control); }
    for (int i = 0; i < USB_ENDPOINTS; ++i)
        if (x->endpoints[i]) { unlink_ed(o, o->bulk_head, x->endpoints[i]); free_ed(x->endpoints[i]); }
    memory_free(x, OWNER_KERNEL);
    d->hc_data = 0;
}

/* ---- root hub ----------------------------------------------------------- */

#define PORT(p) (0x54 + 4 * ((p) - 1))

static int ohci_port_count(struct usb_hc *hc) { return ((struct ohci *)hc->data)->ports; }

static int ohci_port_status(struct usb_hc *hc, int port) {
    struct ohci *o = hc->data;
    u32 v = r32(o, PORT(port));
    int status = 0;
    if (v & 1) status |= USB_PORT_CONNECTED;
    if (v & 2) status |= USB_PORT_ENABLED;
    if (v & (1u << 16)) { status |= USB_PORT_CHANGED; w32(o, PORT(port), 1u << 16); }
    return status | (v & (1u << 9) ? USB_SPEED_LOW : USB_SPEED_FULL) << 8;
}

static int ohci_port_reset(struct usb_hc *hc, int port) {
    struct ohci *o = hc->data;
    w32(o, PORT(port), 1u << 4);                   /* SetPortReset */
    for (int i = 0; i < 50 && !(r32(o, PORT(port)) & (1u << 20)); ++i) cpu_sleep(2000000);
    w32(o, PORT(port), 0x001f0000u);               /* clear the change bits */
    cpu_sleep(20000000);
    u32 v = r32(o, PORT(port));
    if (!(v & 1)) return -1;
    if (!(v & 2)) { w32(o, PORT(port), 2); cpu_sleep(2000000); v = r32(o, PORT(port)); }
    if (!(v & 2)) return -1;
    return v & (1u << 9) ? USB_SPEED_LOW : USB_SPEED_FULL;
}

static void ohci_port_disable(struct usb_hc *hc, int port) { w32(hc->data, PORT(port), 1); }

static void ohci_shutdown(struct usb_hc *hc) {
    struct ohci *o = hc->data;
    w32(o, 0x04, 0);                               /* USB reset state */
    cpu_sleep(2000000);
}

static const struct usb_hc_ops ops = {
    "OHCI", ohci_port_count, ohci_port_status, ohci_port_reset, ohci_port_disable,
    ohci_open, ohci_address, ohci_update_control, ohci_control, ohci_configure, ohci_bulk,
    ohci_reset_endpoint, ohci_interrupt_open, ohci_interrupt_poll, ohci_interrupt_close,
    0, ohci_close, 0, ohci_shutdown,
};

/* Take the controller from the BIOS (SMM) driver. */
void ohci_handoff(const struct pci_function *f) {
    int io;
    u64 bar = pci_bar(f, 0, &io);
    if (!bar || io) return;
    pci_enable(f, 1, 0);
    volatile u32 *regs = (volatile u32 *)(uptr)bar;
    if (regs[1] & (1u << 8)) {
        regs[2] = 1u << 3;                         /* OwnershipChangeRequest */
        for (int i = 0; i < 100 && (regs[1] & (1u << 8)); ++i) cpu_sleep(10000000);
    }
}

void ohci_probe(const struct pci_function *f) {
    int io;
    u64 bar = pci_bar(f, 0, &io);
    if (!bar || io) return;
    pci_enable(f, 1, 0);
    struct ohci *o = memory_alloc(sizeof(*o), 16, OWNER_KERNEL, 1);
    if (!o) return;
    o->regs = (volatile u8 *)(uptr)bar;
    w32(o, 0x14, 0xc000007fu);                     /* every interrupt off */
    u32 interval = r32(o, 0x34) & 0x3fff;
    if (interval < 11000 || interval > 13000) interval = 11999;
    w32(o, 0x08, 1);                               /* HostControllerReset */
    for (int i = 0; i < 100 && (r32(o, 0x08) & 1); ++i) cpu_sleep(10000);
    o->hcca = dma_alloc(256, 256);
    o->control_head = make_ed(ED_SKIP);
    o->bulk_head = make_ed(ED_SKIP);
    o->interrupt_head = make_ed(ED_SKIP);
    if (!o->hcca || !o->control_head || !o->bulk_head || !o->interrupt_head) return;
    for (int i = 0; i < 32; ++i) o->hcca[i] = dma_address(o->interrupt_head);
    w32(o, 0x18, dma_address(o->hcca));
    w32(o, 0x20, dma_address(o->control_head));
    w32(o, 0x28, dma_address(o->bulk_head));
    w32(o, 0x34, interval | ((interval - 210) * 6 / 7) << 16 | 1u << 31);
    w32(o, 0x40, interval * 9 / 10);
    w32(o, 0x44, 0x628);
    w32(o, 0x04, 3u | 1u << 2 | 1u << 4 | 1u << 5 | 2u << 6);   /* operational, all lists */
    u32 a = r32(o, 0x48);
    o->ports = (int)(a & 0xff);
    if (o->ports > USB_MAX_PORTS) o->ports = USB_MAX_PORTS;
    w32(o, 0x50, 1u << 16);                        /* global power on */
    for (int p = 1; p <= o->ports; ++p) w32(o, PORT(p), 1u << 8);
    cpu_sleep((u64)((a >> 24) * 2 + 20) * 1000000ull);
    usb_add_hc(&ops, o, o->ports);
}
