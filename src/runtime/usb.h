/*
 * DOS-XC /runtime/usb.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * The native USB stack (BIOS boot).  Everything is polled: there are no
 * interrupts, so usb_poll() runs from the console input loops, from system
 * calls (at most every few milliseconds) and from the 16-bit bridge.
 *
 *   pci.c          finds the host controllers
 *   uhci.c ohci.c ehci.c xhci.c   host controller drivers
 *   usb.c          devices, enumeration, hubs, hot plug, keyboards, mice,
 *                  mass storage
 *   bridge.c       INT 13h/16h/33h for 16-bit programs (src/dos/bridge.asm)
 */

#pragma once
#include "runtime.h"

/* ---- PCI ---------------------------------------------------------------- */
struct pci_function { u8 bus, device, function, prog_if; u16 vendor, id; };
u32 pci_read32(const struct pci_function *f, u8 offset);
void pci_write32(const struct pci_function *f, u8 offset, u32 value);
u16 pci_read16(const struct pci_function *f, u8 offset);
void pci_write16(const struct pci_function *f, u8 offset, u16 value);
u8 pci_read8(const struct pci_function *f, u8 offset);
void pci_write8(const struct pci_function *f, u8 offset, u8 value);
/* Memory BAR address (0 when absent or above 4 GiB); *io set for I/O BARs. */
u64 pci_bar(const struct pci_function *f, int index, int *io);
u64 pci_bar64(const struct pci_function *f, int index, int *io);   /* no 4 GiB limit */
void pci_enable(const struct pci_function *f, int memory, int io);
typedef void (*pci_found_fn)(const struct pci_function *f);
void pci_scan_class(u8 class_code, u8 subclass, pci_found_fn found);

/* ---- DMA memory ------------------------------------------------------- */
void *dma_alloc(u32 size, u32 align);   /* zeroed; below 4 GiB, identity mapped */
void dma_free(void *block, u32 size);
static inline u32 dma_address(const void *p) { return (u32)(uptr)p; }

/* ---- the USB model ---------------------------------------------------- */
#define USB_SPEED_LOW 1
#define USB_SPEED_FULL 2
#define USB_SPEED_HIGH 3
#define USB_SPEED_SUPER 4

#define USB_OK 0
#define USB_ESTALL (-1)
#define USB_ETIMEOUT (-2)
#define USB_EIO (-3)
#define USB_ENODEV (-4)
#define USB_ENOMEM (-5)
#define USB_EAGAIN (-6)       /* interrupt transfer still pending */
#define USB_EBABBLE (-7)

#define USB_PORT_CONNECTED 1
#define USB_PORT_ENABLED 2
#define USB_PORT_CHANGED 4    /* connection changed since the last look */
#define USB_PORT_SPEED(status) (((status) >> 8) & 7)

#define USB_ENDPOINTS 32      /* index: number + (in ? 16 : 0) */

struct usb_setup { u8 type, request; u16 value, index, length; } __attribute__((packed));

struct usb_endpoint {
    u8 address;               /* bit 7: IN */
    u8 attributes;            /* 2 bulk, 3 interrupt */
    u16 max_packet;
    u8 interval;              /* bInterval as in the descriptor */
};

struct usb_hc;
struct usb_device;

/* An interrupt IN pipe kept busy by the controller driver. */
struct usb_pipe {
    struct usb_device *device;
    struct usb_endpoint endpoint;
    u8 *buffer;               /* DMA buffer, endpoint.max_packet bytes */
    void *hc_data;
};

struct usb_hc_ops {
    const char *name;
    int (*port_count)(struct usb_hc *);
    int (*port_status)(struct usb_hc *, int port);           /* USB_PORT_* | speed << 8 */
    int (*port_reset)(struct usb_hc *, int port);            /* returns the speed, or < 0 */
    void (*port_disable)(struct usb_hc *, int port);
    int (*open)(struct usb_device *);                        /* default control pipe, address 0 */
    int (*address)(struct usb_device *, int address);        /* SET_ADDRESS (or xHCI's own) */
    int (*update_control)(struct usb_device *);              /* max_packet0 changed */
    int (*control)(struct usb_device *, const struct usb_setup *, void *data);
    int (*configure)(struct usb_device *, const struct usb_endpoint *, int count);
    int (*bulk)(struct usb_device *, const struct usb_endpoint *, void *data, u32 length, u32 timeout_ms);
    int (*reset_endpoint)(struct usb_device *, const struct usb_endpoint *);
    int (*interrupt_open)(struct usb_pipe *);
    int (*interrupt_poll)(struct usb_pipe *);                /* bytes, USB_EAGAIN or error */
    void (*interrupt_close)(struct usb_pipe *);
    int (*hub_info)(struct usb_device *, int ports, int think_time, int multi_tt);
    void (*close)(struct usb_device *);
    void (*poll)(struct usb_hc *);
    void (*shutdown)(struct usb_hc *);
};

#define USB_MAX_PORTS 16
struct usb_hc {
    int used;
    const struct usb_hc_ops *ops;
    void *data;
    struct usb_device *root[USB_MAX_PORTS];   /* device on each root port */
    int ports;
    u8 next_address;
    u8 addresses[16];                         /* bitmap of addresses in use */
};

#define USB_CLASS_NONE 0
#define USB_CLASS_HUB 1
#define USB_CLASS_KEYBOARD 2
#define USB_CLASS_MOUSE 3
#define USB_CLASS_STORAGE 4

struct usb_device {
    int used;
    struct usb_hc *hc;
    struct usb_device *parent;        /* hub, or 0 on a root port */
    int port;                         /* 1-based port on the parent / root hub */
    int speed, address, depth;
    u32 route;                        /* xHCI route string */
    struct usb_device *tt_hub;        /* high-speed hub doing the split transactions */
    int tt_port;
    u16 max_packet0;
    u16 vendor, product;
    u8 toggle[USB_ENDPOINTS];         /* data toggles (drivers that keep them in software) */
    void *hc_data;
    int kind;                         /* USB_CLASS_* */
    void *class_data;
    u8 interface;                     /* interface the class driver uses */
    u8 hub_ports;
    int gone;                         /* unplugged: fail every transfer */
};

static inline int usb_endpoint_index(u8 address) { return (address & 15) + (address & 0x80 ? 16 : 0); }

/* usb.c */
void usb_init(void);
void usb_poll(void);                  /* hot plug and input; cheap when called often */
void usb_tick(void);                  /* usb_poll() at most every 4 ms */
void usb_shutdown(void);
int usb_active(void);
int usb_busy(void);                   /* inside the USB code (the bridge must not enter) */
int usb_keyboards(void);
int usb_mice(void);
struct usb_hc *usb_add_hc(const struct usb_hc_ops *ops, void *data, int ports);
int usb_control(struct usb_device *, u8 type, u8 request, u16 value, u16 index, void *data, u16 length);

/* Keyboard and mouse input gathered by usb.c. */
int usb_key_read(void);               /* runtime key code (see api.h), or -1 */
int usb_key_bios(void);               /* BIOS INT 16h word (scan << 8 | ascii), or -1 */
u8 usb_shift_flags(void);             /* BIOS 0x417 bits */
struct usb_mouse_state { int dx, dy, wheel; u8 buttons; int present; };
int usb_mouse_take(struct usb_mouse_state *out);  /* accumulated motion since the last call */

/* Mass storage, for volumes.c and the bridge. */
#define USB_DISKS 8
struct usb_disk_info { u64 sectors; int present, removable, readonly; u32 serial; };
int usb_disk_info(int disk, struct usb_disk_info *out);
int usb_disk_io(int disk, u64 lba, unsigned count, void *buffer, int write);
int usb_disk_eject(int disk);
/* volumes.c is told about disks coming and going. */
void volumes_usb_changed(int disk, int present);

/* host controller probes */
/* firmware hand-offs: every controller, before any of them is started */
void ohci_handoff(const struct pci_function *f);
void ehci_handoff(const struct pci_function *f);
void xhci_handoff(const struct pci_function *f);
void uhci_probe(const struct pci_function *f);
void ohci_probe(const struct pci_function *f);
void ehci_probe(const struct pci_function *f);
void xhci_probe(const struct pci_function *f);
void ehci_companions_ready(void);

/* bridge.c */
void bridge_install(void);
void bridge_set_drive(u8 bios_drive, int usb_disk);
void bridge_runtime_active(int active);   /* 0 while 16-bit code owns the machine */
