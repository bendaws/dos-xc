/*
 * DOS-XC HYPERV.DRV - shared declarations
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * VMBus and the Hyper-V synthetic devices that run on it.  Everything is
 * polled: DOS-XC has no interrupts for drivers.
 */
#ifndef HYPERV_H
#define HYPERV_H

#include <dosxc/driver.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef dosxc_u32 u32;
typedef dosxc_u64 u64;
typedef __SIZE_TYPE__ size_t_hv;

extern const struct dk_api *dk;

/* The loader resolves these for a driver; there is no C library. */
#define memcpy __builtin_memcpy
#define memset __builtin_memset
#define memcmp __builtin_memcmp

#define HV_PAGE 4096u
#define PACKED __attribute__((packed))

/* A GUID's in-memory form. */
#define HV_GUID(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) \
    {(a) & 255, ((a) >> 8) & 255, ((a) >> 16) & 255, (unsigned)(a) >> 24, (b) & 255, (b) >> 8, (c) & 255, (c) >> 8, \
     d0, d1, d2, d3, d4, d5, d6, d7}

/* VMBus packet types and flags. */
#define VM_PKT_DATA_INBAND 6
#define VM_PKT_DATA_USING_XFER_PAGES 7
#define VM_PKT_DATA_USING_GPA_DIRECT 9
#define VM_PKT_COMP 11
#define VMBUS_COMPLETION_REQUESTED 1

struct vmbus_ring;

struct vmbus_channel {
    int used, open;
    u8 type[16], instance[16];
    u32 relid, connection;
    u32 ring_gpadl;
    u8 *ring_memory;
    u32 ring_bytes;
    volatile struct vmbus_ring *out, *in;
    u32 out_size, in_size;           /* bytes of data in each ring */
};

struct vmbus_packet {
    u16 type;
    u32 total;                       /* bytes including the descriptor */
    u32 data_offset;                 /* where the payload starts */
    u64 transaction;
};

int vmbus_start(void);
void vmbus_stop(void);
void vmbus_poll(void);
/* The INDEX-th channel offered with this device type, or 0. */
struct vmbus_channel *vmbus_find(const u8 type[16], int index);
int vmbus_open(struct vmbus_channel *c, u32 ring_bytes);
void vmbus_close(struct vmbus_channel *c);
/* Shares BUFFER (from dma_alloc, page aligned) with the host. */
int vmbus_gpadl(struct vmbus_channel *c, void *buffer, u32 bytes, u32 *handle);
void vmbus_gpadl_free(struct vmbus_channel *c, u32 handle);
int vmbus_send(struct vmbus_channel *c, const void *data, u32 length, u64 transaction, u16 type, u16 flags);
/* An in-band payload describing BYTES at BUFFER (contiguous memory). */
int vmbus_send_gpa(struct vmbus_channel *c, const void *buffer, u32 bytes, const void *data, u32 length, u64 transaction);
/* Copies the next packet into OUT.  Returns 1, 0 when there is none, or < 0
   when it was larger than CAPACITY (it is discarded). */
int vmbus_read(struct vmbus_channel *c, void *out, u32 capacity, struct vmbus_packet *packet);

int hv_net_start(void);
void hv_net_poll(void);
void hv_net_stop(void);
int hv_storage_start(void);
void hv_storage_poll(void);
void hv_storage_stop(void);
int hv_keyboard_start(void);
void hv_keyboard_poll(void);
int hv_mouse_start(void);
void hv_mouse_poll(void);
void hv_input_stop(void);

#endif
