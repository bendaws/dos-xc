/*
 * DOS-XC /runtime/sched.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * Multitasking.  A task is one chain of processes: a program started in the
 * background, plus the children it waits for (spawn stays synchronous).
 * Tasks are switched round robin.  The kernel itself is never preempted:
 * tasks change at system calls and while one sleeps or waits for input,
 * and on a BIOS boot also when the timer interrupts program code that has
 * used up its time slice, so a program stuck in a loop cannot hold up the
 * others.  Under UEFI the firmware owns the timer, so only the system call
 * switches apply there.
 */

#include "runtime.h"

#define MAX_TASKS 24
#define SLICE_NS 10000000u
#define TASK_STACK 32768

#if __SIZEOF_POINTER__ == 8
#define CONTEXT_FRAME 1      /* rbp */
#define CONTEXT_SP 6
#define CONTEXT_IP 7
#else
#define CONTEXT_FRAME 3      /* ebp */
#define CONTEXT_SP 4
#define CONTEXT_IP 5
#endif

struct task {
    int used, started;
    struct process *leaf;      /* the process that runs: a chain's last child */
    struct saved_context context;
    u64 wake;                  /* cpu_nanoseconds() to sleep until; 0 runs now */
    uptr entry, sp;
};

static struct task tasks[MAX_TASKS];
static u8 stacks[MAX_TASKS][TASK_STACK] __attribute__((aligned(16)));
static int running, task_count = 1;       /* task 0 is the shell's chain */
static u64 slice_start, idle_ns;
static int preempting;
static void *shared_board;
static int shared_owner;

void sched_set_leaf(struct process *p) { tasks[running].leaf = p; }

static void account(u64 now) {
    if (current) current->cpu_ns += now - slice_start;
    slice_start = now;
}

static void check_killed(void) {
    if (current && current->killed) process_exit(DOSXC_SIGNALED(9));
}

/* Run another task, or wait if nothing is ready.  Returns when this task's
   turn comes back and it may continue. */
static void schedule(void) {
    struct task *self = &tasks[running];
    for (;;) {
        u64 now = cpu_nanoseconds();
        for (int k = 1; k < MAX_TASKS; ++k) {
            int i = (running + k) % MAX_TASKS;
            struct task *t = &tasks[i];
            if (!t->used || t->wake > now) continue;
            account(now);
            self->leaf = current;
            running = i;
            t->wake = 0;
            current = t->leaf;
            libraries_activate(current);
            runtime_switch(&self->context, &t->context, 1);
            check_killed();
            return;
        }
        if (self->wake <= now) {
            self->wake = 0;
            check_killed();
            return;
        }
        u64 before = cpu_nanoseconds();
        __asm__ volatile("pause");
        idle_ns += cpu_nanoseconds() - before;
    }
}

void sched_yield(void) {
    if (task_count < 2) return;
    tasks[running].wake = 0;
    schedule();
}

void sched_sleep(u64 nanoseconds) {
    if (task_count < 2) { cpu_sleep(nanoseconds); return; }
    tasks[running].wake = cpu_nanoseconds() + nanoseconds;
    schedule();
}

/* The end of every system call: time to give someone else a turn? */
void sched_syscall_end(uptr *iret_frame) {
    if (task_count >= 2 && cpu_nanoseconds() - slice_start >= SLICE_NS) sched_yield();
    check_killed();
    if (preempting) iret_frame[2] |= 0x200;  /* flags: the program takes the timer */
}

/* Timer interrupt (BIOS boot).  Nonzero: switch tasks when it returns. */
int sched_tick(uptr ip) {
    if (!preempting || task_count < 2) return 0;
    if (cpu_nanoseconds() - slice_start < SLICE_NS) return 0;
    if (!current) return 0;
    if (ip >= current->image_base && ip < current->image_base + current->image_size) return 1;
    uptr offset;
    return library_at(ip, &offset) != 0;
}

void sched_preempted(void) { sched_yield(); }

static void task_entry(void) {
    struct task *t = &tasks[running];
    struct process *p = current;
    if (preempting) __asm__ volatile("sti");
    if (!p->killed) runtime_enter(t->entry, t->sp, &p->saved, cpu_has_sse());
    __asm__ volatile("cli");
    current = p;
    process_finish(p);
    current = 0;
    t->used = 0;
    --task_count;
    slice_start = cpu_nanoseconds();
    for (;;) {
        u64 now = cpu_nanoseconds();
        for (int k = 1; k <= MAX_TASKS; ++k) {
            int i = (running + k) % MAX_TASKS;
            struct task *next = &tasks[i];
            if (!next->used || next->wake > now) continue;
            running = i;
            next->wake = 0;
            current = next->leaf;
            libraries_activate(current);
            runtime_leave(&next->context, 1);
        }
        __asm__ volatile("pause");
        idle_ns += cpu_nanoseconds() - now;
    }
}

/* Make P, already loaded, a task of its own. */
int sched_start(struct process *p, uptr entry, uptr sp) {
    int index = -1;
    for (int i = 1; i < MAX_TASKS && index < 0; ++i)
        if (!tasks[i].used) index = i;
    if (index < 0) return -DOSXC_EAGAIN;
    tasks[0].used = 1;
    struct task *t = &tasks[index];
    memset(t, 0, sizeof(*t));
    t->used = 1;
    t->leaf = p;
    t->entry = entry;
    t->sp = sp;
    p->background = 1;
    if (runtime_save(&t->context)) __builtin_unreachable();
    t->context.registers[CONTEXT_FRAME] = 0;
    t->context.registers[CONTEXT_SP] = (uptr)(stacks[index] + TASK_STACK - 16) | (sizeof(uptr) == 8 ? 8 : 12);
    t->context.registers[CONTEXT_IP] = (uptr)task_entry;
    if (task_count++ == 1 && !slice_start) slice_start = cpu_nanoseconds();
    if (!preempting && ctx->bios_available) {
        cpu_preempt_start();
        preempting = 1;
    }
    return p->pid;
}

/* A 16-bit program takes over the machine for a while. */
void sched_pause(void) { if (preempting) cpu_preempt_stop(); }
void sched_resume(void) { if (preempting) cpu_preempt_start(); }

long sched_syscall(int operation, void *a, long b, long c) {
    switch (operation) {
    case DOSXC_TASK_START: {
        struct vconsole *console = c > 0 ? vc_get((int)c - 1) : 0;
        if (c > 0 && !console) return -DOSXC_EINVAL;
        return process_background(current, a, (char *const *)b, console);
    }
    case DOSXC_TASK_CONSOLE_OPEN: return vc_open(current->pid);
    case DOSXC_TASK_CONSOLE_READ: {
        struct vconsole *v = vc_get((int)b);
        return v ? vc_read(v, a, (uptr)c) : -DOSXC_EINVAL;
    }
    case DOSXC_TASK_CONSOLE_KEYS: {
        struct vconsole *v = vc_get((int)b);
        return v ? vc_push_keys(v, a, (uptr)c) : -DOSXC_EINVAL;
    }
    case DOSXC_TASK_CONSOLE_CLOSE:
        vc_close(vc_get((int)b));
        return 0;
    case DOSXC_TASK_YIELD: sched_yield(); return 0;
    case DOSXC_TASK_LIST: return process_list(a, (unsigned)b);
    case DOSXC_TASK_KILL: return process_kill((int)b);
    case DOSXC_TASK_ALIVE: return process_alive((int)b);
    case DOSXC_TASK_STATS: {
        struct dosxc_task_stats *s = a;
        memset(s, 0, sizeof(*s));
        account(cpu_nanoseconds());
        s->idle_ns = idle_ns;
        s->now_ns = cpu_nanoseconds();
        s->memory_total = memory_total();
        s->memory_free = memory_available();
        s->tasks = (dosxc_u32)task_count;
        return 0;
    }
    case DOSXC_TASK_SHARE_SET:
        if (shared_owner && shared_owner != current->pid && process_alive(shared_owner) > 0) return -DOSXC_EBUSY;
        shared_board = a;
        shared_owner = a ? current->pid : 0;
        return 0;
    case DOSXC_TASK_SHARE_GET:
        if (shared_owner && process_alive(shared_owner) <= 0) { shared_board = 0; shared_owner = 0; }
        return (long)(uptr)shared_board;
    }
    return -DOSXC_EINVAL;
}

void sched_process_died(struct process *p) {
    vc_owner_died(p->pid);
    if (shared_owner == p->pid) { shared_owner = 0; shared_board = 0; }
}
