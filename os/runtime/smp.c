/*
 * DOS-XC /os/runtime/smp.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "runtime.h"

#if __SIZEOF_POINTER__ == 8

#define TRAMPOLINE 0x8000u
#define STACK (32u * 1024)
#define LAPIC_ID 0x20
#define LAPIC_TPR 0x80
#define LAPIC_EOI 0xb0
#define LAPIC_SVR 0xf0
#define LAPIC_ICR_LOW 0x300
#define LAPIC_ICR_HIGH 0x310
#define LAPIC_TIMER 0x320
#define LAPIC_LINT0 0x350
#define LAPIC_LINT1 0x360
#define LAPIC_ERROR 0x370
#define LAPIC_INITIAL 0x380
#define LAPIC_CURRENT 0x390
#define LAPIC_DIVIDE 0x3e0
#define VECTOR_TIMER 0xe0
#define VECTOR_IPI 0xe1

enum { CPU_OFF, CPU_ON, CPU_PARKED };

struct cpu {
    u32 apic;
    volatile int state;
    volatile uptr space;         /* the tree its program runs in; 0 while it runs none */
    volatile u32 tlb_done;       /* the last flush request it carried out */
    u64 pat;
    u8 *stack;
};

static struct cpu cpus[MAX_CPUS];
static int cpu_count, online_count, available, started, active;
static volatile int parking;
static volatile u32 tlb_request;
static volatile u64 pat_value;
static volatile u8 *lapic;
static u32 timer_count;                /* LAPIC timer counts in 10 ms, divided by 16 */

extern u8 ap_trampoline[], ap_trampoline_end[], ap_cr3[], ap_efer[], ap_stack[], ap_entry[], ap_index[];

static u32 lapic_read(u32 reg) { return *(volatile u32 *)(lapic + reg); }
static void lapic_write(u32 reg, u32 value) { *(volatile u32 *)(lapic + reg) = value; }

static u64 read_msr(u32 index) {
    u32 low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(index));
    return ((u64)high << 32) | low;
}

static uptr read_cr3(void) { uptr v; __asm__ volatile("mov %%cr3,%0" : "=r"(v)); return v; }
static void write_cr3(uptr v) { __asm__ volatile("mov %0,%%cr3" ::"r"(v) : "memory"); }

static void *field(u8 *symbol) { return (void *)(uptr)(TRAMPOLINE + (symbol - ap_trampoline)); }

static int disabled_by_file(void) {
    struct fat32_info info;
    return ctx->volume && !ctx->stat(ctx->volume, "\\SYSTEM\\NOSMP", &info);
}

void smp_init(void) {
    if (!runtime_ring3 || disabled_by_file()) return;
    if (!ctx->bios_available && ctx->ap_page != TRAMPOLINE) return;     /* UEFI: the loader keeps that page for us */
    u32 a = 1, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    if (!(d & (1u << 9))) return;
    u64 base = read_msr(0x1b);
    if (!(base & (1u << 11)) || (base & (1u << 10))) return;   /* off, or in x2APIC mode */
    uptr cr4;
    __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
    if (cr4 & (1u << 12)) return;                             /* five-level paging */
    lapic = (volatile u8 *)(uptr)(base & 0xfffff000u);
    if (!(lapic_read(LAPIC_SVR) & 0x100)) return;
    const u8 *madt = acpi_table("APIC");
    if (!madt) return;
    u32 length = *(const u32 *)(madt + 4), self = lapic_read(LAPIC_ID) >> 24;
    cpus[0].apic = self;
    cpus[0].state = CPU_ON;
    cpu_count = 1;
    for (u32 at = 44; at + 2 <= length && madt[at + 1] >= 2; at += madt[at + 1]) {
        if (madt[at] != 0 || madt[at + 1] < 8 || !(madt[at + 4] & 1)) continue;
        if (madt[at + 3] == self || cpu_count == MAX_CPUS) continue;
        cpus[cpu_count++].apic = madt[at + 3];
    }
    for (int i = 1; i < cpu_count; ++i)
        if (!(cpus[i].stack = memory_alloc(STACK, PAGE, OWNER_KERNEL, 0))) cpu_count = i;
    available = cpu_count > 1;
    if (available) event_log(DOSXC_EVENT_INFO, "Processors", "%d processors found", cpu_count);
}

static void send_ipi(u32 apic, u32 command) {
    lapic_write(LAPIC_ICR_HIGH, apic << 24);
    lapic_write(LAPIC_ICR_LOW, command);
    for (u32 n = 0; n < 1000000 && (lapic_read(LAPIC_ICR_LOW) & (1u << 12)); ++n) __asm__ volatile("pause");
}

static int wait_state(struct cpu *c, int from, u64 nanoseconds) {
    u64 end = cpu_nanoseconds() + nanoseconds;
    while (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) == from && cpu_nanoseconds() < end) __asm__ volatile("pause");
    return c->state != from;
}

/* The first processor to start measures the LAPIC timer against the TSC. */
static void calibrate(void) {
    lapic_write(LAPIC_DIVIDE, 3);
    lapic_write(LAPIC_TIMER, 0x10000 | VECTOR_TIMER);
    lapic_write(LAPIC_INITIAL, 0xffffffffu);
    cpu_sleep(10000000);
    u32 left = lapic_read(LAPIC_CURRENT);
    lapic_write(LAPIC_INITIAL, 0);
    timer_count = 0xffffffffu - left;
    if (timer_count < 1000) timer_count = 1000000;
}

/* Processor INDEX, in long mode on its own stack (see ap_trampoline). */
__attribute__((noreturn)) void smp_ap_entry(int index);
void smp_ap_entry(int index) {
    struct cpu *c = &cpus[index];
    cpu_ap_setup(index);
    c->pat = pat_value;
    __asm__ volatile("wrmsr" ::"c"(0x277u), "a"((u32)c->pat), "d"((u32)(c->pat >> 32)));
    lapic_write(LAPIC_SVR, 0x1ff);
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_LINT0, 0x10000);
    lapic_write(LAPIC_LINT1, 0x10000);
    lapic_write(LAPIC_ERROR, 0x10000);
    if (!timer_count) calibrate();
    lapic_write(LAPIC_DIVIDE, 3);
    lapic_write(LAPIC_TIMER, 0x20000 | VECTOR_TIMER);
    lapic_write(LAPIC_INITIAL, timer_count);
    c->space = 0;
    c->tlb_done = tlb_request;
    __atomic_store_n(&c->state, CPU_ON, __ATOMIC_RELEASE);
    sched_ap_start(index, c->stack + STACK);
}

int smp_online(int cpu) { return cpu > 0 && cpu < cpu_count && cpus[cpu].state == CPU_ON; }

int smp_count(void) { return active ? online_count : 1; }

int smp_start(void) {
    if (!available || started) return active;
    started = 1;
    uptr root = paging_root();
    uptr size = (uptr)(ap_trampoline_end - ap_trampoline);
    if (!root || root >= 0x100000000ull || size > PAGE) return 0;
    static u8 saved[PAGE];
    memcpy(saved, (void *)(uptr)TRAMPOLINE, PAGE);
    memcpy((void *)(uptr)TRAMPOLINE, ap_trampoline, size);
    *(u64 *)field(ap_cr3) = root;
    *(u64 *)field(ap_efer) = read_msr(0xc0000080u) & ~(u64)(1u << 10);
    *(u64 *)field(ap_entry) = (u64)(uptr)smp_ap_entry;
    cpu_ap_prepare();
    pat_value = read_msr(0x277);
    parking = 0;
    int online = 0;
    for (int i = 1; i < cpu_count; ++i) {
        struct cpu *c = &cpus[i];
        c->state = CPU_OFF;
        *(u64 *)field(ap_stack) = (u64)(uptr)(c->stack + STACK);
        *(u64 *)field(ap_index) = (u64)i;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        send_ipi(c->apic, 0x4500);                 /* INIT */
        cpu_sleep(10000000);
        for (int attempt = 0; attempt < 2 && c->state == CPU_OFF; ++attempt) {
            send_ipi(c->apic, 0x4600 | (TRAMPOLINE >> 12));
            wait_state(c, CPU_OFF, attempt ? 100000000ull : 1000000ull);
        }
        if (c->state == CPU_ON) ++online;
        else send_ipi(c->apic, 0x4500);            /* so it cannot run the page restored below */
    }
    memcpy((void *)(uptr)TRAMPOLINE, saved, PAGE);
    online_count = online + 1;
    active = online > 0;
    if (!active) available = 0;
    return active;
}

/* Every program on the other processors comes back to this one, and they
   wait to be started again. */
void smp_stop(void) {
    if (!started) return;
    started = 0;
    parking = 1;
    for (int i = 1; i < cpu_count; ++i)
        if (cpus[i].state == CPU_ON) send_ipi(cpus[i].apic, 0x4000 | VECTOR_IPI);
    for (int i = 1; i < cpu_count; ++i) {
        if (cpus[i].state == CPU_ON && !wait_state(&cpus[i], CPU_ON, 2000000000ull))
            event_log(DOSXC_EVENT_WARNING, "Processors", "Processor %d did not stop", i);
        send_ipi(cpus[i].apic, 0x4500);
        cpus[i].state = CPU_OFF;
    }
    active = 0;
}

void smp_poke(int cpu) {
    if (smp_online(cpu)) send_ipi(cpus[cpu].apic, 0x4000 | VECTOR_IPI);
}

void smp_eoi(void) { lapic_write(LAPIC_EOI, 0); }

static void flush_here(void) {
    uptr cr4;
    __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
    if (cr4 & 0x80) {
        __asm__ volatile("mov %0,%%cr4" ::"r"(cr4 & ~(uptr)0x80) : "memory");
        __asm__ volatile("mov %0,%%cr4" ::"r"(cr4) : "memory");
    } else {
        write_cr3(read_cr3());
    }
}

int smp_service(void) {
    struct cpu *c = &cpus[cpu_index()];
    u32 request = __atomic_load_n(&tlb_request, __ATOMIC_ACQUIRE);
    if (c->tlb_done != request) {
        if (c->pat != pat_value) {
            c->pat = pat_value;
            __asm__ volatile("wbinvd" ::: "memory");
            __asm__ volatile("wrmsr" ::"c"(0x277u), "a"((u32)c->pat), "d"((u32)(c->pat >> 32)));
            __asm__ volatile("wbinvd" ::: "memory");
        }
        flush_here();
        __atomic_store_n(&c->tlb_done, request, __ATOMIC_RELEASE);
    }
    return parking;
}

void smp_park(void) {
    struct cpu *c = &cpus[cpu_index()];
    lapic_write(LAPIC_INITIAL, 0);
    c->space = 0;
    __atomic_store_n(&c->state, CPU_PARKED, __ATOMIC_RELEASE);
    for (;;) __asm__ volatile("cli; hlt");
}

void smp_tree(uptr root) {
    struct cpu *c = &cpus[cpu_index()];
    u32 request = __atomic_load_n(&tlb_request, __ATOMIC_ACQUIRE);
    c->space = root;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    write_cr3(root ? root : paging_root());
    __atomic_store_n(&c->tlb_done, request, __ATOMIC_RELEASE);
}

/* Called by the boot processor once page tables changed: every processor
   running in TREE (0: any tree) has dropped what it cached before this
   returns. */
void smp_tlb_flush(const void *tree) {
    if (!active) return;
    u32 request = __atomic_add_fetch(&tlb_request, 1, __ATOMIC_SEQ_CST);
    uptr waiting[MAX_CPUS];
    for (int i = 1; i < cpu_count; ++i) {
        uptr space = __atomic_load_n(&cpus[i].space, __ATOMIC_SEQ_CST);
        waiting[i] = 0;
        if (cpus[i].state != CPU_ON || !space || (tree && space != (uptr)tree)) continue;
        waiting[i] = space;
        send_ipi(cpus[i].apic, 0x4000 | VECTOR_IPI);
    }
    for (int i = 1; i < cpu_count; ++i) {
        if (!waiting[i]) continue;
        u64 end = cpu_nanoseconds() + 1000000000ull;
        while ((int)(__atomic_load_n(&cpus[i].tlb_done, __ATOMIC_ACQUIRE) - request) < 0 &&
               cpus[i].space == waiting[i] && cpus[i].state == CPU_ON && cpu_nanoseconds() < end)
            __asm__ volatile("pause");
    }
}

void smp_set_pat(u64 value) {
    pat_value = value;
    smp_tlb_flush(0);
}

/* The timer or a wake-up interrupted this processor in the kernel (idle). */
void smp_kernel_interrupt(void);
void smp_kernel_interrupt(void) {
    smp_eoi();
    smp_service();
}

#else

void smp_init(void) {}
int smp_start(void) { return 0; }
void smp_stop(void) {}
int smp_count(void) { return 1; }
int smp_online(int cpu) { (void)cpu; return 0; }
void smp_poke(int cpu) { (void)cpu; }
int smp_service(void) { return 0; }
void smp_park(void) { for (;;) __asm__ volatile("cli; hlt"); }
void smp_tree(uptr root) { (void)root; }
void smp_tlb_flush(const void *tree) { (void)tree; }
void smp_set_pat(u64 value) { (void)value; }
void smp_eoi(void) {}

#endif
