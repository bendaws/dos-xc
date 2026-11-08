/*
 * DOS-XC /runtime/runtime.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * Internal interfaces of \SYSTEM\ELFAPI.DLL, the 32/64-bit program runtime.
 * The shell loads the module at RUNTIME_BASE, calls runtime_init() once per
 * boot and runtime_run() for every ELF program typed at the prompt.
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
#define MAX_FDS 64
#define MAX_DEPTH 16
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
void cpu_resume(void);
void cpu_kernel_idt(void);
int runtime_enter(uptr entry, uptr stack, struct saved_context *save, int sse);
void runtime_leave(struct saved_context *save, int value) __attribute__((noreturn));
/* setjmp: 0 now, later the value given to runtime_leave(save, value). */
int runtime_save(struct saved_context *save) __attribute__((returns_twice));
/* Saves the caller in SAVE (as runtime_enter does), then runtime_leave(target, value). */
int runtime_switch(struct saved_context *save, struct saved_context *target, int value);
void bios_call(void);                  /* kernel bios_transfer (BIOS boot only) */

/* memory.c */
int memory_init(void);
void *memory_alloc(uptr size, uptr align, int owner, int zero);
int memory_free(void *block, int owner);
void memory_free_owner(int owner);
u64 memory_total(void);
u64 memory_available(void);
void *memory_reserve_front(uptr size);  /* before the allocator starts */

/* cache.c */
void cache_init(void);
void cache_resume(void);
int cache_overlay_read(u32 lba, void *buffer);
int cache_overlay_write(u32 lba, const void *buffer);

/* volumes.c */
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
int volume_notice(char *buffer, uptr size);
void volume_post_notice(const char *text);

/* terminal.c */
dosxc_ssize terminal_write(const u8 *buffer, uptr size);
dosxc_ssize terminal_read(u8 *buffer, uptr size);
int terminal_poll(void);
int terminal_mode(int mode);
int terminal_get_key(int wait);
void terminal_reset(void);
void terminal_input_reset(void);
int terminal_mouse(struct dosxc_mouse *out);
void terminal_mouse_hide(void);
int terminal_interrupted(void);         /* Ctrl+C typed while a program waits */

/* files.c */
struct open_file;
struct library;
#define MAX_PROCESS_LIBRARIES 24
struct process {
    int used, pid;
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
};
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

/* paging.c */
int paging_map(uptr va, uptr pa);
void paging_resume(void);

/* dynamic.c */
void libraries_activate(struct process *p);
void libraries_release(struct process *p);
const char *library_at(uptr address, uptr *offset);

/* system.c */
char **environment_master(void);

/* power.c */
void power_init(void);
void power_poll(void);
void power_off(void);

/* loader.c */
int process_spawn(struct process *parent, const char *path, char *const argv[],
                  char *const envp[], const char *output, int append);
void process_exit(int wait_status) __attribute__((noreturn));
struct process *process_root(void);
void process_finish(struct process *p);
void runtime_exception(uptr *frame);
