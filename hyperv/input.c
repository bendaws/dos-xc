/*
 * DOS-XC /hyperv/input.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "hyperv.h"

static const u8 keyboard_guid[16] = HV_GUID(0xf912ad6d, 0x2b17, 0x48ea, 0xbd, 0x65, 0xf9, 0x27, 0xa6, 0x1c, 0x76, 0x84);
static const u8 mouse_guid[16] = HV_GUID(0xcfa8b69e, 0x5b4a, 0x4cc0, 0xb9, 0x8b, 0x8b, 0xa1, 0xa1, 0xf3, 0xf9, 0x5a);

#define RING_BYTES (40u * 1024)
#define WAIT_NS 5000000000ull
#define PACKET_BYTES 1024

#define KBD_PROTOCOL_REQUEST 1
#define KBD_PROTOCOL_RESPONSE 2
#define KBD_EVENT 3
#define KBD_VERSION 0x00010000u
#define KBD_ACCEPTED 1u
#define KEY_UNICODE 1u
#define KEY_BREAK 2u
#define KEY_E0 4u
#define KEY_E1 8u

static const u8 plain_usage[0x59] = {
    [0x01] = 0x29, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2d, 0x2e, 0x2a,
    [0x0f] = 0x2b, 0x14, 0x1a, 0x08, 0x15, 0x17, 0x1c, 0x18, 0x0c, 0x12, 0x13, 0x2f, 0x30, 0x28,
    [0x1d] = 0xe0, 0x04, 0x16, 0x07, 0x09, 0x0a, 0x0b, 0x0d, 0x0e, 0x0f, 0x33, 0x34, 0x35,
    [0x2a] = 0xe1, 0x31, 0x1d, 0x1b, 0x06, 0x19, 0x05, 0x11, 0x10, 0x36, 0x37, 0x38, 0xe5, 0x55, 0xe2, 0x2c, 0x39,
    [0x3b] = 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43,
    [0x45] = 0x53, 0x47, 0x5f, 0x60, 0x61, 0x56, 0x5c, 0x5d, 0x5e, 0x57, 0x59, 0x5a, 0x5b, 0x62, 0x63,
    [0x57] = 0x44, 0x45,
};

static u8 extended_usage(u8 scan) {
    switch (scan) {
    case 0x1c: return 0x58;
    case 0x1d: return 0xe4;
    case 0x35: return 0x54;
    case 0x37: return 0x46;
    case 0x38: return 0xe6;
    case 0x47: return 0x4a;
    case 0x48: return 0x52;
    case 0x49: return 0x4b;
    case 0x4b: return 0x50;
    case 0x4d: return 0x4f;
    case 0x4f: return 0x4d;
    case 0x50: return 0x51;
    case 0x51: return 0x4e;
    case 0x52: return 0x49;
    case 0x53: return 0x4c;
    case 0x5b: return 0xe3;
    case 0x5c: return 0xe7;
    }
    return 0;
}

static struct vmbus_channel *keyboard;
static u8 keyboard_packet[PACKET_BYTES];

static void keyboard_event(u32 make, u32 info) {
    if ((info & (KEY_UNICODE | KEY_E1)) || make > 0xff) return;
    u8 scan = (u8)make;
    u8 usage = (info & KEY_E0) ? extended_usage(scan) : scan < sizeof(plain_usage) ? plain_usage[scan] : 0;
    if (usage) dk->input_key(usage, !(info & KEY_BREAK));
}

/* The next packet's message type, or 0.  *DATA points at the message. */
static u32 keyboard_next(const u8 **data) {
    struct vmbus_packet p;
    for (;;) {
        int r = vmbus_read(keyboard, keyboard_packet, PACKET_BYTES, &p);
        if (r == 0) return 0;
        if (r < 0 || p.type != VM_PKT_DATA_INBAND || p.total - p.data_offset < 4) continue;
        *data = keyboard_packet + p.data_offset;
        return *(const u32 *)*data;
    }
}

int hv_keyboard_start(void) {
    keyboard = vmbus_find(keyboard_guid, 0);
    if (!keyboard || vmbus_open(keyboard, RING_BYTES)) return -1;
    u32 request[2] = {KBD_PROTOCOL_REQUEST, KBD_VERSION};
    if (vmbus_send(keyboard, request, sizeof(request), 1, VM_PKT_DATA_INBAND, VMBUS_COMPLETION_REQUESTED)) return -1;
    u64 end = dk->nanoseconds() + WAIT_NS;
    while (dk->nanoseconds() < end) {
        const u8 *data;
        u32 type = keyboard_next(&data);
        if (type == KBD_PROTOCOL_RESPONSE) {
            if (!(*(const u32 *)(data + 4) & KBD_ACCEPTED)) {
                dk->log("HYPERV KEYBOARD: protocol refused");
                return -1;
            }
            dk->log("HYPERV KEYBOARD: ready");
            return 0;
        }
        if (!type) __asm__ volatile("pause");
    }
    dk->log("HYPERV KEYBOARD: no response");
    return -1;
}

void hv_keyboard_poll(void) {
    if (!keyboard || !keyboard->open) return;
    const u8 *data;
    u32 type;
    while ((type = keyboard_next(&data))) {
        if (type == KBD_EVENT) keyboard_event(*(const u16 *)(data + 4), *(const u32 *)(data + 8));
    }
}

#define PIPE_DATA 1
#define HID_PROTOCOL_REQUEST 0
#define HID_PROTOCOL_RESPONSE 1
#define HID_DEVICE_INFO 2
#define HID_DEVICE_INFO_ACK 3
#define HID_INPUT_REPORT 4
#define HID_VERSION 0x00020000u
#define PIPE_HEADER 8
#define HID_HEADER 8
#define DEVICE_INFO_BYTES 32
#define HID_DESCRIPTOR_BYTES 9

#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 800

struct field { int present; u32 bit, bits; int absolute; u32 maximum; };

static struct vmbus_channel *mouse;
static u8 mouse_packet[PACKET_BYTES];
static struct field field_x, field_y, field_wheel, field_buttons;
static u32 report_id;
static int mouse_ready, have_position;
static u32 last_x, last_y;

/* Walk a report descriptor for the first input fields of each kind. */
static void parse_descriptor(const u8 *d, u32 length) {
    u32 usages[8], usage_count = 0, usage_page = 0, report_size = 0, report_count = 0, bit = 0, logical_max = 0;
    u32 usage_min = 0, usage_max = 0;
    int have_range = 0;
    report_id = 0;
    for (u32 i = 0; i < length;) {
        u8 item = d[i++];
        if (item == 0xfe) {                       /* a long item */
            if (i >= length) break;
            i += (u32)d[i] + 2;
            continue;
        }
        u32 size = item & 3, value = 0;
        if (size == 3) size = 4;
        if (i + size > length) break;
        for (u32 k = 0; k < size; ++k) value |= (u32)d[i + k] << (8 * k);
        i += size;
        switch (item & 0xfc) {
        case 0x04: usage_page = value; break;
        case 0x24: logical_max = value; break;
        case 0x74: report_size = value; break;
        case 0x94: report_count = value; break;
        case 0x84: if (!report_id) { report_id = value; bit = 8; } break;
        case 0x08: if (usage_count < 8) usages[usage_count++] = value; break;
        case 0x18: usage_min = value; have_range = 1; break;
        case 0x28: usage_max = value; break;
        case 0x80: {                              /* an Input item */
            for (u32 n = 0; n < report_count; ++n) {
                u32 usage = n < usage_count ? usages[n] : usage_count ? usages[usage_count - 1] : 0;
                if (!(value & 1) && usage_page == 1) {
                    struct field *f = usage == 0x30 ? &field_x : usage == 0x31 ? &field_y : usage == 0x38 ? &field_wheel : 0;
                    if (f && !f->present) {
                        f->present = 1;
                        f->bit = bit + n * report_size;
                        f->bits = report_size;
                        f->absolute = !(value & 4);
                        f->maximum = logical_max;
                    }
                }
            }
            if (!(value & 1) && usage_page == 9 && have_range && !field_buttons.present) {
                field_buttons.present = 1;
                field_buttons.bit = bit;
                field_buttons.bits = report_count * report_size;
            }
            bit += report_count * report_size;
            usage_count = 0;
            have_range = 0;
            (void)usage_min;
            (void)usage_max;
            break;
        }
        case 0xa0: case 0xc0: usage_count = 0; have_range = 0; break;
        }
    }
    dk->log("HYPERV MOUSE: x %u+%u (max %u), y %u+%u, wheel %u+%u, buttons %u+%u, report id %u", (unsigned)field_x.bit,
            (unsigned)field_x.bits, (unsigned)field_x.maximum, (unsigned)field_y.bit, (unsigned)field_y.bits,
            (unsigned)field_wheel.bit, (unsigned)field_wheel.bits, (unsigned)field_buttons.bit, (unsigned)field_buttons.bits,
            (unsigned)report_id);
}

static u32 read_bits(const u8 *r, u32 length, u32 bit, u32 bits) {
    u32 value = 0;
    for (u32 i = 0; i < bits && i < 32; ++i) {
        u32 at = bit + i;
        if (at / 8 >= length) break;
        value |= (u32)((r[at / 8] >> (at % 8)) & 1) << i;
    }
    return value;
}

static void mouse_report(const u8 *r, u32 length) {
    if (!field_x.present || !field_y.present || (report_id && (!length || r[0] != report_id))) return;
    u32 x = read_bits(r, length, field_x.bit, field_x.bits), y = read_bits(r, length, field_y.bit, field_y.bits);
    int dx, dy;
    if (field_x.absolute && field_x.maximum) {
        if (!have_position) { last_x = x; last_y = y; have_position = 1; }
        dx = ((int)x - (int)last_x) * SCREEN_WIDTH / (int)field_x.maximum;
        dy = ((int)y - (int)last_y) * SCREEN_HEIGHT / (int)(field_y.maximum ? field_y.maximum : field_x.maximum);
        last_x = x;
        last_y = y;
    } else {
        dx = (int)x << (32 - field_x.bits) >> (32 - field_x.bits);
        dy = (int)y << (32 - field_y.bits) >> (32 - field_y.bits);
    }
    int wheel = 0;
    if (field_wheel.present) {
        u32 w = read_bits(r, length, field_wheel.bit, field_wheel.bits);
        wheel = (int)w << (32 - field_wheel.bits) >> (32 - field_wheel.bits);
    }
    unsigned buttons = field_buttons.present ? read_bits(r, length, field_buttons.bit, field_buttons.bits) & 7 : 0;
    dk->input_mouse(dx, dy, wheel, buttons);
}

static int mouse_send(const void *body, u32 bytes) {
    u8 message[PIPE_HEADER + 32];
    *(u32 *)message = PIPE_DATA;
    *(u32 *)(message + 4) = bytes;
    memcpy(message + PIPE_HEADER, body, bytes);
    return vmbus_send(mouse, message, PIPE_HEADER + bytes, 1, VM_PKT_DATA_INBAND, VMBUS_COMPLETION_REQUESTED);
}

static void mouse_message(const u8 *m, u32 available) {
    if (available < HID_HEADER) return;
    u32 type = *(const u32 *)m, size = *(const u32 *)(m + 4);
    if (type == HID_DEVICE_INFO) {
        u32 at = HID_HEADER + DEVICE_INFO_BYTES;
        if (available < at + HID_DESCRIPTOR_BYTES) return;
        u32 descriptor = (u32)m[at + 7] | (u32)m[at + 8] << 8;
        at += m[at] ? m[at] : HID_DESCRIPTOR_BYTES;
        if (available >= at + descriptor) parse_descriptor(m + at, descriptor);
        u32 ack[3] = {HID_DEVICE_INFO_ACK, 1, 0};
        mouse_send(ack, 9);
        mouse_ready = 1;
        dk->input_mouse(0, 0, 0, 0);
    } else if (type == HID_INPUT_REPORT) {
        if (size <= available - HID_HEADER) mouse_report(m + HID_HEADER, size);
    }
}

static u32 mouse_next(void) {
    struct vmbus_packet p;
    for (;;) {
        int r = vmbus_read(mouse, mouse_packet, PACKET_BYTES, &p);
        if (r == 0) return 0;
        if (r < 0 || p.type != VM_PKT_DATA_INBAND || p.total < p.data_offset + PIPE_HEADER + HID_HEADER) continue;
        const u8 *m = mouse_packet + p.data_offset + PIPE_HEADER;
        u32 type = *(const u32 *)m;
        mouse_message(m, p.total - p.data_offset - PIPE_HEADER);
        return type + 1;
    }
}

int hv_mouse_start(void) {
    mouse = vmbus_find(mouse_guid, 0);
    if (!mouse || vmbus_open(mouse, RING_BYTES)) return -1;
    u32 request[3] = {HID_PROTOCOL_REQUEST, 4, HID_VERSION};
    if (mouse_send(request, sizeof(request))) return -1;
    u64 end = dk->nanoseconds() + WAIT_NS;
    while (!mouse_ready && dk->nanoseconds() < end)
        if (!mouse_next()) __asm__ volatile("pause");
    if (!mouse_ready) {
        dk->log("HYPERV MOUSE: no device information received");
        return -1;
    }
    dk->log("HYPERV MOUSE: ready");
    return 0;
}

void hv_mouse_poll(void) {
    if (!mouse || !mouse->open) return;
    while (mouse_next()) {}
}

void hv_input_stop(void) {
    vmbus_close(keyboard);
    vmbus_close(mouse);
}
