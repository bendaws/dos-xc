/*
 * DOS-XC /runtime/cpu.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * Processor state for 32/64-bit programs: SSE enablement, identity paging
 * up to 4 GiB (64-bit BIOS boot), the exception and system call IDT, time
 * sources.
 */

#include "runtime.h"

extern uptr runtime_exception_stubs[32];
extern char runtime_spurious[];
extern char runtime_syscall_entry[];
int runtime_fxsr;

static int sse;
static u64 tsc_hz;
static u64 tsc_boot;

struct __attribute__((packed)) descriptor_pointer {
    u16 limit;
    uptr base;
};

#if __SIZEOF_POINTER__ == 8
struct gate { u16 low, selector; u8 ist, type; u16 middle; u32 high, zero; };
#else
struct gate { u16 low, selector; u8 zero, type; u16 high; };
#endif
static struct gate idt[256] __attribute__((aligned(16)));
static struct descriptor_pointer idt_pointer, saved_idt;
static int idt_depth;

static void cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static uptr read_cr0(void) { uptr v; __asm__ volatile("mov %%cr0,%0" : "=r"(v)); return v; }
static uptr read_cr4(void) { uptr v; __asm__ volatile("mov %%cr4,%0" : "=r"(v)); return v; }
static void write_cr0(uptr v) { __asm__ volatile("mov %0,%%cr0" ::"r"(v) : "memory"); }
static void write_cr4(uptr v) { __asm__ volatile("mov %0,%%cr4" ::"r"(v) : "memory"); }
static u64 rdtsc(void) { u32 lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((u64)hi << 32) | lo; }

int cpu_has_sse(void) { return sse; }

static void enable_sse(void) {
    u32 a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    if (!(d & (1u << 25)) || !(d & (1u << 24))) return;      /* SSE, FXSR */
    write_cr0((read_cr0() & ~(uptr)((1u << 2) | (1u << 3))) | (1u << 1)); /* !EM !TS MP */
    write_cr4(read_cr4() | (1u << 9) | (1u << 10));              /* OSFXSR OSXMMEXCPT */
    sse = runtime_fxsr = 1;
}

#if __SIZEOF_POINTER__ == 8
static uptr read_cr3(void) { uptr v; __asm__ volatile("mov %%cr3,%0" : "=r"(v)); return v; }
static void write_cr3(uptr v) { __asm__ volatile("mov %0,%%cr3" ::"r"(v) : "memory"); }
/* The BIOS loaders identity-map the first GiB with 2 MiB pages.  Extend
   the same PML4[0] mapping to 4 GiB so the whole program arena is usable. */
static u64 directories[3][512] __attribute__((aligned(4096)));
static void extend_paging(void) {
    u64 *pml4 = (u64 *)(read_cr3() & ~0xfffull);
    if (!(pml4[0] & 1)) return;
    u64 *pdpt = (u64 *)(pml4[0] & 0x000ffffffffff000ull);
    int changed = 0;
    for (int i = 1; i < 4; ++i) {
        if (pdpt[i] & 1) continue;
        for (int j = 0; j < 512; ++j)
            directories[i - 1][j] = ((u64)i << 30) + ((u64)j << 21) + 0x83;
        pdpt[i] = (u64)(uptr)directories[i - 1] | 3;
        changed = 1;
    }
    if (changed) write_cr3(read_cr3());
}
#endif

/* type 0x8e: interrupt gate (IF cleared); 0xef: trap gate, DPL 3 (IF kept). */
static void set_gate(int vector, uptr handler, u16 selector, u8 type) {
    struct gate *g = &idt[vector];
    g->low = (u16)handler;
    g->selector = selector;
    g->type = type;
#if __SIZEOF_POINTER__ == 8
    g->ist = 0;
    g->middle = (u16)(handler >> 16);
    g->high = (u32)(handler >> 32);
    g->zero = 0;
#else
    g->zero = 0;
    g->high = (u16)(handler >> 16);
#endif
}

static void build_idt(void) {
    u16 cs;
    __asm__ volatile("mov %%cs,%0" : "=r"(cs));
    struct descriptor_pointer firmware;
    __asm__ volatile("sidt %0" : "=m"(firmware));
    /* Under UEFI, keep the firmware's interrupt vectors (its timer drives
       console and disk drivers).  BIOS boot runs with interrupts masked. */
    int copy = !ctx->bios_available && firmware.limit + 1 >= sizeof(idt) && firmware.base;
    for (int v = 0; v < 256; ++v) {
        if (copy) idt[v] = ((struct gate *)firmware.base)[v];
        else set_gate(v, (uptr)runtime_spurious, cs, 0x8e);
    }
    for (int v = 0; v < 32; ++v)
        if (v != 2) set_gate(v, runtime_exception_stubs[v], cs, 0x8e);
    /* System calls run with the caller's interrupt flag, as direct ABI 1
       calls did (UEFI firmware drivers depend on its timer interrupt). */
    set_gate(DOSXC_SYSCALL_VECTOR, (uptr)runtime_syscall_entry, cs, 0xef);
    idt_pointer.limit = sizeof(idt) - 1;
    idt_pointer.base = (uptr)idt;
}

void cpu_install_idt(void) {
    if (idt_depth++) return;
    __asm__ volatile("sidt %0" : "=m"(saved_idt));
    __asm__ volatile("lidt %0" ::"m"(idt_pointer));
}

/* The kernel's IDT, for leaving to real mode from inside a program. */
void cpu_kernel_idt(void) { __asm__ volatile("lidt %0" ::"m"(saved_idt)); }

void cpu_restore_idt(void) {
    if (--idt_depth) return;
    __asm__ volatile("lidt %0" ::"m"(saved_idt));
}

/* Measure the TSC against PIT channel 2 over 10 ms. */
static void calibrate(void) {
    u8 port = rt_inb(0x61);
    rt_outb(0x61, (u8)((port & ~0x02) | 0x01));
    rt_outb(0x43, 0xb0);
    rt_outb(0x42, 11932 & 0xff);
    rt_outb(0x42, 11932 >> 8);
    u64 start = rdtsc();
    u32 spins = 0;
    while (!(rt_inb(0x61) & 0x20) && ++spins < 50000000u) {
    }
    u64 elapsed = rdtsc() - start;
    rt_outb(0x61, port);
    tsc_hz = spins < 50000000u && elapsed > 1000 ? elapsed * 100 : 1000000000ull;
    tsc_boot = rdtsc();
}

u64 cpu_nanoseconds(void) {
    u64 ticks = rdtsc() - tsc_boot;
    u64 seconds = ticks / tsc_hz;
    return seconds * 1000000000ull + (ticks - seconds * tsc_hz) * 1000000000ull / tsc_hz;
}

void cpu_sleep(u64 nanoseconds) {
    u64 end = cpu_nanoseconds() + nanoseconds;
    while (cpu_nanoseconds() < end) __asm__ volatile("pause");
}

static u8 cmos(u8 reg) {
    rt_outb(0x70, reg);
    return rt_inb(0x71);
}

i64 cpu_rtc_time(void) {
    for (u32 n = 0; n < 1000000 && (cmos(10) & 0x80); ++n) {
    }
    u8 status = cmos(11);
    int binary = status & 4;
#define BCD(x) (binary ? (x) : (((x) >> 4) * 10 + ((x) & 15)))
    unsigned second = BCD(cmos(0)), minute = BCD(cmos(2)), raw_hour = cmos(4);
    unsigned day = BCD(cmos(7)), month = BCD(cmos(8)), year = BCD(cmos(9));
    unsigned century = BCD(cmos(0x32));
    unsigned hour = BCD(raw_hour & 0x7f);
#undef BCD
    if (!(status & 2)) {
        if (hour == 12) hour = 0;
        if (raw_hour & 0x80) hour += 12;
    }
    year += century >= 19 && century <= 21 ? century * 100 : 2000;
    return rt_epoch(year, month, day, hour, minute, second);
}

/* After a 16-bit program the boot loader rebuilt the processor state:
   enable SSE and the 4 GiB mapping again and take over the new kernel IDT. */
void cpu_resume(void) {
    enable_sse();
#if __SIZEOF_POINTER__ == 8
    if (ctx->bios_available) extend_paging();
#endif
    paging_resume();
    __asm__ volatile("sidt %0" : "=m"(saved_idt));
    if (idt_depth) __asm__ volatile("lidt %0" ::"m"(idt_pointer));
}

int cpu_init(void) {
    enable_sse();
#if __SIZEOF_POINTER__ == 8
    if (ctx->bios_available) extend_paging();
#endif
    build_idt();
    calibrate();
    return 0;
}
