/*
 * DOS-XC /runtime/ehci.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * EHCI (USB 2) host controller driver, polled.  Control and bulk queue
 * heads stay on the asynchronous schedule; interrupt queue heads hang off
 * one anchor that every periodic frame list entry points to (so they are
 * polled every frame).  Full- and low-speed devices on root ports are
 * handed to the companion UHCI/OHCI controllers; behind high-speed hubs
 * they are reached with split transactions.
 */

#include "usb.h"

struct qtd {
    volatile u32 next, alternate, token, buffer[5], buffer_high[5];
    u32 length, pad[2];
};
struct qh {
    volatile u32 link, characteristics, capabilities, current;
    volatile u32 next, alternate, token, buffer[5], buffer_high[5];
    u32 pad[3];
};

#define QTD_ACTIVE 0x80u
#define QTD_HALTED 0x40u
#define QTD_ERRORS 0x7cu
#define T 1u

struct edev {
    struct qh *control;
    struct qh *endpoints[USB_ENDPOINTS];
};

struct epipe { struct qh *qh; struct qtd *qtd; };

struct ehci {
    volatile u8 *cap, *op;
    int ports, companions;
    u32 *frames;
    struct qh *async, *periodic;
    struct qtd *dummy;
};

static u32 r32(volatile u8 *b, u32 o) { return *(volatile u32 *)(b + o); }
static void w32(volatile u8 *b, u32 o, u32 v) { *(volatile u32 *)(b + o) = v; }

static struct qh *new_qh(void) { return dma_alloc(sizeof(struct qh), 64); }
static struct qtd *new_qtd(void) { return dma_alloc(sizeof(struct qtd), 64); }

static void fill_buffers(struct qtd *t, u32 address, u32 length) {
    t->buffer[0] = address;
    u32 page = address & ~0xfffu;
    for (int i = 1; i < 5; ++i) { page += 4096; t->buffer[i] = page; }
    t->length = length;
}

/* Endpoint characteristics and capabilities of a queue head. */
static void describe(struct qh *q, struct usb_device *d, int endpoint, u32 max_packet, int control, int interrupt) {
    u32 eps = d->speed == USB_SPEED_HIGH ? 2 : d->speed == USB_SPEED_LOW ? 1 : 0;
    q->characteristics = (u32)(d->address & 0x7f) | (u32)endpoint << 8 | eps << 12 | (control ? 1u << 14 : 0) |
                         max_packet << 16 | (control && eps != 2 ? 1u << 27 : 0) | (interrupt ? 0 : 4u << 28);
    u32 caps = 1u << 30;
    if (eps != 2 && d->tt_hub) caps |= (u32)(d->tt_hub->address & 0x7f) << 16 | (u32)(d->tt_port & 0x7f) << 23;
    if (interrupt) caps |= eps == 2 ? 0x01u : 0x1c01u;
    q->capabilities = caps;
}

static void async_link(struct ehci *e, struct qh *q) {
    q->link = e->async->link;
    __asm__ volatile("" ::: "memory");
    e->async->link = dma_address(q) | 2;
}

/* Take Q off the asynchronous schedule (interrupt-on-async-advance handshake). */
static void async_unlink(struct ehci *e, struct qh *q) {
    struct qh *p = e->async;
    for (int guard = 0; guard < 1024; ++guard) {
        struct qh *n = (struct qh *)(uptr)(p->link & ~31u);
        if (n == q) { p->link = q->link; break; }
        if (n == e->async) return;
        p = n;
    }
    w32(e->op, 4, 1u << 5);
    w32(e->op, 0, r32(e->op, 0) | 1u << 6);
    for (int i = 0; i < 100 && !(r32(e->op, 4) & (1u << 5)); ++i) cpu_sleep(100000);
    w32(e->op, 4, 1u << 5);
}

static void periodic_link(struct ehci *e, struct qh *q) {
    q->link = e->periodic->link;
    __asm__ volatile("" ::: "memory");
    e->periodic->link = dma_address(q) | 2;
}

static void periodic_unlink(struct ehci *e, struct qh *q) {
    struct qh *p = e->periodic;
    for (int guard = 0; guard < 256 && !(p->link & T); ++guard) {
        struct qh *n = (struct qh *)(uptr)(p->link & ~31u);
        if (n == q) { p->link = q->link; break; }
        p = n;
    }
    cpu_sleep(2000000);                   /* two frames: the controller lets go */
}

/* Run a chain of qTDs on Q and wait.  Returns bytes moved or an error. */
/* A short packet ends a bulk transfer (the controller moves to the
   inactive dummy); in a control transfer it skips to the status stage. */
static int run(struct ehci *e, struct qh *q, struct qtd **chain, int count, u32 timeout_ms, int control) {
    for (int i = 0; i < count; ++i) {
        chain[i]->next = i + 1 < count ? dma_address(chain[i + 1]) : T;
        chain[i]->alternate = control ? dma_address(chain[count - 1]) : dma_address(e->dummy);
    }
    __asm__ volatile("" ::: "memory");
    q->token &= 1u << 31;                 /* overlay idle and not halted; keep the toggle */
    q->next = dma_address(chain[0]);
    q->alternate = T;
    u64 end = cpu_nanoseconds() + (u64)timeout_ms * 1000000ull;
    int result = 0;
    for (;;) {
        u32 last = chain[count - 1]->token;
        int halted = 0, active = 0;
        for (int i = 0; i < count; ++i) {
            u32 tk = chain[i]->token;
            if (tk & QTD_HALTED) halted = 1;
            if (tk & QTD_ACTIVE) active = 1;
        }
        if (halted) {
            u32 tk = 0;
            for (int i = 0; i < count; ++i) if (chain[i]->token & QTD_HALTED) tk = chain[i]->token;
            result = tk & 0x10 ? USB_EBABBLE : (tk & 0x28) ? USB_EIO : USB_ESTALL;
            break;
        }
        if (!(last & QTD_ACTIVE)) break;
        /* A short packet sent the controller to the dummy: done early. */
        if (active && (q->current & ~31u) == dma_address(e->dummy) && !(q->token & QTD_ACTIVE)) break;
        if (cpu_nanoseconds() > end) { result = USB_ETIMEOUT; break; }
    }
    if (result == USB_ETIMEOUT || result < 0) {
        /* Take the queue head off the schedule to stop it, then clear it. */
        int periodic = (q->capabilities & 0xff) != 0;
        if (periodic) periodic_unlink(e, q); else async_unlink(e, q);
        q->next = T;
        q->alternate = T;
        q->token = 0;
        q->current = 0;
        if (periodic) periodic_link(e, q); else async_link(e, q);
        return result;
    }
    int moved = 0;
    for (int i = 0; i < count; ++i) {
        u32 tk = chain[i]->token;
        if (tk & QTD_ACTIVE) break;
        moved += (int)(chain[i]->length - ((tk >> 16) & 0x7fff));
    }
    q->next = T;
    return moved;
}

static struct qtd *make_qtd(u32 pid, u32 address, u32 length, int toggle) {
    struct qtd *t = new_qtd();
    if (!t) return 0;
    fill_buffers(t, address, length);
    t->token = QTD_ACTIVE | pid << 8 | 3u << 10 | length << 16 | (toggle ? 1u << 31 : 0);
    return t;
}

static int ehci_control(struct usb_device *d, const struct usb_setup *setup, void *data) {
    struct ehci *e = d->hc->data;
    struct edev *x = d->hc_data;
    if (!x || d->gone) return USB_ENODEV;
    static u8 *setup_buffer;
    if (!setup_buffer && !(setup_buffer = dma_alloc(8, 64))) return USB_ENOMEM;
    memcpy(setup_buffer, setup, 8);
    struct qtd *chain[3];
    int count = 0, in = setup->type & 0x80;
    chain[count++] = make_qtd(2, dma_address(setup_buffer), 8, 0);
    if (setup->length) chain[count++] = make_qtd(in ? 1 : 0, dma_address(data), setup->length, 1);
    chain[count++] = make_qtd(setup->length && in ? 0 : 1, 0, 0, 1);
    int r = USB_ENOMEM;
    if (chain[0] && chain[count - 1] && (count == 2 || chain[1])) {
        chain[count - 1]->token |= 1u << 15;
        r = run(e, x->control, chain, count, 2000, 1);
        if (r >= 0) r = setup->length ? (int)(chain[1]->length - ((chain[1]->token >> 16) & 0x7fff)) : 0;
    }
    for (int i = 0; i < count; ++i) dma_free(chain[i], sizeof(struct qtd));
    return r;
}

static int ehci_open(struct usb_device *d) {
    struct ehci *e = d->hc->data;
    struct edev *x = memory_alloc(sizeof(*x), 16, OWNER_KERNEL, 1);
    if (!x) return -1;
    d->hc_data = x;
    x->control = new_qh();
    if (!x->control) return -1;
    describe(x->control, d, 0, d->max_packet0, 1, 0);
    x->control->next = T;
    x->control->alternate = T;
    async_link(e, x->control);
    return 0;
}

static int ehci_address(struct usb_device *d, int address) {
    if (usb_control(d, 0x00, 5, (u16)address, 0, 0, 0) < 0) return -1;
    cpu_sleep(2000000);
    struct edev *x = d->hc_data;
    d->address = address;
    x->control->characteristics = (x->control->characteristics & ~0x7fu) | (u32)address;
    return 0;
}

static int ehci_update_control(struct usb_device *d) {
    struct edev *x = d->hc_data;
    x->control->characteristics = (x->control->characteristics & ~(0x7ffu << 16)) | (u32)d->max_packet0 << 16;
    return 0;
}

static int ehci_configure(struct usb_device *d, const struct usb_endpoint *eps, int count) {
    struct ehci *e = d->hc->data;
    struct edev *x = d->hc_data;
    for (int i = 0; i < count; ++i) {
        if ((eps[i].attributes & 3) != 2) continue;     /* interrupt pipes get theirs later */
        int index = usb_endpoint_index(eps[i].address);
        if (x->endpoints[index]) continue;
        struct qh *q = new_qh();
        if (!q) return -1;
        describe(q, d, eps[i].address & 15, eps[i].max_packet & 0x7ff, 0, 0);
        q->next = T;
        q->alternate = T;
        x->endpoints[index] = q;
        async_link(e, q);
    }
    return 0;
}

static int ehci_bulk(struct usb_device *d, const struct usb_endpoint *ep, void *data, u32 length, u32 timeout) {
    struct ehci *e = d->hc->data;
    struct edev *x = d->hc_data;
    if (!x || d->gone) return USB_ENODEV;
    struct qh *q = x->endpoints[usb_endpoint_index(ep->address)];
    if (!q) return USB_EIO;
    struct qtd *chain[8];
    int count = 0;
    u32 address = dma_address(data), left = length, pid = ep->address & 0x80 ? 1 : 0;
    do {
        u32 room = 5 * 4096 - (address & 0xfff);
        u32 chunk = left < room ? left : room;
        if (chunk < left) chunk -= chunk % (ep->max_packet ? ep->max_packet : 512);
        if (count == 8 || !(chain[count] = make_qtd(pid, address, chunk, 0))) {
            for (int i = 0; i < count; ++i) dma_free(chain[i], sizeof(struct qtd));
            return USB_ENOMEM;
        }
        ++count;
        address += chunk;
        left -= chunk;
    } while (left);
    chain[count - 1]->token |= 1u << 15;
    int r = run(e, q, chain, count, timeout, 0);
    for (int i = 0; i < count; ++i) dma_free(chain[i], sizeof(struct qtd));
    return r;
}

static int ehci_reset_endpoint(struct usb_device *d, const struct usb_endpoint *ep) {
    struct edev *x = d->hc_data;
    if (!x) return -1;
    struct qh *q = x->endpoints[usb_endpoint_index(ep->address)];
    if (q) { q->token &= ~(1u << 31 | QTD_HALTED | QTD_ACTIVE); q->next = T; }
    return 0;
}

static void queue_interrupt(struct usb_pipe *pipe, struct epipe *p) {
    struct qtd *t = p->qtd;
    u32 length = pipe->endpoint.max_packet;
    fill_buffers(t, dma_address(pipe->buffer), length);
    t->next = T;
    t->alternate = T;
    __asm__ volatile("" ::: "memory");
    t->token = QTD_ACTIVE | 1u << 8 | 3u << 10 | length << 16;
    p->qh->next = dma_address(t);
}

static int ehci_interrupt_open(struct usb_pipe *pipe) {
    struct usb_device *d = pipe->device;
    struct ehci *e = d->hc->data;
    struct epipe *p = memory_alloc(sizeof(*p), 16, OWNER_KERNEL, 1);
    if (!p) return -1;
    pipe->buffer = dma_alloc(pipe->endpoint.max_packet ? pipe->endpoint.max_packet : 8, 64);
    p->qh = new_qh();
    p->qtd = new_qtd();
    if (!pipe->buffer || !p->qh || !p->qtd) return -1;
    describe(p->qh, d, pipe->endpoint.address & 15, pipe->endpoint.max_packet & 0x7ff, 0, 1);
    p->qh->alternate = T;
    pipe->hc_data = p;
    queue_interrupt(pipe, p);
    periodic_link(e, p->qh);
    return 0;
}

static int ehci_interrupt_poll(struct usb_pipe *pipe) {
    struct epipe *p = pipe->hc_data;
    if (!p) return USB_EIO;
    u32 tk = p->qtd->token;
    if (tk & QTD_ACTIVE) return USB_EAGAIN;
    int r = tk & QTD_HALTED ? USB_EIO : (int)(pipe->endpoint.max_packet - ((tk >> 16) & 0x7fff));
    if (tk & QTD_HALTED) p->qh->token &= ~QTD_HALTED;
    queue_interrupt(pipe, p);
    return r;
}

static void ehci_interrupt_close(struct usb_pipe *pipe) {
    struct ehci *e = pipe->device->hc->data;
    struct epipe *p = pipe->hc_data;
    if (!p) return;
    periodic_unlink(e, p->qh);
    dma_free(p->qh, sizeof(struct qh));
    dma_free(p->qtd, sizeof(struct qtd));
    dma_free(pipe->buffer, pipe->endpoint.max_packet ? pipe->endpoint.max_packet : 8);
    memory_free(p, OWNER_KERNEL);
    pipe->hc_data = 0;
}

static void ehci_close(struct usb_device *d) {
    struct ehci *e = d->hc->data;
    struct edev *x = d->hc_data;
    if (!x) return;
    if (x->control) { async_unlink(e, x->control); dma_free(x->control, sizeof(struct qh)); }
    for (int i = 0; i < USB_ENDPOINTS; ++i)
        if (x->endpoints[i]) { async_unlink(e, x->endpoints[i]); dma_free(x->endpoints[i], sizeof(struct qh)); }
    memory_free(x, OWNER_KERNEL);
    d->hc_data = 0;
}

/* ---- root hub ----------------------------------------------------------- */

#define PORTSC(p) (0x44 + 4 * ((p) - 1))
#define PORT_CLEAR 0x2au                      /* write-1-to-clear bits */

static int ehci_port_count(struct usb_hc *hc) { return ((struct ehci *)hc->data)->ports; }

static int ehci_port_status(struct usb_hc *hc, int port) {
    struct ehci *e = hc->data;
    u32 v = r32(e->op, PORTSC(port));
    if (v & (1u << 13)) return 0;             /* owned by a companion */
    int status = 0;
    if (v & 1) status |= USB_PORT_CONNECTED;
    if (v & 4) status |= USB_PORT_ENABLED;
    if (v & 2) {
        status |= USB_PORT_CHANGED;
        w32(e->op, PORTSC(port), (v & ~PORT_CLEAR) | 2);
    }
    return status | USB_SPEED_HIGH << 8;
}

static void release(struct ehci *e, int port) {
    u32 v = r32(e->op, PORTSC(port));
    w32(e->op, PORTSC(port), (v & ~PORT_CLEAR) | 1u << 13);
}

static int ehci_port_reset(struct usb_hc *hc, int port) {
    struct ehci *e = hc->data;
    u32 v = r32(e->op, PORTSC(port));
    if (((v >> 10) & 3) == 1) {               /* K state: low speed */
        if (e->companions) { release(e, port); return 0; }
        return -1;
    }
    w32(e->op, PORTSC(port), (v & ~(PORT_CLEAR | 4u)) | 1u << 8);
    cpu_sleep(50000000);
    v = r32(e->op, PORTSC(port));
    w32(e->op, PORTSC(port), v & ~(PORT_CLEAR | 1u << 8));
    for (int i = 0; i < 100 && (r32(e->op, PORTSC(port)) & (1u << 8)); ++i) cpu_sleep(100000);
    cpu_sleep(5000000);
    v = r32(e->op, PORTSC(port));
    w32(e->op, PORTSC(port), (v & ~PORT_CLEAR) | (v & PORT_CLEAR));    /* clear changes */
    if (!(v & 1)) return -1;
    if (!(v & 4)) {                           /* full speed: not ours */
        if (e->companions) { release(e, port); return 0; }
        return -1;
    }
    return USB_SPEED_HIGH;
}

static void ehci_port_disable(struct usb_hc *hc, int port) {
    struct ehci *e = hc->data;
    u32 v = r32(e->op, PORTSC(port));
    w32(e->op, PORTSC(port), v & ~(PORT_CLEAR | 4u));
}

static void ehci_shutdown(struct usb_hc *hc) {
    struct ehci *e = hc->data;
    w32(e->op, 0, r32(e->op, 0) & ~1u);
    for (int i = 0; i < 100 && !(r32(e->op, 4) & (1u << 12)); ++i) cpu_sleep(100000);
    w32(e->op, 0x40, 0);                      /* ports back to the companions */
}

static const struct usb_hc_ops ops = {
    "EHCI", ehci_port_count, ehci_port_status, ehci_port_reset, ehci_port_disable,
    ehci_open, ehci_address, ehci_update_control, ehci_control, ehci_configure, ehci_bulk,
    ehci_reset_endpoint, ehci_interrupt_open, ehci_interrupt_poll, ehci_interrupt_close,
    0, ehci_close, 0, ehci_shutdown,
};

/* Take the controller from the firmware (USBLEGSUP in configuration space). */
void ehci_handoff(const struct pci_function *f) {
    int io;
    u64 bar = pci_bar(f, 0, &io);
    if (!bar || io) return;
    pci_enable(f, 1, 0);
    u8 eecp = (u8)(r32((volatile u8 *)(uptr)bar, 8) >> 8);
    for (int guard = 0; eecp >= 0x40 && guard < 16; ++guard) {
        u32 v = pci_read32(f, eecp);
        if ((v & 0xff) == 1) {
            pci_write32(f, eecp, v | 1u << 24);
            for (int i = 0; i < 100 && (pci_read32(f, eecp) & (1u << 16)); ++i) cpu_sleep(10000000);
            pci_write32(f, eecp, (pci_read32(f, eecp) & ~(1u << 16)) | 1u << 24);
            pci_write32(f, (u8)(eecp + 4), 0xe0000000u);
        }
        eecp = (u8)(v >> 8);
    }
}

void ehci_probe(const struct pci_function *f) {
    int io;
    u64 bar = pci_bar(f, 0, &io);
    if (!bar || io) return;
    pci_enable(f, 1, 0);
    struct ehci *e = memory_alloc(sizeof(*e), 16, OWNER_KERNEL, 1);
    if (!e) return;
    e->cap = (volatile u8 *)(uptr)bar;
    e->op = e->cap + e->cap[0];
    u32 hcs = r32(e->cap, 4), hcc = r32(e->cap, 8);
    e->ports = (int)(hcs & 15);
    e->companions = (int)((hcs >> 12) & 15);
    /* Stop and reset. */
    w32(e->op, 0, r32(e->op, 0) & ~1u);
    for (int i = 0; i < 100 && !(r32(e->op, 4) & (1u << 12)); ++i) cpu_sleep(100000);
    w32(e->op, 0, 2);
    for (int i = 0; i < 100 && (r32(e->op, 0) & 2); ++i) cpu_sleep(1000000);
    w32(e->op, 8, 0);                         /* no interrupts */
    e->frames = dma_alloc(4096, 4096);
    e->async = new_qh();
    e->periodic = new_qh();
    e->dummy = new_qtd();
    if (!e->frames || !e->async || !e->periodic || !e->dummy) return;
    e->dummy->next = T;
    e->dummy->alternate = T;
    e->dummy->token = 0;                      /* inactive: the queue head stops there */
    e->async->link = dma_address(e->async) | 2;
    e->async->characteristics = 1u << 15;     /* head of reclamation list */
    e->async->next = T;
    e->async->alternate = T;
    e->async->token = QTD_HALTED;
    e->periodic->link = T;
    e->periodic->capabilities = 1u << 30 | 1;  /* a zero S-mask is undefined in the periodic list */
    e->periodic->next = T;
    e->periodic->alternate = T;
    e->periodic->token = QTD_HALTED;
    for (int i = 0; i < 1024; ++i) e->frames[i] = dma_address(e->periodic) | 2;
    if (hcc & 1) w32(e->op, 0x10, 0);
    w32(e->op, 0x14, dma_address(e->frames));
    w32(e->op, 0x18, dma_address(e->async));
    w32(e->op, 0, 8u << 16 | 1u << 5 | 1u << 4 | 1);   /* run, async + periodic schedules */
    w32(e->op, 0x40, 1);                       /* route every port to EHCI */
    cpu_sleep(5000000);
    if (hcs & 16)
        for (int p = 1; p <= e->ports; ++p) {
            u32 v = r32(e->op, PORTSC(p));
            w32(e->op, PORTSC(p), (v & ~PORT_CLEAR) | 1u << 12);
        }
    cpu_sleep(20000000);
    usb_add_hc(&ops, e, e->ports);
}
