/*
 * DOS-XC /libc/include/dosxc/driver.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * The DOS-XC device driver interface (driver ABI 1).  See docs/DRIVERS.md.
 *
 * A driver is a position-independent ELF shared object named *.DRV in
 * \SYSTEM\DRIVERS.  Build one with the DOS-XC cross compiler:
 *
 *   x86_64-dosxc-gcc -shared -fPIC -nostdlib -ffreestanding -mno-red-zone \
 *       -mno-sse -mno-mmx -O2 mydriver.c -o MYDRIVER.DRV
 *
 * It exports one symbol, dosxc_driver (use DK_DRIVER), and links against
 * nothing: every service comes through the struct dk_api table its load
 * callback receives (the compiler's memcpy, memset, memmove and memcmp
 * calls are resolved by the loader).  Drivers run in ring 0 and nothing interrupts them:
 * the system calls poll() often, so poll instead of waiting for an IRQ.
 * A processor exception inside a driver stops that driver only.
 */

#ifndef _DOSXC_DRIVER_H
#define _DOSXC_DRIVER_H

#include "abi.h"

#define DK_ABI_VERSION 1
#define DK_ANY 0xffff

/* Identifies a PCI function (the same layout the runtime uses). */
struct dk_pci_device {
    unsigned char bus, device, function, prog_if;
    unsigned short vendor, id;
};
/* A table of these ends with vendor 0.  DK_ANY matches any device id. */
struct dk_pci_id { unsigned short vendor, device; };

/* Network adapters (DK_NET_*). */
struct dk_net_ops {
    /* Send one Ethernet frame (header included, FCS excluded; at least 60
       bytes).  Copy it before returning.  Returns 0, or < 0 when full. */
    int (*transmit)(void *instance, const void *frame, unsigned length);
};

/* Disks (DK_DISK_*).  512-byte sectors. */
struct dk_disk_ops {
    /* Transfer COUNT sectors (at most 64); wait for it.  Returns 0 or < 0. */
    int (*io)(void *instance, dosxc_u64 lba, unsigned count, void *buffer, int write);
};
#define DK_DISK_READONLY 1

struct dk_api {
    dosxc_u32 version, size;

    /* Logging: %s %c %d %u %x %lx %02x style only.  log() goes to the
       driver log (DRIVERS /LOG); notice() to the user before the next prompt. */
    void (*log)(const char *format, ...) __attribute__((format(printf, 1, 2)));
    void (*notice)(const char *format, ...) __attribute__((format(printf, 1, 2)));
    int (*format)(char *out, unsigned long size, const char *format, ...) __attribute__((format(printf, 3, 4)));

    /* Time. */
    dosxc_u64 (*nanoseconds)(void);       /* monotonic, since boot */
    void (*sleep)(dosxc_u64 nanoseconds);

    /* Ordinary memory, zero-filled and page aligned. */
    void *(*alloc)(dosxc_size size);
    void (*free)(void *block);

    /* Memory devices may read and write: physically contiguous, zero-filled,
       page aligned (ALIGN may ask for more), below 4 GiB. */
    void *(*dma_alloc)(dosxc_size size, dosxc_size align);
    void (*dma_free)(void *block);
    dosxc_u64 (*dma_address)(const void *block);

    /* PCI configuration space. */
    unsigned char (*pci_read8)(const struct dk_pci_device *, unsigned char offset);
    unsigned short (*pci_read16)(const struct dk_pci_device *, unsigned char offset);
    dosxc_u32 (*pci_read32)(const struct dk_pci_device *, unsigned char offset);
    void (*pci_write8)(const struct dk_pci_device *, unsigned char offset, unsigned char value);
    void (*pci_write16)(const struct dk_pci_device *, unsigned char offset, unsigned short value);
    void (*pci_write32)(const struct dk_pci_device *, unsigned char offset, dosxc_u32 value);
    /* BAR INDEX: the address, 0 if absent; *io is set for an I/O port BAR. */
    dosxc_u64 (*pci_bar)(const struct dk_pci_device *, int index, int *io);
    /* Turns on bus mastering and memory and/or I/O decoding. */
    void (*pci_enable)(const struct dk_pci_device *, int memory, int io);

    /* A pointer for device registers at PHYSICAL (from pci_bar); 0 when the
       address cannot be mapped (above 4 GiB on a BIOS boot). */
    volatile void *(*mmio_map)(dosxc_u64 physical, dosxc_size size);

    /* Network adapters.  register returns a handle, or < 0.  NAME is shown
       by IPCONFIG; MAC is the adapter's address.  receive() takes one frame
       (FCS removed) and may be called only from poll().  link() reports
       the carrier and speed in Mbit/s whenever they change. */
    int (*net_register)(const struct dk_net_ops *ops, void *instance, const unsigned char mac[6], const char *name);
    void (*net_receive)(int handle, const void *frame, unsigned length);
    void (*net_link)(int handle, int up, unsigned megabits);
    void (*net_unregister)(int handle);

    /* Disks: register returns a handle or < 0, and mounts the disk's volumes. */
    int (*disk_register)(const struct dk_disk_ops *ops, void *instance, dosxc_u64 sectors, unsigned flags, const char *name);
    void (*disk_unregister)(int handle);

    /* Input.  USAGE is a HID keyboard usage (modifiers 0xe0..0xe7 too), from
       poll() only.  A key press also repeats if the device sends it again.
       DX and DY are relative; BUTTONS has bit 0 left, 1 right, 2 middle. */
    void (*input_key)(unsigned usage, int pressed);
    void (*input_mouse)(int dx, int dy, int wheel, unsigned buttons);

    /* Nonzero when firmware services are still running (a UEFI boot). */
    int (*firmware_services)(void);
};

struct dk_driver {
    dosxc_u32 abi;                        /* DK_ABI_VERSION */
    dosxc_u32 size;                       /* sizeof (struct dk_driver) */
    const char *name, *version, *description;
    const struct dk_pci_id *pci_ids;      /* devices to bind; 0: probe once, with a null device */

    /* Called once, first.  Keep API; return 0, or < 0 to refuse to load. */
    int (*load)(const struct dk_api *api);
    /* A matching device: set *instance and return 0, or < 0 to pass on it. */
    int (*probe)(const struct dk_pci_device *device, void **instance);
    /* Called often (every few hundred microseconds while programs run). */
    void (*poll)(void *instance);
    /* Stop the device (no more DMA) and free what probe took. */
    void (*remove)(void *instance);
    /* Called last, when unloading; may be 0. */
    void (*unload)(void);
};

#define DK_DRIVER __attribute__((visibility("default"))) const struct dk_driver dosxc_driver

/* Device registers. */
static __inline__ dosxc_u32 dk_read32(volatile void *base, unsigned offset) {
    return *(volatile dosxc_u32 *)((volatile char *)base + offset);
}
static __inline__ void dk_write32(volatile void *base, unsigned offset, dosxc_u32 value) {
    *(volatile dosxc_u32 *)((volatile char *)base + offset) = value;
}
static __inline__ unsigned short dk_read16(volatile void *base, unsigned offset) {
    return *(volatile unsigned short *)((volatile char *)base + offset);
}
static __inline__ void dk_write16(volatile void *base, unsigned offset, unsigned short value) {
    *(volatile unsigned short *)((volatile char *)base + offset) = value;
}
static __inline__ unsigned char dk_read8(volatile void *base, unsigned offset) {
    return *(volatile unsigned char *)((volatile char *)base + offset);
}
static __inline__ void dk_write8(volatile void *base, unsigned offset, unsigned char value) {
    *(volatile unsigned char *)((volatile char *)base + offset) = value;
}
static __inline__ unsigned char dk_inb(unsigned short port) {
    unsigned char v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(port)); return v;
}
static __inline__ unsigned short dk_inw(unsigned short port) {
    unsigned short v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(port)); return v;
}
static __inline__ dosxc_u32 dk_inl(unsigned short port) {
    dosxc_u32 v; __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(port)); return v;
}
static __inline__ void dk_outb(unsigned short port, unsigned char v) { __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(port)); }
static __inline__ void dk_outw(unsigned short port, unsigned short v) { __asm__ volatile("outw %0,%1" ::"a"(v), "Nd"(port)); }
static __inline__ void dk_outl(unsigned short port, dosxc_u32 v) { __asm__ volatile("outl %0,%1" ::"a"(v), "Nd"(port)); }
/* Orders memory writes the device reads against the register write. */
static __inline__ void dk_barrier(void) { __asm__ volatile("mfence" ::: "memory"); }

#endif
