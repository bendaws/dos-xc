/*
 * DOS-XC /programs/api.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * The interface between the kernel's init module (SHELL.DLL) and
 * \SYSTEM\ELFAPI.DLL, the 32/64-bit program runtime.  Every user program,
 * the command shell included, is an ELF program run by the runtime.
 */

#pragma once
#include "../drivers/fat32.h"
#define RUNTIME_BASE 0x4400000u
#define RUNTIME_LIMIT 0xc00000u
#define CONTEXT_SAVE 0x4200000u     /* context kept across a 16-bit program */
#define PATH_LIMIT 260
#define KEY_UP 0x101
#define KEY_DOWN 0x102
#define KEY_RIGHT 0x103
#define KEY_LEFT 0x104
#define KEY_HOME 0x105
#define KEY_END 0x106
#define KEY_INSERT 0x107
#define KEY_DELETE 0x108
#define KEY_PAGE_UP 0x109
#define KEY_PAGE_DOWN 0x10a
#define KEY_F1 0x10b
#define KEY_F2 0x10c
#define KEY_F3 0x10d
#define KEY_F10 0x114

/* Firmware disk flags (UEFI disk_info). */
#define DISK_FLAG_REMOVABLE 1
#define DISK_FLAG_READONLY 2
#define DISK_FLAG_SYSTEM 4          /* the disk DOS-XC booted from */

typedef unsigned (__attribute__((sysv_abi)) *disk_count_fn)(void);
typedef int (__attribute__((sysv_abi)) *disk_info_fn)(unsigned disk, uint64_t *sectors, unsigned *flags);
typedef int (__attribute__((sysv_abi)) *disk_io_fn)(unsigned disk, uint64_t lba, unsigned count, void *buffer, int write);

struct command_context {
    unsigned abi;
    struct fat32_volume *volume;     /* the system volume */
    char drive;                      /* its drive letter */
    char cwd[PATH_LIMIT];            /* 16-bit program directory (system volume) */
    int errorlevel;
    unsigned speed;                  /* 16-bit CPU duty percent */
    int bios_available;
    void (*write)(const char *);
    void (*putc)(char);
    int (*read_key)(void);
    void (*clear)(void);
    void (*color)(unsigned char);
    void (*cursor)(unsigned,unsigned);
    void (*beep)(unsigned,unsigned);
    int (*stat)(const struct fat32_volume *,const char *,struct fat32_info *);
    int (*list)(const struct fat32_volume *,const char *,fat32_list_fn,void *);
    int (*read)(const struct fat32_volume *,const char *,void *,uint32_t);
    int (*save)(const struct fat32_volume *,const char *,const void *,uint32_t);
    int (*mkdir)(const struct fat32_volume *,const char *);
    int (*remove)(const struct fat32_volume *,const char *);
    int (*rmdir)(const struct fat32_volume *,const char *);
    int (*rename)(const struct fat32_volume *,const char *,const char *);
    int (*copy_file)(const struct fat32_volume *,const char *,const char *);
    uint64_t (*free_bytes)(const struct fat32_volume *);
    void (*text_color)(unsigned char);
    int (*read_at)(const struct fat32_volume *,const char *,uint32_t,void *,uint32_t);
    int (*mount)(struct fat32_volume *,uint32_t);
    int (*read_cursor)(const struct fat32_volume *,const char *,struct fat32_cursor *,uint32_t,void *,uint32_t);
    uint64_t memory_base, memory_size;   /* firmware-reserved RAM (UEFI), or 0 */
    /* Console primitives for the runtime's terminal emulation. */
    void (*get_cursor)(unsigned *row, unsigned *column);
    void (*put_at)(unsigned row, unsigned column, char character);
    void (*erase)(unsigned row, unsigned column, unsigned count);
    unsigned char (*get_color)(void);
    /* Moves rows top..bottom by one line (up != 0: towards the top). 0 when unsupported. */
    int (*scroll)(unsigned top, unsigned bottom, int up);
    /* Runs a 16-bit program: never returns.  The kernel restarts after it
       and calls the runtime's resume entry.  0 on UEFI boot. */
    void (*run_dos)(const char *file, const char *arguments, const char *cwd);
    void (*reboot)(void);
    /* UEFI firmware disks; 0 on BIOS boot (the runtime uses INT 13h). */
    disk_count_fn disk_count;
    disk_info_fn disk_info;
    disk_io_fn disk_io;
    int live;                        /* booted from installation media */
    /* UEFI: look at the firmware's disks again (indices stay; gone disks
       fail disk_info) and read its pointing devices.  0 when absent. */
    unsigned (__attribute__((sysv_abi)) *disk_rescan)(void);
    int (__attribute__((sysv_abi)) *pointer)(int *dx, int *dy, int *wheel, unsigned *buttons);
    /* The 8.3 form of a path on a volume (16-bit programs only see aliases). */
    int (*short_path)(const struct fat32_volume *, const char *, char *, uint32_t);
    /* UEFI: switch off through the firmware, and where its ACPI RSDP is.
       BIOS boot: 0 (the runtime finds the RSDP itself). */
    void (__attribute__((sysv_abi)) *poweroff)(void);
    uint64_t acpi_rsdp;
};

/* \SYSTEM\ELFAPI.DLL: the 32/64-bit ELF program runtime. */
#define RUNTIME_MAGIC 0x46454442u   /* "BDEF" */
#define RUNTIME_VERSION 2
struct runtime_header {
    unsigned magic, version, bits, base, init, run, bss_start, bss_end, resume;
};
typedef int (*runtime_init_fn)(struct command_context *);
typedef int (*runtime_run_fn)(const char *path,const char *arguments);
/* Continues the 32/64-bit program that started a 16-bit one.  Returns
   when that program chain has finished, or -1 when nothing was waiting. */
typedef int (*runtime_resume_fn)(struct command_context *,int dos_exit_code);
