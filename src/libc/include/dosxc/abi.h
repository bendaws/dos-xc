/*
 * DOS-XC /libc/include/dosxc/abi.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * The DOS-XC 32/64-bit program ABI (version 1).
 *
 * Executable format
 *   A DOS-XC program is an ELF32 (i386) or ELF64 (x86-64) file named
 *   *.EXE.  It is a static position-independent executable: ET_DYN, no
 *   PT_INTERP, no DT_NEEDED, and only RELATIVE / IRELATIVE relocations
 *   (DT_REL, DT_RELA or DT_RELR).  It carries an SHT_NOTE section named
 *   ".note.dosxc" whose note has name "DOS-XC", type DOSXC_NOTE_ABI and
 *   a struct dosxc_note_abi descriptor.  The loader rejects ELF files
 *   without the note.  PT_GNU_STACK's p_memsz, when non-zero, requests
 *   the stack size.
 *
 * Process start
 *   The entry point is entered with the System V process layout: the stack
 *   pointer addresses argc, followed by argv[], NULL, envp[], NULL and an
 *   auxiliary vector.  The auxiliary entry DOSXC_AT_API holds a pointer to
 *   the struct dosxc_api system table.  Programs run in ring 0 and must not
 *   use a red zone (interrupts are taken on the program stack).
 *
 * System calls
 *   Every entry of struct dosxc_api is an ordinary function using the
 *   platform C calling convention.  Calls that can fail return a negative
 *   errno value (for example -DOSXC_ENOENT).
 */

#ifndef _DOSXC_ABI_H
#define _DOSXC_ABI_H

#define DOSXC_ABI_VERSION 1
#define DOSXC_NOTE_NAME "DOS-XC"
#define DOSXC_NOTE_ABI 1
#define DOSXC_AT_API 0x42440001

typedef __INT32_TYPE__ dosxc_i32;
typedef __UINT32_TYPE__ dosxc_u32;
typedef __INT64_TYPE__ dosxc_i64;
typedef __UINT64_TYPE__ dosxc_u64;
typedef __SIZE_TYPE__ dosxc_size;
typedef __PTRDIFF_TYPE__ dosxc_ssize;

struct dosxc_note_abi {
    dosxc_u32 abi_version;
    dosxc_u32 flags;
};

/* Open flags (shared with <fcntl.h>). */
#define DOSXC_O_RDONLY    0x0000
#define DOSXC_O_WRONLY    0x0001
#define DOSXC_O_RDWR      0x0002
#define DOSXC_O_ACCMODE   0x0003
#define DOSXC_O_CREAT     0x0040
#define DOSXC_O_EXCL      0x0080
#define DOSXC_O_NOCTTY    0x0100
#define DOSXC_O_TRUNC     0x0200
#define DOSXC_O_APPEND    0x0400
#define DOSXC_O_NONBLOCK  0x0800
#define DOSXC_O_DIRECTORY 0x10000
#define DOSXC_O_CLOEXEC   0x80000

/* File types reported in dosxc_stat.mode (shared with <sys/stat.h>). */
#define DOSXC_S_IFMT   0170000
#define DOSXC_S_IFDIR  0040000
#define DOSXC_S_IFCHR  0020000
#define DOSXC_S_IFIFO  0010000
#define DOSXC_S_IFREG  0100000

/* FAT attributes reported in dosxc_stat.attributes. */
#define DOSXC_ATTR_READONLY  0x01
#define DOSXC_ATTR_HIDDEN    0x02
#define DOSXC_ATTR_SYSTEM    0x04
#define DOSXC_ATTR_DIRECTORY 0x10
#define DOSXC_ATTR_ARCHIVE   0x20

struct dosxc_stat {
    dosxc_u64 size;
    dosxc_i64 mtime;          /* seconds since 1970-01-01, local time */
    dosxc_u32 mode;           /* DOSXC_S_IF* | permission bits */
    dosxc_u32 attributes;     /* FAT attribute byte, 0 for devices */
    dosxc_u32 device;
    dosxc_u32 inode;          /* stable hash of the canonical path */
};

struct dosxc_dirent {
    char name[256];
    dosxc_u32 attributes;
    dosxc_u32 reserved;
    dosxc_u64 size;
    dosxc_i64 mtime;
};

struct dosxc_sysinfo {
    dosxc_u32 abi_version;
    dosxc_u32 bits;           /* 32 or 64 */
    dosxc_u64 memory_total;   /* bytes managed for programs */
    dosxc_u64 memory_free;
    char version[32];          /* "DOS-XC v0.2" */
    char drive;                /* system drive letter */
    char firmware;             /* 'B' for BIOS, 'U' for UEFI */
    char reserved[6];
};

/* console() operations */
#define DOSXC_CONSOLE_CLEAR   1
#define DOSXC_CONSOLE_COLOR   2   /* a = attribute (background << 4 | foreground) */
#define DOSXC_CONSOLE_CURSOR  3   /* a = row, b = column */
#define DOSXC_CONSOLE_GETKEY  4   /* a != 0: wait.  Returns key or -1 */
#define DOSXC_CONSOLE_BEEP    5   /* a = hertz, b = milliseconds */
#define DOSXC_CONSOLE_MODE    6   /* a = DOSXC_CONSOLE_RAW or 0 (line editing + echo) */
#define DOSXC_CONSOLE_RAW     1

/* fdflags() */
#define DOSXC_FD_CLOEXEC 1

/* Wait status encoding, identical to <sys/wait.h>. */
#define DOSXC_EXITED(code) (((code) & 0xff) << 8)
#define DOSXC_SIGNALED(signal) ((signal) & 0x7f)

struct dosxc_api {
    dosxc_u32 version;        /* DOSXC_ABI_VERSION */
    dosxc_u32 size;           /* sizeof (struct dosxc_api) */

    /* Process control.  exit() takes a wait status (DOSXC_EXITED/SIGNALED).
       spawn() runs a program to completion and returns its wait status. */
    void (*exit)(int wait_status) __attribute__((noreturn));
    int (*spawn)(const char *path, char *const argv[], char *const envp[]);
    int (*getpid)(void);
    int (*getppid)(void);
    int (*system)(const char *command_line);

    /* File descriptors. */
    int (*open)(const char *path, int flags, int mode);
    int (*close)(int fd);
    dosxc_ssize (*read)(int fd, void *buffer, dosxc_size size);
    dosxc_ssize (*write)(int fd, const void *buffer, dosxc_size size);
    dosxc_i64 (*seek)(int fd, dosxc_i64 offset, int whence);
    int (*dup)(int fd, int minimum);          /* lowest free fd >= minimum */
    int (*dup2)(int fd, int target);
    int (*fdflags)(int fd, int set, int flags); /* returns flags; sets if set */
    int (*status_flags)(int fd);              /* O_ACCMODE | O_APPEND */
    int (*pipe)(int fds[2]);
    int (*isatty)(int fd);
    int (*ftruncate)(int fd, dosxc_i64 length);
    int (*fsync)(int fd);
    int (*fstat)(int fd, struct dosxc_stat *status);

    /* Paths: '/' and '\' both separate components; "C:" is optional. */
    int (*stat)(const char *path, struct dosxc_stat *status);
    int (*unlink)(const char *path);
    int (*rename)(const char *from, const char *to);
    int (*mkdir)(const char *path);
    int (*rmdir)(const char *path);
    int (*chdir)(const char *path);
    int (*getcwd)(char *buffer, dosxc_size size);   /* "/DIR/SUB" form */
    dosxc_ssize (*listdir)(const char *path, struct dosxc_dirent *entries,
                            dosxc_size capacity);   /* returns total count */
    int (*resolve)(const char *path, char *buffer, dosxc_size size);

    /* Memory: page-aligned, zero-filled blocks owned by the process. */
    void *(*alloc)(dosxc_size size);
    int (*release)(void *block);

    /* Time and system. */
    dosxc_i64 (*time)(void);                  /* local seconds since 1970 */
    dosxc_u64 (*nanoseconds)(void);           /* monotonic, since boot */
    void (*sleep)(dosxc_u64 nanoseconds);
    int (*console)(int operation, long a, long b);
    int (*sysinfo)(struct dosxc_sysinfo *info);
};

/* errno values (shared with <errno.h>). */
#define DOSXC_EPERM 1
#define DOSXC_ENOENT 2
#define DOSXC_ESRCH 3
#define DOSXC_EINTR 4
#define DOSXC_EIO 5
#define DOSXC_ENXIO 6
#define DOSXC_E2BIG 7
#define DOSXC_ENOEXEC 8
#define DOSXC_EBADF 9
#define DOSXC_ECHILD 10
#define DOSXC_EAGAIN 11
#define DOSXC_ENOMEM 12
#define DOSXC_EACCES 13
#define DOSXC_EFAULT 14
#define DOSXC_EBUSY 16
#define DOSXC_EEXIST 17
#define DOSXC_EXDEV 18
#define DOSXC_ENODEV 19
#define DOSXC_ENOTDIR 20
#define DOSXC_EISDIR 21
#define DOSXC_EINVAL 22
#define DOSXC_ENFILE 23
#define DOSXC_EMFILE 24
#define DOSXC_ENOTTY 25
#define DOSXC_EFBIG 27
#define DOSXC_ENOSPC 28
#define DOSXC_ESPIPE 29
#define DOSXC_EROFS 30
#define DOSXC_EMLINK 31
#define DOSXC_EPIPE 32
#define DOSXC_EDOM 33
#define DOSXC_ERANGE 34
#define DOSXC_ENAMETOOLONG 36
#define DOSXC_ENOSYS 38
#define DOSXC_ENOTEMPTY 39
#define DOSXC_ELOOP 40

#ifndef __ASSEMBLER__
#ifdef __cplusplus
extern "C" {
#endif
/* Set by crt0 from the auxiliary vector. */
extern const struct dosxc_api *__dosxc;
#ifdef __cplusplus
}
#endif
#endif

#endif
