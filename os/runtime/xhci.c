/*
 * DOS-XC /os/runtime/xhci.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "usb.h"

#define RING_TRBS 128
#define EVENT_TRBS 256

struct trb { u32 parameter_low, parameter_high, status, control; };
struct ring { struct trb *trbs; u32 enqueue; u32 cycle; };

struct xpipe { int pending, done, length; u32 trb; struct usb_pipe *pipe; };

struct xdev {
    int slot;
    u8 *input, *output;
    struct ring rings[32];            /* by device context index */
    struct xpipe *pipes[32];
    int max_dci;
    u32 slot_dw0, slot_dw1, slot_dw2;
};

struct xhci {
    volatile u8 *cap, *op, *rt;
    volatile u32 *db;
    int ports, slots, context_size;
    u8 port_major[256];               /* 2 or 3 per port */
    u64 *dcbaa;
    struct ring command;
    struct trb *events;
    u32 event_dequeue, event_cycle;
    struct xdev *by_slot[256];
    /* the synchronous transfer or command being waited for */
    int wait_active, wait_done, wait_code, wait_slot, wait_dci;
    u32 wait_last, wait_data, wait_residual, wait_command;
    u32 wait_count, wait_trbs[64], wait_lengths[64];
};

static u32 r32(volatile u8 *base, u32 off) { return *(volatile u32 *)(base + off); }
static void w32(volatile u8 *base, u32 off, u32 v) { *(volatile u32 *)(base + off) = v; }
static void w64(volatile u8 *base, u32 off, u64 v) {
    *(volatile u32 *)(base + off) = (u32)v;
    *(volatile u32 *)(base + off + 4) = (u32)(v >> 32);
}

static int ring_init(struct ring *r, u32 count) {
    r->trbs = dma_alloc(count * 16, 4096);
    if (!r->trbs) return -1;
    r->enqueue = 0;
    r->cycle = 1;
    struct trb *link = &r->trbs[count - 1];
    link->parameter_low = dma_address(r->trbs);
    link->parameter_high = 0;
    link->control = 6u << 10 | 2;
    return 0;
}

/* Queue a TRB; returns its address. */
static u32 ring_put(struct ring *r, u32 count, u32 p0, u32 p1, u32 status, u32 control) {
    struct trb *t = &r->trbs[r->enqueue];
    t->parameter_low = p0;
    t->parameter_high = p1;
    t->status = status;
    __asm__ volatile("" ::: "memory");
    t->control = (control & ~1u) | r->cycle;
    u32 address = dma_address(t);
    if (++r->enqueue == count - 1) {
        struct trb *link = &r->trbs[count - 1];
        link->control = (link->control & ~1u) | r->cycle | (control & 0x10);  /* keep a chain going */
        r->cycle ^= 1;
        r->enqueue = 0;
    }
    return address;
}

static u32 ring_position(const struct ring *r) { return dma_address(&r->trbs[r->enqueue]) | r->cycle; }

/* ---- events ----------------------------------------------------------- */

static void handle_event(struct xhci *x, const struct trb *e) {
    u32 type = (e->control >> 10) & 63, code = e->status >> 24;
    if (type == 33) {
        if (x->wait_active && x->wait_command == e->parameter_low) {
            x->wait_code = (int)code;
            x->wait_slot = (int)(e->control >> 24);
            x->wait_done = 1;
        }
        return;
    }
    if (type != 32) return;
    int slot = (int)(e->control >> 24), dci = (int)((e->control >> 16) & 31);
    u32 trb = e->parameter_low, residual = e->status & 0xffffff;

    struct xdev *xd = x->by_slot[slot];
    if (xd && xd->pipes[dci] && xd->pipes[dci]->pending && xd->pipes[dci]->trb == trb) {
        struct xpipe *p = xd->pipes[dci];
        p->pending = 0;
        p->done = 1;
        u32 size = p->pipe->endpoint.max_packet;
        p->length = code == 1 || code == 13 ? (int)(size - (residual > size ? size : residual)) : -(int)code;
        return;
    }
    if (!x->wait_active || x->wait_command || slot != x->wait_slot || dci != x->wait_dci) return;
    /* One synchronous transfer at a time per endpoint, so every event of
       this endpoint belongs to it. */
    if (code != 1 && code != 13) { x->wait_code = (int)code; x->wait_done = 1; return; }
    if (x->wait_count) {
        u32 index = 0, actual = 0;
        while (index < x->wait_count && x->wait_trbs[index] != trb) ++index;
        if (index == x->wait_count) return;
        for (u32 i = 0; i < index; ++i) actual += x->wait_lengths[i];
        actual += x->wait_lengths[index] - (residual > x->wait_lengths[index] ? x->wait_lengths[index] : residual);
        if (code == 13 || trb == x->wait_last) { x->wait_residual = actual; x->wait_code = 1; x->wait_done = 1; }
        return;
    }
    if (trb == x->wait_data) x->wait_residual = residual;
    if (trb == x->wait_last) { x->wait_code = 1; x->wait_done = 1; }
}

static void process_events(struct xhci *x) {
    int any = 0;
    for (;;) {
        struct trb *e = &x->events[x->event_dequeue];
        if ((e->control & 1) != x->event_cycle) break;
        handle_event(x, e);
        any = 1;
        if (++x->event_dequeue == EVENT_TRBS) { x->event_dequeue = 0; x->event_cycle ^= 1; }
    }
    if (any) w64(x->rt, 0x38, dma_address(&x->events[x->event_dequeue]) | 8);  /* ERDP, clear EHB */
}

static int wait_for(struct xhci *x, u32 timeout_ms) {
    u64 end = cpu_nanoseconds() + (u64)timeout_ms * 1000000ull;
    while (!x->wait_done) {
        process_events(x);
        if (x->wait_done) break;
        if (cpu_nanoseconds() > end) return -1;
        if (r32(x->op, 4) & 0x1004) return -1;    /* host system error / controller error */
    }
    return 0;
}

static int command(struct xhci *x, u32 p0, u32 p1, u32 status, u32 control, int *slot) {
    x->wait_active = 1;
    x->wait_done = 0;
    x->wait_code = 0;
    x->wait_command = ring_put(&x->command, RING_TRBS, p0, p1, status, control);
    x->db[0] = 0;
    int r = wait_for(x, 2000);
    x->wait_active = 0;
    x->wait_command = 0;
    if (r) return -1;
    if (slot) *slot = x->wait_slot;
    return x->wait_code == 1 ? 0 : -x->wait_code;
}

/* ---- contexts ----------------------------------------------------------- */

static u32 *input_control(struct xhci *x, struct xdev *d) { (void)x; return (u32 *)d->input; }
static u32 *input_slot(struct xhci *x, struct xdev *d) { return (u32 *)(d->input + x->context_size); }
static u32 *input_endpoint(struct xhci *x, struct xdev *d, int dci) { return (u32 *)(d->input + (dci + 1) * x->context_size); }

static void clear_input(struct xhci *x, struct xdev *d) { memset(d->input, 0, (u32)x->context_size * 33); }

static void fill_slot(struct xhci *x, struct xdev *d) {
    u32 *s = input_slot(x, d);
    s[0] = d->slot_dw0 | (u32)d->max_dci << 27;
    s[1] = d->slot_dw1;
    s[2] = d->slot_dw2;
}

static int root_port(const struct usb_device *dev) {
    while (dev->parent) dev = dev->parent;
    return dev->port;
}

static int xhci_speed(int speed) {
    return speed == USB_SPEED_FULL ? 1 : speed == USB_SPEED_LOW ? 2 : speed == USB_SPEED_HIGH ? 3 : 4;
}

static void endpoint_context(struct xhci *x, struct xdev *d, int dci, int type, u32 max_packet, u32 interval, u32 average) {
    u32 *e = input_endpoint(x, d, dci);
    memset(e, 0, (u32)x->context_size);
    e[0] = interval << 16;
    e[1] = 3u << 1 | (u32)type << 3 | max_packet << 16;
    u32 dequeue = ring_position(&d->rings[dci]);
    e[2] = dequeue;
    e[3] = 0;
    e[4] = average | (type == 7 || type == 3 ? max_packet << 16 : 0);
}

static int xhci_open(struct usb_device *dev) {
    struct xhci *x = dev->hc->data;
    int slot = 0;
    if (command(x, 0, 0, 0, 9u << 10, &slot) || slot <= 0 || slot > x->slots) return -1;
    struct xdev *d = memory_alloc(sizeof(*d), 16, OWNER_KERNEL, 1);
    if (!d) return -1;
    d->slot = slot;
    dev->hc_data = d;
    d->input = dma_alloc((u32)x->context_size * 33, 64);
    d->output = dma_alloc((u32)x->context_size * 32, 64);
    if (!d->input || !d->output || ring_init(&d->rings[1], RING_TRBS)) return -1;
    x->dcbaa[slot] = dma_address(d->output);
    x->by_slot[slot] = d;
    if (dev->speed == USB_SPEED_FULL) dev->max_packet0 = 64;
    d->max_dci = 1;
    d->slot_dw0 = (dev->route & 0xfffff) | (u32)xhci_speed(dev->speed) << 20;
    d->slot_dw1 = (u32)root_port(dev) << 16;
    d->slot_dw2 = 0;
    if (dev->tt_hub && dev->tt_hub->hc_data) {
        struct xdev *hub = dev->tt_hub->hc_data;
        d->slot_dw2 = (u32)hub->slot | (u32)dev->tt_port << 8;
        if (hub->slot_dw0 & (1u << 25)) d->slot_dw0 |= 1u << 25;
    }
    clear_input(x, d);
    input_control(x, d)[1] = 3;
    fill_slot(x, d);
    endpoint_context(x, d, 1, 4, dev->max_packet0, 0, 8);
    if (command(x, dma_address(d->input), 0, 0, 11u << 10 | (u32)slot << 24, 0)) return -1;
    dev->address = (int)(((u32 *)(d->output + 0))[3] & 0xff);
    return 0;
}

static int xhci_address(struct usb_device *dev, int address) { (void)dev; (void)address; return 0; }

static int xhci_update_control(struct usb_device *dev) {
    struct xhci *x = dev->hc->data;
    struct xdev *d = dev->hc_data;
    clear_input(x, d);
    input_control(x, d)[1] = 2;
    u32 *e = input_endpoint(x, d, 1);
    memcpy(e, d->output + x->context_size, (u32)x->context_size);
    e[1] = (e[1] & 0xffff) | (u32)dev->max_packet0 << 16;
    return command(x, dma_address(d->input), 0, 0, 13u << 10 | (u32)d->slot << 24, 0) ? -1 : 0;
}

static int reset_ring(struct xhci *x, struct xdev *d, int dci) {
    command(x, 0, 0, 0, 14u << 10 | (u32)d->slot << 24 | (u32)dci << 16, 0);
    u32 position = ring_position(&d->rings[dci]);
    return command(x, position, 0, 0, 16u << 10 | (u32)d->slot << 24 | (u32)dci << 16, 0);
}

static void stop_ring(struct xhci *x, struct xdev *d, int dci) {
    command(x, 0, 0, 0, 15u << 10 | (u32)d->slot << 24 | (u32)dci << 16, 0);
    u32 position = ring_position(&d->rings[dci]);
    command(x, position, 0, 0, 16u << 10 | (u32)d->slot << 24 | (u32)dci << 16, 0);
}

static int finish(struct xhci *x, struct xdev *d, int dci, u32 timeout, int *actual) {
    x->db[d->slot] = (u32)dci;
    int r = wait_for(x, timeout);
    x->wait_active = 0;
    if (r) { stop_ring(x, d, dci); return USB_ETIMEOUT; }
    if (x->wait_code == 1) { if (actual) *actual = (int)x->wait_residual; return 0; }
    int code = x->wait_code;
    reset_ring(x, d, dci);
    if (code == 6) return USB_ESTALL;
    if (code == 3) return USB_EBABBLE;
    return USB_EIO;
}

static int xhci_control(struct usb_device *dev, const struct usb_setup *setup, void *data) {
    struct xhci *x = dev->hc->data;
    struct xdev *d = dev->hc_data;
    if (!d || dev->gone) return USB_ENODEV;
    struct ring *r = &d->rings[1];
    u32 setup_words[2];
    memcpy(setup_words, setup, 8);
    int in = setup->type & 0x80, length = setup->length;
    u32 trt = length ? (in ? 3u : 2u) : 0u;
    x->wait_active = 1;
    x->wait_done = 0;
    x->wait_command = 0;
    x->wait_slot = d->slot;
    x->wait_dci = 1;
    x->wait_count = 0;
    x->wait_residual = 0;
    x->wait_data = 0;
    ring_put(r, RING_TRBS, setup_words[0], setup_words[1], 8, 2u << 10 | 1u << 6 | trt << 16);
    if (length)
        x->wait_data = ring_put(r, RING_TRBS, dma_address(data), 0, (u32)length,
                                3u << 10 | (in ? 1u << 16 : 0) | 1u << 2);
    x->wait_last = ring_put(r, RING_TRBS, 0, 0, 0, 4u << 10 | (length && in ? 0 : 1u << 16) | 1u << 5);
    int result = finish(x, d, 1, 2000, 0);
    if (result) return result;
    int actual = length - (int)(x->wait_residual > (u32)length ? (u32)length : x->wait_residual);
    return actual;
}

static int dci_of(u8 address) { return (address & 15) * 2 + (address & 0x80 ? 1 : 0); }

static int interval_of(const struct usb_device *dev, const struct usb_endpoint *e) {
    if (dev->speed >= USB_SPEED_HIGH) {
        int v = e->interval ? e->interval - 1 : 0;
        return v > 15 ? 15 : v;
    }
    int frames = e->interval ? e->interval : 1, v = 3;
    while (v < 10 && (1 << (v + 1)) <= frames * 8) ++v;
    return v;
}

static int xhci_configure(struct usb_device *dev, const struct usb_endpoint *eps, int count) {
    struct xhci *x = dev->hc->data;
    struct xdev *d = dev->hc_data;
    clear_input(x, d);
    u32 add = 1;
    for (int i = 0; i < count; ++i) {
        int dci = dci_of(eps[i].address);
        if (!d->rings[dci].trbs && ring_init(&d->rings[dci], RING_TRBS)) return -1;
        if (dci > d->max_dci) d->max_dci = dci;
        int type = (eps[i].attributes & 3) == 2 ? (eps[i].address & 0x80 ? 6 : 2) : (eps[i].address & 0x80 ? 7 : 3);
        u32 mps = eps[i].max_packet & 0x7ff;
        endpoint_context(x, d, dci, type, mps, type == 6 || type == 2 ? 0 : (u32)interval_of(dev, &eps[i]),
                         type == 6 || type == 2 ? 3072 : mps);
        add |= 1u << dci;
    }
    input_control(x, d)[1] = add;
    fill_slot(x, d);
    return command(x, dma_address(d->input), 0, 0, 12u << 10 | (u32)d->slot << 24, 0) ? -1 : 0;
}

static int xhci_hub_info(struct usb_device *dev, int ports, int think_time, int multi_tt) {
    struct xhci *x = dev->hc->data;
    struct xdev *d = dev->hc_data;
    d->slot_dw0 |= 1u << 26 | (multi_tt ? 1u << 25 : 0);
    d->slot_dw1 = (d->slot_dw1 & 0x00ffffff) | (u32)ports << 24;
    if (dev->speed == USB_SPEED_HIGH) d->slot_dw2 = (d->slot_dw2 & ~(3u << 16)) | (u32)think_time << 16;
    clear_input(x, d);
    input_control(x, d)[1] = 1;
    fill_slot(x, d);
    return command(x, dma_address(d->input), 0, 0, 12u << 10 | (u32)d->slot << 24, 0) ? -1 : 0;
}

static int xhci_bulk(struct usb_device *dev, const struct usb_endpoint *e, void *data, u32 length, u32 timeout) {
    struct xhci *x = dev->hc->data;
    struct xdev *d = dev->hc_data;
    if (!d || dev->gone) return USB_ENODEV;
    int dci = dci_of(e->address);
    struct ring *r = &d->rings[dci];
    if (!r->trbs) return USB_EIO;
    x->wait_active = 1;
    x->wait_done = 0;
    x->wait_command = 0;
    x->wait_slot = d->slot;
    x->wait_dci = dci;
    x->wait_data = 0;
    x->wait_count = 0;
    x->wait_residual = 0;
    u32 address = dma_address(data), left = length;
    do {
        u32 chunk = 0x10000 - (address & 0xffff);
        if (chunk > left) chunk = left;
        if (x->wait_count == 64) return USB_EIO;
        int last = chunk == left;
        u32 trb = ring_put(r, RING_TRBS, address, 0, chunk, 1u << 10 | 1u << 2 | (last ? 1u << 5 : 1u << 4));
        x->wait_last = trb;
        x->wait_trbs[x->wait_count] = trb;
        x->wait_lengths[x->wait_count++] = chunk;
        address += chunk;
        left -= chunk;
    } while (left);
    int actual = 0;
    int result = finish(x, d, dci, timeout, &actual);
    return result ? result : actual;
}

static int xhci_reset_endpoint(struct usb_device *dev, const struct usb_endpoint *e) {
    struct xhci *x = dev->hc->data;
    struct xdev *d = dev->hc_data;
    if (!d) return -1;
    int dci = dci_of(e->address);
    u32 *out = (u32 *)(d->output + dci * x->context_size);
    if ((out[0] & 7) == 2) return reset_ring(x, d, dci);
    return 0;
}

static void queue_interrupt(struct xhci *x, struct xdev *d, struct xpipe *p, int dci) {
    p->trb = ring_put(&d->rings[dci], RING_TRBS, dma_address(p->pipe->buffer), 0, p->pipe->endpoint.max_packet,
                      1u << 10 | 1u << 2 | 1u << 5);
    p->pending = 1;
    p->done = 0;
    x->db[d->slot] = (u32)dci;
}

static int xhci_interrupt_open(struct usb_pipe *pipe) {
    struct usb_device *dev = pipe->device;
    struct xhci *x = dev->hc->data;
    struct xdev *d = dev->hc_data;
    int dci = dci_of(pipe->endpoint.address);
    if (!d->rings[dci].trbs) return -1;
    struct xpipe *p = memory_alloc(sizeof(*p), 16, OWNER_KERNEL, 1);
    pipe->buffer = dma_alloc(pipe->endpoint.max_packet ? pipe->endpoint.max_packet : 8, 64);
    if (!p || !pipe->buffer) return -1;
    p->pipe = pipe;
    pipe->hc_data = p;
    d->pipes[dci] = p;
    queue_interrupt(x, d, p, dci);
    return 0;
}

static int xhci_interrupt_poll(struct usb_pipe *pipe) {
    struct usb_device *dev = pipe->device;
    struct xhci *x = dev->hc->data;
    struct xdev *d = dev->hc_data;
    struct xpipe *p = pipe->hc_data;
    if (!p || !d) return USB_EIO;
    if (!p->done) return USB_EAGAIN;
    int length = p->length;
    int dci = dci_of(pipe->endpoint.address);
    if (length < 0) {
        if (length == -6 || length == -4) reset_ring(x, d, dci);
        cpu_sleep(1000000);
    }
    queue_interrupt(x, d, p, dci);
    return length < 0 ? USB_EIO : length;
}

static void xhci_interrupt_close(struct usb_pipe *pipe) {
    struct usb_device *dev = pipe->device;
    struct xdev *d = dev->hc_data;
    struct xpipe *p = pipe->hc_data;
    if (d) {
        int dci = dci_of(pipe->endpoint.address);
        if (d->pipes[dci] == p) d->pipes[dci] = 0;
    }
    dma_free(pipe->buffer, pipe->endpoint.max_packet ? pipe->endpoint.max_packet : 8);
    if (p) memory_free(p, OWNER_KERNEL);
    pipe->hc_data = 0;
}

static void xhci_close(struct usb_device *dev) {
    struct xhci *x = dev->hc->data;
    struct xdev *d = dev->hc_data;
    if (!d) return;
    if (d->slot) {
        command(x, 0, 0, 0, 10u << 10 | (u32)d->slot << 24, 0);
        x->dcbaa[d->slot] = 0;
        x->by_slot[d->slot] = 0;
    }
    for (int i = 0; i < 32; ++i) if (d->rings[i].trbs) dma_free(d->rings[i].trbs, RING_TRBS * 16);
    dma_free(d->input, (u32)x->context_size * 33);
    dma_free(d->output, (u32)x->context_size * 32);
    memory_free(d, OWNER_KERNEL);
    dev->hc_data = 0;
}

#define PORTSC(x, p) (0x400 + 0x10 * ((p) - 1))
#define PORT_KEEP 0x0e00c3e9u                /* RO and RW-S bits: write back unchanged */

static int xhci_port_count(struct usb_hc *hc) { return ((struct xhci *)hc->data)->ports; }

static int convert_speed(u32 portsc) {
    u32 s = (portsc >> 10) & 15;
    return s == 1 ? USB_SPEED_FULL : s == 2 ? USB_SPEED_LOW : s == 3 ? USB_SPEED_HIGH : s >= 4 ? USB_SPEED_SUPER : 0;
}

static int xhci_port_status(struct usb_hc *hc, int port) {
    struct xhci *x = hc->data;
    u32 v = r32(x->op, PORTSC(x, port));
    int status = 0;
    if (v & 1) status |= USB_PORT_CONNECTED;
    if (v & 2) status |= USB_PORT_ENABLED;
    if (v & (1u << 17)) {
        status |= USB_PORT_CHANGED;
        w32(x->op, PORTSC(x, port), (v & PORT_KEEP & ~2u) | 1u << 17);
    }
    return status | convert_speed(v) << 8;
}

static int xhci_port_reset(struct usb_hc *hc, int port) {
    struct xhci *x = hc->data;
    u32 v = r32(x->op, PORTSC(x, port));
    if (x->port_major[port] == 3) {
        for (int i = 0; i < 50 && !(r32(x->op, PORTSC(x, port)) & 2); ++i) cpu_sleep(10000000);
        v = r32(x->op, PORTSC(x, port));
        if (!(v & 2)) {
            w32(x->op, PORTSC(x, port), (v & PORT_KEEP & ~2u) | 1u << 31);
            for (int i = 0; i < 50 && !(r32(x->op, PORTSC(x, port)) & (1u << 19)); ++i) cpu_sleep(10000000);
            v = r32(x->op, PORTSC(x, port));
            w32(x->op, PORTSC(x, port), (v & PORT_KEEP & ~2u) | 1u << 19 | 1u << 21);
        }
    } else {
        w32(x->op, PORTSC(x, port), (v & PORT_KEEP & ~2u) | 1u << 4);
        for (int i = 0; i < 50; ++i) {
            cpu_sleep(10000000);
            if (r32(x->op, PORTSC(x, port)) & (1u << 21)) break;
        }
        v = r32(x->op, PORTSC(x, port));
        w32(x->op, PORTSC(x, port), (v & PORT_KEEP & ~2u) | 1u << 21);
    }
    cpu_sleep(20000000);
    v = r32(x->op, PORTSC(x, port));
    /* Acknowledge every change the reset caused, so the next poll does not
       take it for a new connection. */
    w32(x->op, PORTSC(x, port), (v & PORT_KEEP & ~2u) | 0x00fe0000u);
    if (!(v & 1) || !(v & 2)) return -1;
    return convert_speed(v);
}

static void xhci_port_disable(struct usb_hc *hc, int port) {
    struct xhci *x = hc->data;
    u32 v = r32(x->op, PORTSC(x, port));
    w32(x->op, PORTSC(x, port), (v & PORT_KEEP) | 2);
}

static void xhci_poll(struct usb_hc *hc) { process_events(hc->data); }

static void xhci_shutdown(struct usb_hc *hc) {
    struct xhci *x = hc->data;
    w32(x->op, 0, r32(x->op, 0) & ~1u);
    for (int i = 0; i < 100 && !(r32(x->op, 4) & 1); ++i) cpu_sleep(1000000);
    w32(x->op, 0, 2);
}

static const struct usb_hc_ops ops = {
    "xHCI", xhci_port_count, xhci_port_status, xhci_port_reset, xhci_port_disable,
    xhci_open, xhci_address, xhci_update_control, xhci_control, xhci_configure, xhci_bulk,
    xhci_reset_endpoint, xhci_interrupt_open, xhci_interrupt_poll, xhci_interrupt_close,
    xhci_hub_info, xhci_close, xhci_poll, xhci_shutdown,
};

/* ---- start-up ----------------------------------------------------------- */

static void take_ownership(struct xhci *x, u32 hccparams) {
    u32 offset = (hccparams >> 16) * 4;
    while (offset) {
        volatile u8 *cap = x->cap + offset;
        u32 v = r32(cap, 0), id = v & 0xff;
        if (id == 2) {
            u32 major = v >> 24, ports = r32(cap, 8);
            u32 first = ports & 0xff, count = (ports >> 8) & 0xff;
            for (u32 p = first; p < first + count && p < 256; ++p) x->port_major[p] = (u8)major;
        }
        u32 next = (v >> 8) & 0xff;
        offset = next ? offset + next * 4 : 0;
    }
}

/* Take the controller from the firmware (the USB legacy support capability). */
void xhci_handoff(const struct pci_function *f) {
    int io;
    u64 bar = pci_bar(f, 0, &io);
    if (!bar || io) return;
    pci_enable(f, 1, 0);
    volatile u8 *base = (volatile u8 *)(uptr)bar;
    u32 offset = (r32(base, 0x10) >> 16) * 4;
    for (int guard = 0; offset && guard < 64; ++guard) {
        volatile u8 *cap = base + offset;
        u32 v = r32(cap, 0);
        if ((v & 0xff) == 1) {
            w32(cap, 0, v | 1u << 24);
            for (int i = 0; i < 100 && (r32(cap, 0) & (1u << 16)); ++i) cpu_sleep(10000000);
            w32(cap, 0, (r32(cap, 0) & ~(1u << 16)) | 1u << 24);
            w32(cap, 4, (r32(cap, 4) & 0x000e1fee) | 0xe0000000u);   /* SMIs off, clear status */
        }
        u32 next = (v >> 8) & 0xff;
        offset = next ? offset + next * 4 : 0;
    }
}

void xhci_probe(const struct pci_function *f) {
    int io;
    u64 bar = pci_bar(f, 0, &io);
    if (!bar || io) return;
    pci_enable(f, 1, 0);
    struct xhci *x = memory_alloc(sizeof(*x), 16, OWNER_KERNEL, 1);
    if (!x) return;
    x->cap = (volatile u8 *)(uptr)bar;
    u32 caplength = x->cap[0];
    x->op = x->cap + caplength;
    x->rt = x->cap + (r32(x->cap, 0x18) & ~0x1fu);
    x->db = (volatile u32 *)(x->cap + (r32(x->cap, 0x14) & ~3u));
    u32 hcs1 = r32(x->cap, 4), hcs2 = r32(x->cap, 8), hcc1 = r32(x->cap, 0x10);
    x->slots = (int)(hcs1 & 0xff);
    x->ports = (int)(hcs1 >> 24);
    x->context_size = hcc1 & 4 ? 64 : 32;
    if (x->ports > USB_MAX_PORTS) x->ports = USB_MAX_PORTS;
    if (x->slots > 64) x->slots = 64;
    take_ownership(x, hcc1);
    w32(x->op, 0, r32(x->op, 0) & ~1u);
    for (int i = 0; i < 100 && !(r32(x->op, 4) & 1); ++i) cpu_sleep(1000000);
    w32(x->op, 0, 2);
    for (int i = 0; i < 1000 && (r32(x->op, 0) & 2); ++i) cpu_sleep(1000000);
    for (int i = 0; i < 1000 && (r32(x->op, 4) & (1u << 11)); ++i) cpu_sleep(1000000);
    if (r32(x->op, 0) & 2) return;
    w32(x->op, 0x38, (u32)x->slots);
    x->dcbaa = dma_alloc(256 * 8, 4096);
    if (!x->dcbaa) return;
    u32 scratch = ((hcs2 >> 27) & 31) | ((hcs2 >> 16) & 0x3e0);
    if (scratch) {
        u64 *array = dma_alloc(scratch * 8, 4096);
        if (!array) return;
        for (u32 i = 0; i < scratch; ++i) {
            void *page = memory_alloc(4096, 4096, OWNER_KERNEL, 1);
            if (!page) return;
            array[i] = dma_address(page);
        }
        x->dcbaa[0] = dma_address(array);
    }
    w64(x->op, 0x30, dma_address(x->dcbaa));
    if (ring_init(&x->command, RING_TRBS)) return;
    w64(x->op, 0x18, dma_address(x->command.trbs) | 1);
    x->events = dma_alloc(EVENT_TRBS * 16, 4096);
    u32 *erst = dma_alloc(64, 64);
    if (!x->events || !erst) return;
    erst[0] = dma_address(x->events);
    erst[1] = 0;
    erst[2] = EVENT_TRBS;
    x->event_cycle = 1;
    w32(x->rt, 0x28, 1);                               /* ERSTSZ */
    w64(x->rt, 0x38, dma_address(x->events));         /* ERDP */
    w64(x->rt, 0x30, dma_address(erst));               /* ERSTBA */
    w32(x->rt, 0x20, 0);                               /* IMAN: no interrupts */
    w32(x->op, 0, 1);
    for (int i = 0; i < 100 && (r32(x->op, 4) & 1); ++i) cpu_sleep(1000000);
    if (hcc1 & 8)
        for (int p = 1; p <= x->ports; ++p) {
            u32 v = r32(x->op, PORTSC(x, p));
            w32(x->op, PORTSC(x, p), (v & PORT_KEEP & ~2u) | 1u << 9);
        }
    cpu_sleep(20000000);
    for (int p = 1; p <= x->ports; ++p) if (!x->port_major[p]) x->port_major[p] = 2;
    usb_add_hc(&ops, x, x->ports);
}
