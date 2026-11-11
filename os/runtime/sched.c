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
};

static struct task tasks[MAX_TASKS];
static u8 *stacks[MAX_TASKS];             /* allocated the first time a slot is used */
static int running, task_count = 1;       /* task 0 is the shell's chain */
static u64 slice_start, idle_ns;
static int preempting;
static void *shared_board;
static uptr shared_size;
static int shared_owner;

static struct { uptr slot; u8 pad[PAGE - sizeof(uptr)]; } slot_page __attribute__((aligned(4096)));
#define sched_thread_slot slot_page.slot

static uptr tp_of(const struct task *t) {
    if (t->thread && t->leaf == t->owner) return t->tp;
    return t->leaf ? t->leaf->tp : 0;
}

static void slot_update(struct task *t) {
    sched_thread_slot = t->thread && t->leaf == t->owner ? t->thread_data : 0;
    cpu_set_tp(tp_of(t));
}

void sched_restore_tp(void) {
    uptr tp = tp_of(&tasks[running]);
    if (tp) cpu_set_tp(tp);
}

void sched_set_leaf(struct process *p) {
    tasks[running].leaf = p;
    tasks[running].fault_depth = 0;
    slot_update(&tasks[running]);
}

static void account(u64 now) {
    if (current) current->cpu_ns += now - slice_start;
    slice_start = now;
}

static void check_killed(void) {
    if (tasks[running].finishing) return;
    if (current && current->killed) process_exit(current->exit_pending ? current->exit_status : DOSXC_SIGNALED(9));
}

static int task_ready(const struct task *t, u64 now) {
    return t->used && (t->wake <= now || (t->leaf && t->leaf->killed));
}

static void schedule(void) {
    struct task *self = &tasks[running];
    for (;;) {
        u64 now = cpu_nanoseconds();
        for (int k = 1; k < MAX_TASKS; ++k) {
            int i = (running + k) % MAX_TASKS;
            struct task *t = &tasks[i];
            if (!task_ready(t, now)) continue;
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
    if (vm_contains(ip)) return 1;
    uptr offset;
    return library_at(ip, &offset) != 0;
}

void sched_preempted(void) { sched_yield(); }

static __attribute__((noreturn)) void task_retire(void) {
    struct task *t = &tasks[running];
    current = 0;
    t->used = 0;
    --task_count;
    slice_start = cpu_nanoseconds();
    for (;;) {
        u64 now = cpu_nanoseconds();
        for (int k = 1; k <= MAX_TASKS; ++k) {
            int i = (running + k) % MAX_TASKS;
            struct task *next = &tasks[i];
            if (!task_ready(next, now)) continue;
            running = i;
            next->wake = 0;
            current = next->leaf;
            slot_update(next);
            libraries_activate(current);
            runtime_leave(&next->context, 1);
        }
        __asm__ volatile("pause");
        idle_ns += cpu_nanoseconds() - now;
    }
}

static void task_entry(void) {
    struct task *t = &tasks[running];
    struct process *p = current;
    if (preempting) __asm__ volatile("sti");
    int status = p->killed ? DOSXC_SIGNALED(9) : runtime_enter(t->entry, t->sp, &p->saved, cpu_has_sse());
    __asm__ volatile("cli");
    current = p;
    process_record_exit(p, p->killed && !p->exit_pending ? DOSXC_SIGNALED(9) : status);
    process_exited(p, p->killed && !p->exit_pending ? DOSXC_SIGNALED(9) : status);
    process_finish(p);
    task_retire();
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
    if (!preempting && ctx->bios_available) {
        cpu_preempt_start();
        preempting = 1;
    }
    return index;
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
    case DOSXC_TASK_THREAD_SLOT: return (long)(uptr)&sched_thread_slot;
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
        s->now_ns = cpu_nanoseconds();
        s->memory_total = memory_total() + vm_total();
        s->memory_free = memory_available() + vm_free();
        s->tasks = (dosxc_u32)task_count;
        return 0;
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

/* The page programs read their thread data from: ring 3 may use it. */
void sched_init(void) { paging_user_base((uptr)&slot_page, PAGE, 2); }
