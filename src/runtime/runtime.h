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
void rt_print(const char *);
void rt_print_hex(u64);
void rt_print_decimal(u64);
i64 rt_epoch(unsigned year, unsigned month, unsigned day, unsigned hour, unsigned minute, unsigned second);
static inline void rt_outb(u16 port, u8 value) { __asm__ volatile("outb %0,%1" ::"a"(value), "Nd"(port)); }
static inline u8 rt_inb(u16 port) { u8 v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(port)); return v; }

/* cpu.c */
struct saved_context { uptr registers[16]; };
int cpu_init(void);
int cpu_has_sse(void);
void cpu_install_idt(void);
void cpu_restore_idt(void);
u64 cpu_nanoseconds(void);
void cpu_sleep(u64 nanoseconds);
i64 cpu_rtc_time(void);
int runtime_enter(uptr entry, uptr stack, struct saved_context *save, int sse);
void runtime_leave(struct saved_context *save, int value) __attribute__((noreturn));
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

/* files.c */
struct open_file;
struct process {
    int used, pid;
    struct process *parent;
    struct open_file *fds[MAX_FDS];
    u8 cloexec[MAX_FDS];
    char cwd[PATH_MAX_RT];       /* canonical "\DIR\SUB" form */
    char path[PATH_MAX_RT];      /* canonical program path */
    uptr image_base, image_size;
    struct saved_context saved;
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

/* loader.c */
int process_spawn(struct process *parent, const char *path, char *const argv[],
                  char *const envp[], const char *output, int append);
void process_exit(int wait_status) __attribute__((noreturn));
void runtime_exception(uptr *frame);
