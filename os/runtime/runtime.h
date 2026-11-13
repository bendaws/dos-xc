/*
 * DOS-XC /os/runtime/runtime.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once
#include "../programs/api.h"
#include "../libc/include/dosxc/abi.h"

typedef __UINTPTR_TYPE__ uptr;
typedef long long i64;
typedef unsigned long long u64;
typedef unsigned u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define PATH_MAX_RT 260
#define MAX_FDS 1024
#define MAX_DEPTH 32
#define OWNER_KERNEL 0
#define PAGE 4096u

extern struct command_context *ctx;
extern int runtime_bits;

/* support.c */
void *memcpy(void *, const void *, __SIZE_TYPE__);
void *memset(void *, int, __SIZE_TYPE__);
void *memmove(void *, const void *, __SIZE_TYPE__);
int memcmp(const void *, const void *, __SIZE_TYPE__);
uptr rt_strlen(const char *);
void rt_strcpy(char *, const char *);
int rt_upper(int);
int rt_equal_nocase(const char *, const char *);
int rt_strcmp(const char *, const char *);
void rt_print(const char *);
int keyboard_modifiers(void);
void rt_print_hex(u64);
void rt_print_decimal(u64);
i64 rt_epoch(unsigned year, unsigned month, unsigned day, unsigned hour, unsigned minute, unsigned second);
static inline void rt_outb(u16 port, u8 value) { __asm__ volatile("outb %0,%1" ::"a"(value), "Nd"(port)); }
static inline u8 rt_inb(u16 port) { u8 v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(port)); return v; }

/* cpu.c */
struct saved_context { uptr registers[16]; };
/* Registers saved by the int 0x80 entry (switch*.asm), lowest address first. */
struct syscall_frame {
    uptr ax, bx, cx, dx, si, di, bp;
#if __SIZEOF_POINTER__ == 8
    uptr r8, r9, r10, r11, r12, r13, r14, r15;
#endif
};
extern int runtime_fxsr;               /* fxsave/fxrstor usable (switch*.asm) */
void runtime_syscall(struct syscall_frame *frame);
int cpu_init(void);
int cpu_has_sse(void);
void cpu_install_idt(void);
void cpu_restore_idt(void);
u64 cpu_nanoseconds(void);
void cpu_sleep(u64 nanoseconds);
i64 cpu_rtc_time(void);
int cpu_rtc_set(i64 seconds);
void clock_init(void);
int clock_control(int op, struct dosxc_clock *value);
void cpu_resume(void);
void cpu_kernel_idt(void);
void cpu_preempt_start(void);
void cpu_preempt_stop(void);
#define MAX_CPUS 32
int cpu_index(void);                    /* this processor: 0 is the boot processor */
void cpu_set_gs(uptr value);
void cpu_ap_prepare(void);              /* on the boot processor, before others start */
void cpu_ap_setup(int index);           /* on processor INDEX as it starts */

/* smp.c: the other processors run ring 3 code only; the kernel stays on processor 0. */
void smp_init(void);
int smp_start(void);                    /* 1 when other processors run programs */
void smp_stop(void);                    /* their programs go back to processor 0 */
int smp_count(void);                    /* processors running programs, 1 when only this one */
int smp_online(int cpu);
void smp_poke(int cpu);                 /* wake processor CPU */
int smp_service(void);                  /* other processors: nonzero when asked to stop */
void smp_park(void) __attribute__((noreturn));
void smp_tree(uptr root);               /* other processors: run in ROOT (0: the base tree) */
void smp_tlb_flush(const void *tree);   /* the page tables of TREE changed; 0: every tree */
void smp_set_pat(u64 value);
void smp_eoi(void);
const u8 *acpi_table(const char *signature);   /* power.c */
int runtime_enter(uptr entry, uptr stack, struct saved_context *save, int sse);
void runtime_leave(struct saved_context *save, int value) __attribute__((noreturn));
/* setjmp: 0 now, later the value given to runtime_leave(save, value). */
int runtime_save(struct saved_context *save) __attribute__((returns_twice));
/* Saves the caller in SAVE (as runtime_enter does), then runtime_leave(target, value). */
int runtime_switch(struct saved_context *save, struct saved_context *target, int value);
void bios_call(void);                  /* kernel bios_transfer (BIOS boot only) */

int memory_init(void);
void *memory_alloc(uptr size, uptr align, int owner, int zero);
int memory_free(void *block, int owner);
void memory_free_owner(int owner);
u64 memory_total(void);
int memory_high(unsigned index, u64 *base, u64 *length);
int memory_owns(uptr base, uptr size, int owner);
int memory_free_range(uptr base, uptr size, int owner);
u64 memory_available(void);
u64 memory_owned(int owner);
void memory_each(int owner, void (*fn)(uptr base, uptr size, void *context), void *context);
void *memory_reserve_front(uptr size);  /* before the allocator starts */

void cache_init(void);
void cache_resume(void);
void cache_use_native(int device, u64 base);   /* storage.c holds the system volume */
void storage_flush_all(void);                  /* storage.c: the disks' write caches */
int cache_overlay_read(u32 lba, void *buffer);
int cache_overlay_write(u32 lba, const void *buffer);

void volumes_init(void);
void volumes_scan(void);
void volumes_resume(void);
struct fat32_volume *volume_of(const char *canonical);
int volume_exists(char letter);
int volumes_list(struct dosxc_volume *out, unsigned capacity);
int disk_io(unsigned index, u64 lba, unsigned count, void *buffer, int write);
int disk_info(unsigned index, struct dosxc_disk *out);
unsigned disk_count(void);
int fs_stat(const char *path, struct fat32_info *info);
int fs_list(const char *path, fat32_list_fn fn, void *context);
int fs_read(const char *path, void *buffer, u32 size);
int fs_read_at(const char *path, u32 offset, void *buffer, u32 size);
int fs_read_cursor(const char *path, struct fat32_cursor *cursor, u32 offset, void *buffer, u32 size);
int fs_save(const char *path, const void *data, u32 size);
int fs_mkdir(const char *path);
int fs_remove(const char *path);
int fs_rmdir(const char *path);
int fs_rename(const char *from, const char *to);
int fs_writable(const char *path);
void volumes_poll(void);                 /* UEFI: disks coming and going */
int volume_eject(char letter);
int volume_mount_image(const char *canonical);   /* the drive letter, or -DOSXC_* */
int volume_image_busy(const char *canonical);    /* 1 while it is mounted */
struct iso_volume;
struct iso_volume *iso_mount(unsigned disk);     /* 0 when DISK holds no ISO 9660 volume */
void iso_unmount(struct iso_volume *v);
u64 iso_bytes(const struct iso_volume *v);
int iso_stat(struct iso_volume *v, const char *path, struct fat32_info *info);
int iso_list(struct iso_volume *v, const char *path, fat32_list_fn fn, void *context);
int iso_read_cursor(struct iso_volume *v, const char *path, struct fat32_cursor *c, u32 offset, void *buffer, u32 size);
int volume_notice(char *buffer, uptr size);
void volume_post_notice(const char *text);

/* terminal.c */
struct process;
dosxc_ssize terminal_write(const u8 *buffer, uptr size);
dosxc_ssize terminal_read(u8 *buffer, uptr size);
int terminal_poll(void);
int terminal_mode(int mode);
int terminal_get_key(int wait);
void terminal_reset(void);
void terminal_input_reset(void);
int terminal_mouse(struct dosxc_mouse *out);
int terminal_interrupted(void);         /* Ctrl+C typed while a program waits */
int terminal_pointer(struct dosxc_pointer *out);
struct vconsole;
int vc_open(int owner);
struct vconsole *vc_get(int id);
void vc_attach(struct process *p, struct vconsole *v);
void vc_detach(struct process *p);
void vc_close(struct vconsole *v);
void vc_owner_died(int pid);
int vc_read(struct vconsole *v, u8 *buffer, uptr size);
int vc_push_keys(struct vconsole *v, const int *keys, uptr count);
int vc_clear(struct vconsole *v);
int vc_cursor(struct vconsole *v, unsigned row, unsigned column);
int vc_color(struct vconsole *v, unsigned attribute, int whole);
int vc_get_color(struct vconsole *v);

/* display.c */
int display_syscall(int operation, long index, void *buffer, dosxc_size size);
int display_graphics(void);             /* a display is in graphics mode */
void display_text(void);                /* every display back to the console */
int display_owner(void);                /* pid of the program using the graphics modes */
void display_process_finished(int pid);

/* files.c */
struct open_file;
struct library;
#define MAX_PROCESS_LIBRARIES 24
struct tls_module {
    const void *object;          /* the struct elf_object it belongs to */
    uptr image, filesz, memsz, align;
    uptr offset;                 /* below the thread pointer; 0 = allocated per thread */
};

struct process {
    int used, pid;
    uptr fault_entry;            /* the C library's entry for faults, and which */
    u32 fault_mask;              /* signals (1 << n) it wants */
    struct process *parent;
    struct open_file *fds[MAX_FDS];
    u8 cloexec[MAX_FDS];
    char cwd[PATH_MAX_RT];       /* canonical "C:\DIR\SUB" form */
    char path[PATH_MAX_RT];      /* canonical program path */
    uptr image_base, image_size;
    struct saved_context saved;
    struct library *libraries[MAX_PROCESS_LIBRARIES];   /* dependencies first */
    u8 *library_data[MAX_PROCESS_LIBRARIES];             /* this process's copies */
    int library_count;
    const struct elf_object *program;   /* set when dynamically linked: for dlopen */
    struct tls_module tls[MAX_PROCESS_LIBRARIES + 1];
    int tls_count;
    uptr tls_static;             /* bytes of thread-local data below the thread pointer */
    uptr tp;                     /* the main thread's thread pointer */
    int background, killed;      /* a task of its own; End Task was asked for */
    int exit_pending, exit_status; /* a thread ended the process with this status */
    u64 cpu_ns;
    struct vconsole *vc;         /* its terminal window, if any */
    struct aspace *space;        /* 64-bit: its page tables, shared by its threads */
    int grantees[4];             /* processes that may use all of its memory (the desktop) */
    char user[32];               /* its user account; "" for the system */
    u32 sandbox;                 /* DOSXC_SANDBOX_* rights given up, also for its children */
    u8 unlocked;                 /* may manage accounts (DOSXC_USER_UNLOCK) */
    u8 replaced;                 /* exec started its successor under the same pid */
    u8 orphaned;                 /* its parent ended first (so it is no login) */
    struct fork_remap *remaps;   /* 64-bit fork: low memory seen at the parent's addresses */
    int remap_count, remap_capacity;
    u8 gs_self;                  /* its C library finds the running thread at %gs:0 */
    u8 slot_user;                /* it reads the shared thread slot, so stays on processor 0 */
};
/* accounts.c */
int accounts_enabled(void);
int accounts_admin(void);
/* events.c */
void event_log(int level, const char *source, const char *format, ...);   /* %s %d %u %x %c */
const char *event_program(const struct process *p);
void process_exited(struct process *p, int status);   /* logs it */
long events_syscall(int op, void *a, void *b);
int rt_format_v(char *out, uptr size, const char *format, __builtin_va_list ap);                                 /* the running process may administer */
int accounts_access(const char *canonical, int write);    /* 0 or -EACCES */
void accounts_init(void);
long user_syscall(int operation, const void *a, void *b);
int env_set(const char *name, const char *value);
int process_grant(struct process *p, int pid);           /* PID may use all of P's memory */
int process_share(int owner, uptr start, uptr end, int pid);   /* PID may use OWNER's [start, end) */
extern struct process *current;
int path_resolve(const struct process *, const char *input, char *out);
void path_posix(const char *canonical, char *out, uptr size);
int files_init(void);
int files_setup(struct process *child, struct process *parent, const char *output, int append);
void files_release(struct process *);
void files_fill_api(struct dosxc_api *api);
int files_console_mode(int mode);
int files_is_executable(const char *canonical, u8 magic[4]);
void files_flush_all(void);
void files_volume_gone(char letter);
void files_flush_volume(char letter);
int files_fullpath(const char *path, char *buffer, dosxc_size size);
long unix_syscall(int operation, uptr a, uptr b);
int files_poll(int fd, int events);     /* -ENOTSOCK unless FD is a local socket */

/* paging.c */
int paging_map(uptr va, uptr pa);
uptr paging_root(void);
int paging_user_identity(int isolated);  /* ring 3: our own identity tables */
extern int paging_isolated;
struct aspace;                          /* a page-table tree: one per process (64-bit) */
struct aspace *aspace_new(void);
void aspace_free(struct aspace *a);
void aspace_activate(struct aspace *a);         /* 0: the base tree */
int aspace_map(struct aspace *a, uptr va, uptr pa);
void paging_share_top(unsigned index);
void aspace_user(struct aspace *a, uptr va, uptr size, int mode);   /* ring 3 access: 0 none, 1 read, 2 write */
void aspace_user_all(uptr va, uptr size, int mode);
u64 *aspace_root(const struct aspace *a);
struct aspace *aspace_at(int index);    /* 0 for the base tree and unused slots */
void paging_user_base(uptr va, uptr size, int mode);   /* in every tree made after this */
void paging_write_combining(uptr va, uptr size);       /* a framebuffer, in every tree (64-bit) */
void sched_init(void);
void vm_tables_free(u64 *pml4);         /* vm.c: a tree's mapping tables */
int vm_user_ok(uptr start, uptr end, int write);
int paging_user_ok(uptr p, uptr n, int write);   /* the running program may use that memory */
int paging_user_string(const char *s);
struct process *process_find(int pid);
int process_may_access(int owner, int pid, uptr address);   /* PID may use OWNER's memory there */
void process_memory_added(uptr base, uptr size, int owner);  /* memory.c */
void process_memory_removed(uptr base, uptr size);
extern u64 paging_user;                 /* the U bit for page entries, or 0 */
extern int runtime_ring3;               /* programs run in ring 3 */
uptr runtime_on_stack(uptr (*fn)(uptr), uptr argument, void *top);   /* switch64.asm */
/* vm.c */
long library_syscall(int op, void *a, void *b);
int tls_add(struct process *p, const void *object, uptr image, uptr filesz, uptr memsz, uptr align, uptr vaddr, int fixed);
void cpu_set_tp(uptr value);
void sched_restore_tp(void);
long vm_syscall(int op, uptr address, uptr size, long arg, int fd, u64 offset);
struct vm_object;
struct vm_object *vm_object_open(const char *name, int create, int exclusive);   /* name 0: anonymous */
int vm_object_unlink(const char *name);
void vm_object_close(struct vm_object *o);
u64 vm_object_size(const struct vm_object *o);
int vm_object_resize(struct vm_object *o, u64 size);
struct vm_object *files_object(int fd);
long shm_syscall(int operation, const char *name, int flags);
int vm_fault(uptr address, uptr error);
void vm_release(int owner);
void vm_resume(void);
void cpu_page_fault_stack(void);
int vm_contains(uptr address);
int vm_present(uptr address);
u64 vm_total(void);
u64 vm_free(void);
void paging_resume(void);

/* dynamic.c */
void libraries_activate(struct process *p);
void libraries_release(struct process *p);
const char *library_at(uptr address, uptr *offset);

char **environment_master(void);
void speaker_process_finished(int pid);
void audio_process_finished(int pid);
i64 audio_syscall(int operation, uptr a, uptr b);

void power_init(void);
void power_poll(void);
void power_off(void);

/* sched.c */
void sched_set_leaf(struct process *p);
void sched_yield(void);
void sched_sleep(u64 nanoseconds);
void sched_syscall_end(uptr *iret_frame);
int sched_tick(uptr ip);
void sched_preempted(void);
int sched_start(struct process *p, uptr entry, uptr sp);
void sched_pause(void);
void sched_resume(void);
long sched_syscall(int operation, void *a, long b, long c);
void sched_process_died(struct process *p);
void sched_thread_leave(int wait_status);
int socket_wait_interrupted(void);
void sched_stop_threads(struct process *p);
void sched_kernel_enter(void);          /* first thing on entering the kernel from a program */
void sched_ap_start(int cpu, u8 *stack_top) __attribute__((noreturn));

int volumes_driver_disk(int handle, u64 sectors, int readonly, const char *name);
void volumes_driver_disk_gone(int handle);
int driver_disk_io(int handle, u64 lba, unsigned count, void *buffer, int write);
void usb_input_key(unsigned usage, int pressed);
void usb_input_mouse(int dx, int dy, int wheel, unsigned buttons);

void random_stir(void);
long random_syscall(void *buffer, dosxc_size size);

/* loader.c */
int process_background(struct process *parent, const char *path, char *const argv[], char *const envp[], struct vconsole *console);
int process_background_fds(struct process *parent, const char *path, char *const argv[], char *const envp[], const int *map, int count);
int files_remap(struct process *child, struct process *parent, const int *map, int count);
void process_record_exit(struct process *p, int status);
long process_wait(int *status, int pid, int no_wait);
void process_kill_console(struct vconsole *console);
int process_list(struct dosxc_task_info *out, unsigned capacity);
int process_kill(int pid);
int process_alive(int pid);
int process_spawn(struct process *parent, const char *path, char *const argv[],
                  char *const envp[], const char *output, int append);
void process_exit(int wait_status) __attribute__((noreturn));
struct process *process_root(void);
void process_finish(struct process *p);
int sched_fault_enter(void);
void sched_fault_leave(void);
void runtime_exception(uptr *frame, uptr *regs);
struct process *process_at(int index);  /* slot INDEX of MAX_DEPTH, or 0 */
long process_fork(struct syscall_frame *frame);
long process_exec(const char *path, char *const argv[], char *const envp[]);
int sched_fork(struct process *child, const uptr *frame, const void *fx);
int sched_threads(const struct process *p);    /* other threads of P that run */
uptr sched_tp(void);                    /* the running thread's thread pointer */

/* fork.c (64-bit) */
struct fork_remap { uptr va, pa, size; };     /* [va, va + size) shows the frames at pa */
int fork_memory(struct process *child, struct process *parent, uptr sp);
int libraries_fork(struct process *child, struct process *parent);
int files_fork(struct process *child, struct process *parent);
int fork_release(struct process *p, uptr block);  /* 1 when BLOCK was one of P's copies */
void fork_forget(struct process *p);
int fork_keeper(uptr base, uptr size);  /* a process still showing those frames' addresses, or 0 */
int vm_fork(struct process *child, struct process *parent);
void vm_swap_owner(int a, int b);
void memory_swap_owner(int a, int b);
int runtime_enter_frame(const uptr *frame, struct saved_context *save, const void *fx);
