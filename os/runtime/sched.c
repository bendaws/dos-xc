/*
 * DOS-XC /os/runtime/sched.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "runtime.h"

#define MAX_TASKS 256
#define SLICE_NS 10000000u
#define TASK_STACK (128u * 1024)   /* kernel stack: system calls of ring 3 programs run here */

#if __SIZEOF_POINTER__ == 8
#define CONTEXT_FRAME 1      /* rbp */
#define CONTEXT_SP 6
#define CONTEXT_IP 7
#else
#define CONTEXT_FRAME 3      /* ebp */
#define CONTEXT_SP 4
#define CONTEXT_IP 5
#endif

/* Where a task is.  Only processor 0 runs the kernel: a task the others
   run comes back to it for every system call, fault or end of turn. */
enum { ON_BSP, OFFERED, ON_AP };
#define BURST_MIN 200000u            /* ns a task runs between system calls before it may move */

struct task {
    int used, started;
    struct process *leaf;      /* the process that runs: a chain's last child */
    struct saved_context context;
    u64 wake;                  /* cpu_nanoseconds() to sleep until; 0 runs now */
    uptr entry, sp;
    int thread;                /* a thread of OWNER rather than a process's main line */
    int finishing;             /* waiting for its process's threads to end */
    int fault_depth;           /* fault handlers running on this thread */
    struct process *owner;
    uptr thread_data;          /* what sched_thread_slot holds while it runs */
    uptr wait_address;         /* futex it sleeps on; 0 when it is not waiting */
    uptr tp;                   /* a thread's thread pointer */
    struct saved_context thread_saved;
    volatile int where;        /* ON_BSP, OFFERED (to any processor) or ON_AP */
    u64 user_from;             /* when it last went back to its program; 0 in the kernel */
    u64 burst;                 /* average time its program runs between kernel entries */
    uptr cr2;                  /* a fault's address, as it moves to processor 0 */
    u8 fx[512] __attribute__((aligned(16)));   /* its FPU state while it moves */
};

/* What another processor is doing. */
struct ap {
    int task;                  /* the task it runs, or -1 */
    int hand, hand_state;      /* a task it just left, to be given WHERE = hand_state */
    int last;                  /* where it starts looking for work */
    u64 since, idle_ns;
    struct saved_context fresh;   /* the start of its loop */
};

static struct task tasks[MAX_TASKS];
static u8 *stacks[MAX_TASKS];             /* allocated the first time a slot is used */
static int running, task_count = 1;       /* task 0 is the shell's chain */
static u64 slice_start, idle_ns;
static int preempting;
static void *shared_board;
static uptr shared_size;
static int shared_owner;
static struct ap aps[MAX_CPUS];
static struct saved_context bsp_fresh;     /* processor 0 with nothing to run */
static u8 bsp_stack[16384] __attribute__((aligned(16)));
static int offer = -1;                     /* a task processor 0 just left for the others */
static volatile int offered;               /* tasks OFFERED */
static volatile int handed;                /* a task came back for the kernel */

static struct { uptr slot; u8 pad[PAGE - sizeof(uptr)]; } slot_page __attribute__((aligned(4096)));
#define sched_thread_slot slot_page.slot
static struct { uptr zero; u8 pad[PAGE - sizeof(uptr)]; } zero_page __attribute__((aligned(4096)));

static uptr tp_of(const struct task *t) {
    if (t->thread && t->leaf == t->owner) return t->tp;
    return t->leaf ? t->leaf->tp : 0;
}

/* %gs:0 for the C library: the thread's data, or 0 for a main line. */
static uptr gs_of(const struct task *t) {
    if (t->thread && t->leaf == t->owner) return t->thread_data;
    return (uptr)&zero_page;
}

static void slot_update(struct task *t) {
    sched_thread_slot = t->thread && t->leaf == t->owner ? t->thread_data : 0;
    cpu_set_tp(tp_of(t));
    if (paging_user) cpu_set_gs(gs_of(t));
}

void sched_restore_tp(void) {
    uptr tp = tp_of(&tasks[running]);
    if (tp) cpu_set_tp(tp);
    if (paging_user) cpu_set_gs(gs_of(&tasks[running]));
}

void sched_set_leaf(struct process *p) {
    tasks[running].leaf = p;
    tasks[running].fault_depth = 0;
    slot_update(&tasks[running]);
}

static void add_ns(u64 *total, u64 ns) {
#if __SIZEOF_POINTER__ == 8
    __atomic_fetch_add(total, ns, __ATOMIC_RELAXED);
#else
    *total += ns;
#endif
}

static void account(u64 now) {
    if (current) add_ns(&current->cpu_ns, now - slice_start);
    slice_start = now;
}

static void check_killed(void) {
    if (tasks[running].finishing) return;
    if (current && current->killed) process_exit(current->exit_pending ? current->exit_status : DOSXC_SIGNALED(9));
}

static int task_ready(const struct task *t, u64 now) {
    return t->used && (t->wake <= now || (t->leaf && t->leaf->killed));
}

static int claim(struct task *t, int to) {
    int expected = OFFERED;
    if (!__atomic_compare_exchange_n(&t->where, &expected, to, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
    __atomic_fetch_sub(&offered, 1, __ATOMIC_RELAXED);
    return 1;
}

static int idle_aps(void) {
    int n = 0;
    for (int cpu = 1; cpu < MAX_CPUS; ++cpu)
        if (smp_online(cpu) && aps[cpu].task < 0) ++n;
    return n;
}

static void wake_idle_ap(void) {
    for (int cpu = 1; cpu < MAX_CPUS; ++cpu)
        if (smp_online(cpu) && aps[cpu].task < 0) { smp_poke(cpu); return; }
}

/* The next task processor 0 may run after FROM (FROM too when SELF_TOO),
   claimed when it was offered: its index, or -1.  Offered tasks are left
   for idle processors. */
static int pick(int from, u64 now, int self_too) {
    int spare = idle_aps();
    for (int k = 1; k <= MAX_TASKS; ++k) {
        int i = (from + k) % MAX_TASKS;
        struct task *t = &tasks[i];
        if (!t->used || (i == from && !self_too)) continue;
        int where = __atomic_load_n(&t->where, __ATOMIC_ACQUIRE);
        if (where == ON_BSP && task_ready(t, now)) return i;
        if (where == OFFERED && !spare && claim(t, ON_BSP)) return i;
    }
    return -1;
}

static void schedule(void) {
    struct task *self = &tasks[running];
    for (;;) {
        u64 now = cpu_nanoseconds();
        int i = pick(running, now, 0);
        if (i >= 0) {
            struct task *t = &tasks[i];
            account(now);
            self->leaf = current;
            running = i;
            t->wake = 0;
            current = t->leaf;
            slot_update(t);
            libraries_activate(current);
            runtime_switch(&self->context, &t->context, 1);
            check_killed();
            return;
        }
        if (self->wake <= now || (current && current->killed)) {
            self->wake = 0;
            check_killed();
            return;
        }
        if (self->wait_address && current && !self->finishing) socket_wait_interrupted();
        u64 before = cpu_nanoseconds();
        __asm__ volatile("pause");
        idle_ns += cpu_nanoseconds() - before;
    }
}

/* Processor 0 with no task of its own: it runs what is ready. */
static __attribute__((noreturn)) void bsp_loop(void) {
    __asm__ volatile("cli");
    if (offer >= 0) {
        struct task *t = &tasks[offer];
        offer = -1;
        __atomic_fetch_add(&offered, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&t->where, OFFERED, __ATOMIC_RELEASE);
        wake_idle_ap();
    }
    current = 0;
    for (;;) {
        u64 now = cpu_nanoseconds();
        int i = pick(running, now, 1);
        if (i >= 0) {
            struct task *t = &tasks[i];
            account(now);
            running = i;
            t->wake = 0;
            current = t->leaf;
            slot_update(t);
            libraries_activate(current);
            runtime_leave(&t->context, 1);
        }
        __asm__ volatile("pause");
        idle_ns += cpu_nanoseconds() - now;
    }
}

static void note_burst(struct task *t, u64 now) {
    if (!t->user_from) return;
    t->burst = (t->burst * 3 + (now - t->user_from)) / 4;
    t->user_from = 0;
}

static void save_fx(u8 *fx) { __asm__ volatile("fxsave %0" : "=m"(*(u8 (*)[512])fx)); }
static void restore_fx(const u8 *fx) { __asm__ volatile("fxrstor %0" ::"m"(*(const u8 (*)[512])fx)); }

/* The running task on another processor stops there and becomes STATE;
   this returns wherever it continues (processor 0 for ON_BSP). */
static void ap_leave(int state) {
    struct ap *a = &aps[cpu_index()];
    struct task *t = &tasks[a->task];
    u64 now = cpu_nanoseconds();
    if (t->leaf) add_ns(&t->leaf->cpu_ns, now - a->since);
    note_burst(t, now);
    t->wake = 0;
    __asm__ volatile("mov %%cr2,%0" : "=r"(t->cr2));
    save_fx(t->fx);
    a->hand = a->task;
    a->hand_state = state;
    a->task = -1;
    runtime_switch(&t->context, &a->fresh, 1);
    restore_fx(t->fx);
    __asm__ volatile("mov %0,%%cr2" ::"r"(t->cr2));
}

/* Another processor's loop: run offered tasks, sleep in between. */
static __attribute__((noreturn)) void ap_loop(void) {
    int cpu = cpu_index();
    struct ap *a = &aps[cpu];
    smp_tree(0);
    if (a->hand >= 0) {
        struct task *t = &tasks[a->hand];
        int state = a->hand_state;
        a->hand = -1;
        if (state == OFFERED) __atomic_fetch_add(&offered, 1, __ATOMIC_RELAXED);
        else handed = 1;
        __atomic_store_n(&t->where, state, __ATOMIC_RELEASE);
    }
    for (;;) {
        if (smp_service()) smp_park();
        u64 now = cpu_nanoseconds();
        for (int k = 1; k <= MAX_TASKS; ++k) {
            int i = (a->last + k) % MAX_TASKS;
            struct task *t = &tasks[i];
            if (__atomic_load_n(&t->where, __ATOMIC_RELAXED) != OFFERED || !claim(t, ON_AP)) continue;
            struct process *p = t->leaf;
            if (!p || p->killed || !p->space) {
                handed = 1;
                __atomic_store_n(&t->where, ON_BSP, __ATOMIC_RELEASE);
                continue;
            }
            a->task = a->last = i;
            a->since = t->user_from = now;
            smp_tree((uptr)aspace_root(p->space));
            cpu_set_tp(tp_of(t));
            cpu_set_gs(gs_of(t));
            runtime_leave(&t->context, 1);
        }
        __asm__ volatile("sti; hlt; cli");
        a->idle_ns += cpu_nanoseconds() - now;
    }
}

void sched_ap_start(int cpu, u8 *stack_top) {
    struct ap *a = &aps[cpu];
    a->task = a->hand = -1;
    if (runtime_save(&a->fresh)) __builtin_unreachable();
    a->fresh.registers[CONTEXT_FRAME] = 0;
    a->fresh.registers[CONTEXT_SP] = ((uptr)stack_top - 16) | 8;
    a->fresh.registers[CONTEXT_IP] = (uptr)ap_loop;
    runtime_leave(&a->fresh, 1);
}

/* An interrupt stopped a program on another processor. */
void sched_ap_interrupt(void);
void sched_ap_interrupt(void) {
    smp_eoi();
    int stop = smp_service();
    struct ap *a = &aps[cpu_index()];
    if (a->task < 0) return;
    struct task *t = &tasks[a->task];
    if (stop || !t->leaf || t->leaf->killed) ap_leave(ON_BSP);
    else if (offered && cpu_nanoseconds() - a->since >= SLICE_NS) ap_leave(OFFERED);
    else return;
    if (!cpu_index()) check_killed();
}

void sched_kernel_enter(void) {
    if (cpu_index()) {
        ap_leave(ON_BSP);
        return;
    }
    note_burst(&tasks[running], cpu_nanoseconds());
}

/* Processor 0 is about to go back to the running program: another
   processor may continue it instead. */
static void offload(void) {
    struct task *self = &tasks[running];
    self->user_from = cpu_nanoseconds();
    if (task_count < 2 || !current || !current->space || current->killed || current->slot_user) return;
    if (self->finishing || self->wait_address || self->burst < BURST_MIN) return;
    if (!smp_start() || !idle_aps()) return;
    account(cpu_nanoseconds());
    self->leaf = current;
    offer = running;
    runtime_switch(&self->context, &bsp_fresh, 1);
    if (!cpu_index()) check_killed();
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
    if (iret_frame[1] & 3) offload();
}

/* Timer interrupt (BIOS boot), or any firmware interrupt of ring 3 (UEFI).
   Nonzero: switch tasks when it returns. */
int sched_tick(uptr ip) {
    if (!preempting || task_count < 2) return 0;
    if (cpu_nanoseconds() - slice_start < SLICE_NS && !handed) return 0;
    if (!current) return 0;
    if (ip >= current->image_base && ip < current->image_base + current->image_size) return 1;
    if (vm_contains(ip)) return 1;
    uptr offset;
    return library_at(ip, &offset) != 0;
}

void sched_preempted(void) {
    handed = 0;
    tasks[running].burst = SLICE_NS;
    sched_yield();
    if (paging_user) offload();
}

static __attribute__((noreturn)) void task_retire(void) {
    struct task *t = &tasks[running];
    current = 0;
    t->used = 0;
    --task_count;
    slice_start = cpu_nanoseconds();
    runtime_leave(&bsp_fresh, 1);
}

static __attribute__((noreturn)) void task_end(struct process *p, int status) {
    __asm__ volatile("cli");
    current = p;
    if (!p->replaced) {
        if (p->killed && !p->exit_pending) status = DOSXC_SIGNALED(9);
        process_record_exit(p, status);
        process_exited(p, status);
    }
    process_finish(p);
    task_retire();
}

static void task_entry(void) {
    struct task *t = &tasks[running];
    struct process *p = current;
    if (preempting) __asm__ volatile("sti");
    task_end(p, p->killed ? DOSXC_SIGNALED(9) : runtime_enter(t->entry, t->sp, &p->saved, cpu_has_sse()));
}

/* A forked process: the system call frame and FPU state sit at the top of its stack. */
#define FORK_STATE 1024
static void fork_entry(void) {
    struct process *p = current;
    u8 *top = stacks[running] + TASK_STACK - FORK_STATE;
    if (preempting) __asm__ volatile("sti");
    task_end(p, p->killed ? DOSXC_SIGNALED(9) : runtime_enter_frame((const uptr *)top, &p->saved, top + 512));
}

static void thread_entry(void) {
    struct task *t = &tasks[running];
    if (preempting) __asm__ volatile("sti");
    if (!current->killed) runtime_enter(t->entry, t->sp, &t->thread_saved, cpu_has_sse());
    __asm__ volatile("cli");
    task_retire();
}

/* Set up a new task for P that starts in ENTRY_POINT.  Returns its index. */
static int task_create(struct process *p, uptr entry, uptr sp, void (*entry_point)(void)) {
    int index = -1;
    for (int i = 1; i < MAX_TASKS && index < 0; ++i)
        if (!tasks[i].used) index = i;
    if (index < 0) return -DOSXC_EAGAIN;
    if (!stacks[index] && !(stacks[index] = memory_alloc(TASK_STACK, PAGE, OWNER_KERNEL, 0))) return -DOSXC_ENOMEM;
    tasks[0].used = 1;
    struct task *t = &tasks[index];
    memset(t, 0, sizeof(*t));
    t->used = 1;
    t->leaf = p;
    t->entry = entry;
    t->sp = sp;
    if (runtime_save(&t->context)) __builtin_unreachable();
    t->context.registers[CONTEXT_FRAME] = 0;
    t->context.registers[CONTEXT_SP] = (uptr)(stacks[index] + TASK_STACK - 16) | (sizeof(uptr) == 8 ? 8 : 12);
    t->context.registers[CONTEXT_IP] = (uptr)entry_point;
    if (task_count++ == 1 && !slice_start) slice_start = cpu_nanoseconds();
    if (!preempting && (ctx->bios_available || runtime_ring3)) {
        cpu_preempt_start();
        preempting = 1;
    }
    return index;
}

int sched_fork(struct process *child, const uptr *frame, const void *fx) {
    int index = task_create(child, 0, 0, fork_entry);
    if (index < 0) return index;
    u8 *top = stacks[index] + TASK_STACK - FORK_STATE;
    memcpy(top, frame, 20 * sizeof(uptr));
    memcpy(top + 512, fx, 512);
    if (preempting) ((uptr *)top)[17] |= 0x200;        /* flags: the program takes the timer */
    tasks[index].context.registers[CONTEXT_SP] = (uptr)(top - 16) | 8;
    child->background = 1;
    return child->pid;
}

uptr sched_tp(void) { return tp_of(&tasks[running]); }

int sched_threads(const struct process *p) {
    int n = 0;
    for (int i = 0; i < MAX_TASKS; ++i)
        if (tasks[i].used && tasks[i].thread && tasks[i].owner == p) ++n;
    return n;
}

/* Make P, already loaded, a task of its own. */
int sched_start(struct process *p, uptr entry, uptr sp) {
    int index = task_create(p, entry, sp, task_entry);
    if (index < 0) return index;
    p->background = 1;
    return p->pid;
}

static int thread_create(uptr entry, uptr sp, uptr thread_data) {
    int index = task_create(current, entry, sp, thread_entry);
    if (index < 0) return index;
    tasks[index].thread = 1;
    tasks[index].owner = current;
    tasks[index].thread_data = thread_data;
    tasks[index].tp = 0;
    return index;
}

static long futex_wake(int *address, long count) {
    long woken = 0;
    for (int i = 0; i < MAX_TASKS && woken < count; ++i) {
        struct task *t = &tasks[i];
        if (t->used && t->wait_address == (uptr)address && t->leaf == current) {
            t->wait_address = 0;
            t->wake = 0;
            ++woken;
        }
    }
    return woken;
}

static long futex_wait(int *address, int expected, long nanoseconds) {
    struct task *self = &tasks[running];
    if (*(volatile int *)address != expected) return -DOSXC_EAGAIN;
    self->wait_address = (uptr)address;
    self->wake = nanoseconds > 0 ? cpu_nanoseconds() + (u64)nanoseconds : ~(u64)0;
    schedule();
    int timed_out = self->wait_address != 0;
    self->wait_address = 0;
    return timed_out ? -DOSXC_ETIMEDOUT : 0;
}

static __attribute__((noreturn)) void thread_finish(int *clear) {
    struct task *t = &tasks[running];
    if (clear) {
        *clear = 0;
        futex_wake(clear, 0x7fffffff);
    }
    runtime_leave(&t->thread_saved, 1);
    __builtin_unreachable();
}

/* A thread's call to end its process ends only the thread, here: the main
   thread does the real exit, on the stack it started from. */
void sched_thread_leave(int wait_status) {
    struct task *t = &tasks[running];
    if (!t->thread || t->leaf != t->owner) return;
    if (!t->owner->killed) {
        t->owner->killed = 1;
        t->owner->exit_pending = 1;
        t->owner->exit_status = wait_status;
    }
    thread_finish(0);
}

/* P is ending: wait for its threads, each of which ends at its next turn. */
void sched_stop_threads(struct process *p) {
    struct task *self = &tasks[running];
    int was_finishing = self->finishing;
    for (;;) {
        int remaining = 0;
        for (int i = 0; i < MAX_TASKS; ++i) {
            struct task *t = &tasks[i];
            if (t == self || !t->used || !t->thread || t->owner != p) continue;
            ++remaining;
            t->owner->killed = 1;
            if (t->leaf) t->leaf->killed = 1;
        }
        if (!remaining) break;
        self->finishing = 1;
        self->wake = 0;
        schedule();
    }
    self->finishing = was_finishing;
}

void sched_pause(void) { if (preempting) cpu_preempt_stop(); }
void sched_resume(void) { if (preempting) cpu_preempt_start(); }

int sched_fault_enter(void) {
    if (tasks[running].fault_depth >= 4) return -1;
    ++tasks[running].fault_depth;
    return 0;
}

void sched_fault_leave(void) {
    if (tasks[running].fault_depth) --tasks[running].fault_depth;
}

long sched_syscall(int operation, void *a, long b, long c) {
    switch (operation) {
    case DOSXC_TASK_START: {
        struct vconsole *console = c > 0 ? vc_get((int)c - 1) : 0;
        if (c > 0 && !console) return -DOSXC_EINVAL;
        return process_background(current, a, (char *const *)b, 0, console);
    }
    case DOSXC_TASK_SPAWN: {
        const struct dosxc_spawn *spawn = a;
        return process_background(current, spawn->path, spawn->argv, spawn->envp, 0);
    }
    case DOSXC_TASK_SANDBOX:
        current->sandbox |= (u32)(uptr)a & DOSXC_SANDBOX_ALL;
        return current->sandbox;
    case DOSXC_TASK_SPAWN_FDS: {
        const struct dosxc_spawn_fds *spawn = a;
        return process_background_fds(current, spawn->path, spawn->argv, spawn->envp, spawn->map, spawn->count);
    }
    case DOSXC_TASK_WAIT: return process_wait(a, (int)b, (int)c);
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
    case DOSXC_TASK_FAULT_ENTRY:
        current->fault_entry = (uptr)a;
        current->fault_mask = (u32)b;
        return 0;
    case DOSXC_TASK_FAULT_DONE: sched_fault_leave(); return 0;
    case DOSXC_TASK_THREAD_CREATE: return thread_create((uptr)a, (uptr)b, (uptr)c);
    case DOSXC_TASK_THREAD_EXIT: thread_finish(a);
    case DOSXC_TASK_THREAD_SLOT:
        if ((uptr)a == 1 && paging_user) {        /* the C library reads %gs:0 instead */
            current->gs_self = 1;
            cpu_set_gs(gs_of(&tasks[running]));
            return 1;
        }
        current->slot_user = 1;
        return (long)(uptr)&sched_thread_slot;
    case DOSXC_TASK_SET_TP: {
        struct task *t = &tasks[running];
        if (t->thread && t->leaf == t->owner) t->tp = (uptr)a; else current->tp = (uptr)a;
        cpu_set_tp((uptr)a);
        return 0;
    }
    case DOSXC_TASK_FUTEX_WAIT: return futex_wait(a, (int)b, c);
    case DOSXC_TASK_FUTEX_WAKE: return futex_wake(a, b);
    case DOSXC_TASK_LIST: return process_list(a, (unsigned)b);
    case DOSXC_TASK_KILL: {
        struct process *target = process_find((int)b);
        if (target && !accounts_admin() && !rt_equal_nocase(target->user, current->user)) return -DOSXC_EPERM;
        return process_kill((int)b);
    }
    case DOSXC_TASK_ALIVE: return process_alive((int)b);
    case DOSXC_TASK_STATS: {
        struct dosxc_task_stats *s = a;
        memset(s, 0, sizeof(*s));
        account(cpu_nanoseconds());
        s->idle_ns = idle_ns;
        s->cpus = 1;
        for (int cpu = 1; cpu < MAX_CPUS; ++cpu)
            if (smp_online(cpu)) {
                s->idle_ns += aps[cpu].idle_ns;
                ++s->cpus;
            }
        s->now_ns = cpu_nanoseconds();
        s->memory_total = memory_total() + vm_total();
        s->memory_free = memory_available() + vm_free();
        s->tasks = (dosxc_u32)task_count;
        return 0;
    }
    case DOSXC_TASK_CPU_IDLE: {
        if (b < 1) return -DOSXC_EINVAL;
        if (b > MAX_CPUS) b = MAX_CPUS;
        if (preempting) smp_start();
        u64 *out = a;
        long n = 0;
        out[n++] = idle_ns;
        for (int cpu = 1; cpu < MAX_CPUS && n < b; ++cpu)
            if (smp_online(cpu)) out[n++] = aps[cpu].idle_ns;
        return n;
    }
    case DOSXC_TASK_SHARE_SET:
        if (shared_owner && shared_owner != current->pid && process_alive(shared_owner) > 0) return -DOSXC_EBUSY;
        shared_board = a;
        shared_size = b > 0 ? (uptr)b : 1u << 20;
        shared_owner = a ? current->pid : 0;
        return 0;
    case DOSXC_TASK_SHARE_GET:
        if (shared_owner && process_alive(shared_owner) <= 0) { shared_board = 0; shared_owner = 0; }
        if (shared_board && shared_owner != current->pid) {
            /* A client of the desktop: it sees the board, the desktop sees all of it. */
            uptr start = (uptr)shared_board & ~(uptr)(PAGE - 1);
            process_share(shared_owner, start, (uptr)shared_board + shared_size, current->pid);
            process_grant(current, shared_owner);
        }
        return (long)(uptr)shared_board;
    }
    return -DOSXC_EINVAL;
}

void sched_process_died(struct process *p) {
    vc_owner_died(p->pid);
    if (shared_owner == p->pid) { shared_owner = 0; shared_board = 0; }
}

/* The pages programs read their thread data from: ring 3 may use them. */
void sched_init(void) {
    paging_user_base((uptr)&slot_page, PAGE, 2);
    paging_user_base((uptr)&zero_page, PAGE, 1);
    for (int cpu = 0; cpu < MAX_CPUS; ++cpu) aps[cpu].task = aps[cpu].hand = -1;
    if (runtime_save(&bsp_fresh)) __builtin_unreachable();
    bsp_fresh.registers[CONTEXT_FRAME] = 0;
    bsp_fresh.registers[CONTEXT_SP] = (uptr)(bsp_stack + sizeof(bsp_stack) - 16) | (sizeof(uptr) == 8 ? 8 : 12);
    bsp_fresh.registers[CONTEXT_IP] = (uptr)bsp_loop;
}
