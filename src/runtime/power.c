/*
 * DOS-XC /runtime/power.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * Power: switching the computer off (ACPI sleep state S5, or the UEFI
 * firmware's ResetSystem) and the power button.
 *
 * The ACPI tables are found from the RSDP (the BIOS areas on BIOS boot,
 * the firmware's configuration table under UEFI).  The FADT gives the PM1
 * event and control registers; the \_S5 package in the DSDT gives the
 * sleep type values.  The power button's status bit (PM1 PWRBTN_STS) is
 * polled; when it is set, the runtime starts SHUTDOWN.EXE (coreutils) as a
 * child of whatever program is running, or switches off by itself when
 * that program cannot be found.
 */

#include "runtime.h"
#include "net.h"
#include "usb.h"

static u16 pm1a_event, pm1b_event, pm1a_control, pm1b_control;
static u8 pm1_event_length;
static u16 sleep_type_a, sleep_type_b;
static int have_s5, have_button, button_busy;
static u64 last_poll;

static inline void outw(u16 port, u16 v) { __asm__ volatile("outw %0,%1" ::"a"(v), "Nd"(port)); }
static inline u16 inw(u16 port) { u16 v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(port)); return v; }

static int checksum_ok(const u8 *p, u32 length) {
    u8 sum = 0;
    for (u32 i = 0; i < length; ++i) sum = (u8)(sum + p[i]);
    return sum == 0;
}

static const u8 *find_rsdp(void) {
    if (ctx->acpi_rsdp) return (const u8 *)(uptr)ctx->acpi_rsdp;
    if (!ctx->bios_available) return 0;
    uptr ebda = (uptr)*(volatile u16 *)0x40e << 4;
    uptr ranges[2][2] = {{ebda, ebda + 1024}, {0xe0000, 0x100000}};
    for (int r = 0; r < 2; ++r) {
        if (!ranges[r][0] || ranges[r][0] >= 0x100000) continue;
        for (uptr a = ranges[r][0]; a + 20 <= ranges[r][1]; a += 16) {
            const u8 *p = (const u8 *)a;
            if (!memcmp(p, "RSD PTR ", 8) && checksum_ok(p, 20)) return p;
        }
    }
    return 0;
}

/* The table with SIGNATURE listed in the RSDT/XSDT, or 0. */
static const u8 *find_table(const u8 *rsdp, const char *signature) {
    int extended = rsdp[15] >= 2 && *(u64 *)(rsdp + 24) && *(u64 *)(rsdp + 24) < 0x100000000ull;
    const u8 *root = extended ? (const u8 *)(uptr)*(u64 *)(rsdp + 24) : (const u8 *)(uptr)*(u32 *)(rsdp + 16);
    if (!root || memcmp(root, extended ? "XSDT" : "RSDT", 4)) return 0;
    u32 length = *(u32 *)(root + 4), size = extended ? 8 : 4;
    for (u32 at = 36; at + size <= length; at += size) {
        u64 address = extended ? *(u64 *)(root + at) : *(u32 *)(root + at);
        if (!address || address >= 0x100000000ull) continue;
        const u8 *t = (const u8 *)(uptr)address;
        if (!memcmp(t, signature, 4)) return t;
    }
    return 0;
}

/* SLP_TYPa/b from the DSDT's \_S5 package: Name(_S5, Package(){a, b, ...}). */
static void find_s5(const u8 *dsdt) {
    u32 length = *(u32 *)(dsdt + 4);
    for (u32 i = 36; i + 8 < length; ++i) {
        if (memcmp(dsdt + i, "_S5_", 4)) continue;
        if (!(dsdt[i - 1] == 0x08 || (dsdt[i - 2] == 0x08 && dsdt[i - 1] == '\\'))) continue;
        const u8 *p = dsdt + i + 4;
        if (*p++ != 0x12) continue;                       /* PackageOp */
        p += ((*p & 0xc0) >> 6) + 1;                      /* PkgLength */
        ++p;                                              /* NumElements */
        u16 values[2];
        for (int v = 0; v < 2; ++v) {
            if (*p == 0x0a) { values[v] = p[1]; p += 2; }  /* BytePrefix */
            else if (*p == 0x00 || *p == 0x01) values[v] = *p++;   /* ZeroOp / OneOp */
            else values[v] = *p++;
        }
        sleep_type_a = values[0];
        sleep_type_b = values[1];
        have_s5 = 1;
        return;
    }
}

void power_init(void) {
    const u8 *rsdp = find_rsdp();
    if (!rsdp) return;
    const u8 *fadt = find_table(rsdp, "FACP");
    if (!fadt) return;
    u32 fadt_length = *(u32 *)(fadt + 4);
    pm1a_event = (u16)*(u32 *)(fadt + 56);
    pm1b_event = (u16)*(u32 *)(fadt + 60);
    pm1a_control = (u16)*(u32 *)(fadt + 64);
    pm1b_control = (u16)*(u32 *)(fadt + 68);
    pm1_event_length = fadt[88];
    u32 flags = fadt_length >= 116 ? *(u32 *)(fadt + 112) : 0;
    u64 dsdt = *(u32 *)(fadt + 40);
    if (fadt_length >= 148 && *(u64 *)(fadt + 140) && *(u64 *)(fadt + 140) < 0x100000000ull) dsdt = *(u64 *)(fadt + 140);
    if (dsdt && !memcmp((const u8 *)(uptr)dsdt, "DSDT", 4)) find_s5((const u8 *)(uptr)dsdt);
    /* ACPI mode (SCI_EN), so the power button reports to us instead of the
       BIOS's SMM code.  BIOS boot only: UEFI firmware has done it. */
    u32 smi_command = *(u32 *)(fadt + 48);
    u8 enable = fadt[52];
    if (ctx->bios_available && pm1a_control && !(inw(pm1a_control) & 1) && smi_command && enable) {
        rt_outb((u16)smi_command, enable);
        for (int i = 0; i < 300 && !(inw(pm1a_control) & 1); ++i) cpu_sleep(10000000);
    }
    /* The fixed-feature power button (FADT flag PWR_BUTTON clear). */
    if (pm1a_event && pm1_event_length >= 4 && !(flags & (1u << 4))) {
        u16 enable_port = (u16)(pm1a_event + pm1_event_length / 2);
        outw(pm1a_event, 0x100);                         /* clear PWRBTN_STS */
        outw(enable_port, (u16)(inw(enable_port) | 0x100)); /* PWRBTN_EN */
        have_button = 1;
    }
}

/* Switch the computer off; returns only when that is impossible. */
void power_off(void) {
    files_flush_all();
    drivers_shutdown();
    usb_shutdown();
    if (ctx->poweroff) ctx->poweroff();
    if (have_s5 && pm1a_control) {
        __asm__ volatile("cli");
        outw(pm1a_control, (u16)((inw(pm1a_control) & 0xe3ff) | sleep_type_a << 10 | 1u << 13));
        if (pm1b_control) outw(pm1b_control, (u16)((inw(pm1b_control) & 0xe3ff) | sleep_type_b << 10 | 1u << 13));
        cpu_sleep(500000000);
    }
    outw(0x604, 0x2000);                                  /* QEMU and Bochs, without ACPI tables */
    outw(0xb004, 0x2000);
}

/* The power button was pressed: run SHUTDOWN.EXE (or switch off). */
static void button_pressed(void) {
    if (button_busy) return;
    button_busy = 1;
    char path[PATH_MAX_RT];
    path[0] = (char)rt_upper(ctx->drive);
    rt_strcpy(path + 1, ":\\Programs\\coreutils\\SHUTDOWN.EXE");
    struct fat32_info info;
    if (current && !fs_stat(path, &info)) {
        char *argv[3] = {"SHUTDOWN.EXE", "/B", 0};
        display_text();
        terminal_mouse_hide();
        process_spawn(current, path, argv, 0, 0, 0);       /* returns only if cancelled or failed */
    } else {
        rt_print("\r\nThe power button was pressed: shutting down.\r\n");
        power_off();
    }
    button_busy = 0;
}

/* Called often (system calls, keyboard waits): look at the power button. */
void power_poll(void) {
    if (!have_button || button_busy) return;
    u64 now = cpu_nanoseconds();
    if (now - last_poll < 50000000ull) return;
    last_poll = now;
    u16 status = inw(pm1a_event);
    if (pm1b_event) status |= inw(pm1b_event);
    if (!(status & 0x100)) return;
    outw(pm1a_event, 0x100);
    if (pm1b_event) outw(pm1b_event, 0x100);
    button_pressed();
}
