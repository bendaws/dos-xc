/*
 * DOS-XC /libc/include/dosxc/abi.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * The DOS-XC 32/64-bit program ABI (version 2).
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
 *   auxiliary vector.  Programs run in ring 0 and must not use a red zone
 *   (interrupts are taken on the program stack).
 *
 * System calls
 *   A program makes a system call with "int $0x80" (DOSXC_SYSCALL_VECTOR):
 *   the call number (DOSXC_SYS_*) goes in eax/rax and the arguments in
 *   registers; see <dosxc/syscall.h> for the register assignment and C
 *   wrappers.  Calls that can fail return a negative errno value (for
 *   example -DOSXC_ENOENT).
 *
 * The console understands the ANSI/VT100 escape sequences listed in
 * docs/PROGRAMS.md.
 *
 * ABI versions
 *   The note's abi_version selects the system call interface:
 *     1  calls through the struct dosxc_api function table, whose address
 *        is passed in the auxiliary entry DOSXC_AT_API (legacy; still
 *        accepted by the loader)
 *     2  interrupt 0x80 system calls; no DOSXC_AT_API entry is passed
 */

#ifndef _DOSXC_ABI_H
#define _DOSXC_ABI_H

#define DOSXC_ABI_VERSION 2
#define DOSXC_ABI_TABLE 1          /* oldest version: function table calls */
#define DOSXC_NOTE_NAME "DOS-XC"
#define DOSXC_NOTE_ABI 1
#define DOSXC_AT_API 0x42440001    /* ABI 1 only: pointer to struct dosxc_api */
#define DOSXC_AT_BASE 0x42440002   /* load address of the image */
#define DOSXC_AT_INITS 0x42440003  /* shared library constructors to run, 0-terminated */
#define DOSXC_AT_FINIS 0x42440004  /* shared library destructors to run, 0-terminated */
#define DOSXC_SYSCALL_VECTOR 0x80

/* System call numbers (eax/rax for int 0x80). */
#define DOSXC_SYS_EXIT 1
#define DOSXC_SYS_SPAWN 2
#define DOSXC_SYS_GETPID 3
#define DOSXC_SYS_GETPPID 4
#define DOSXC_SYS_SYSTEM 5
#define DOSXC_SYS_RUNDOS 6
#define DOSXC_SYS_REBOOT 7
#define DOSXC_SYS_OPEN 10
#define DOSXC_SYS_CLOSE 11
#define DOSXC_SYS_READ 12
#define DOSXC_SYS_WRITE 13
#define DOSXC_SYS_SEEK 14
#define DOSXC_SYS_DUP 15
#define DOSXC_SYS_DUP2 16
#define DOSXC_SYS_FDFLAGS 17
#define DOSXC_SYS_STATUS_FLAGS 18
#define DOSXC_SYS_PIPE 19
#define DOSXC_SYS_ISATTY 20
#define DOSXC_SYS_FTRUNCATE 21
#define DOSXC_SYS_FSYNC 22
#define DOSXC_SYS_FSTAT 23
#define DOSXC_SYS_STAT 30
#define DOSXC_SYS_UNLINK 31
#define DOSXC_SYS_RENAME 32
#define DOSXC_SYS_MKDIR 33
#define DOSXC_SYS_RMDIR 34
#define DOSXC_SYS_CHDIR 35
#define DOSXC_SYS_GETCWD 36
#define DOSXC_SYS_LISTDIR 37
#define DOSXC_SYS_RESOLVE 38
#define DOSXC_SYS_ALLOC 40
#define DOSXC_SYS_RELEASE 41
#define DOSXC_SYS_TIME 50
#define DOSXC_SYS_NANOSECONDS 51
#define DOSXC_SYS_SLEEP 52
#define DOSXC_SYS_CONSOLE 53
#define DOSXC_SYS_SYSINFO 54
#define DOSXC_SYS_ENVIRON 55
#define DOSXC_SYS_SHELL 56
#define DOSXC_SYS_VOLUMES 57
#define DOSXC_SYS_DISK 58
#define DOSXC_SYS_FULLPATH 59
#define DOSXC_SYS_LIBRARY 60
#define DOSXC_SYS_DRIVE 61
#define DOSXC_SYS_POWER 62
#define DOSXC_SYS_SOCKET 70        /* domain, type, protocol */
#define DOSXC_SYS_CONNECT 71       /* fd, addr, length */
#define DOSXC_SYS_BIND 72          /* fd, addr, length */
#define DOSXC_SYS_LISTEN 73        /* fd, backlog */
#define DOSXC_SYS_ACCEPT 74        /* fd, addr, length pointer */
#define DOSXC_SYS_SENDTO 75        /* fd, struct dosxc_message * */
#define DOSXC_SYS_RECVFROM 76      /* fd, struct dosxc_message * */
#define DOSXC_SYS_SHUTDOWN 77      /* fd, how */
#define DOSXC_SYS_SOCKOPT 78       /* fd, set, level, name, struct dosxc_sockopt * */
#define DOSXC_SYS_SOCKNAME 79      /* fd, peer, addr, length pointer */
#define DOSXC_SYS_FDPOLL 80        /* fd, DOSXC_POLL_* wanted; returns those ready */
#define DOSXC_SYS_SETFL 81         /* fd, DOSXC_O_NONBLOCK | DOSXC_O_APPEND */
#define DOSXC_SYS_NET 82           /* op, a, b */
#define DOSXC_SYS_DRIVER 83        /* op, a, buffer, size */
#define DOSXC_SYS_DISPLAY 84       /* op, a, buffer, size */
#define DOSXC_SYS_TASK 85          /* op, a, b, c */

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
#define DOSXC_S_IFSOCK 0140000

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
    char live;                 /* 1 when running from installation media */
    char reserved[5];
};

/* volumes(): one entry per drive letter. */
#define DOSXC_VOLUME_SYSTEM    1
#define DOSXC_VOLUME_READONLY  2
#define DOSXC_VOLUME_REMOVABLE 4
#define DOSXC_VOLUME_LIVE      8     /* system volume on installation media */
struct dosxc_volume {
    char letter;
    unsigned char type;        /* 12, 16 or 32 (FAT type) */
    unsigned char flags;       /* DOSXC_VOLUME_* */
    unsigned char reserved;
    dosxc_u32 disk;            /* disk() index; meaningless for the system volume */
    dosxc_u64 total_bytes;
    dosxc_u64 free_bytes;
};

/* disk(op, index, lba, buffer, count): whole physical disks, 512-byte
   sectors, at most 64 sectors per transfer.  The disk DOS-XC booted from
   is never writable. */
#define DOSXC_DISK_INFO   1    /* buffer = struct dosxc_disk; returns disk count */
#define DOSXC_DISK_READ   2
#define DOSXC_DISK_WRITE  3
#define DOSXC_DISK_RESCAN 4    /* mount the FAT volumes found on the disks again */
#define DOSXC_DISK_REMOVABLE 1
#define DOSXC_DISK_READONLY  2
#define DOSXC_DISK_SYSTEM    4
#define DOSXC_DISK_USB       8     /* driven by DOS-XC's own USB driver */
struct dosxc_disk {
    dosxc_u64 sectors;
    dosxc_u32 flags;           /* DOSXC_DISK_* */
    dosxc_u32 bios;            /* INT 13h drive number (BIOS boot), else 0 */
};

/* environ(op, name, buffer, size): the system (master) environment.  The
   shell passes it to the programs it starts; SET changes it. */
#define DOSXC_ENV_GET  1       /* value of name into buffer; returns its length */
#define DOSXC_ENV_SET  2       /* buffer = value, or 0 to remove name */
#define DOSXC_ENV_LIST 3       /* "NAME=value\0...\0\0" into buffer; returns the size */

/* shell(op, a, b) */
#define DOSXC_SHELL_CHDIR 1    /* a = path: the command shell's directory (CD in batch files) */
#define DOSXC_SHELL_SPEED 2    /* a = 0: query, else set the 16-bit CPU duty percent (1..100) */

/* console() operations */
#define DOSXC_CONSOLE_CLEAR   1
#define DOSXC_CONSOLE_COLOR   2   /* a = attribute (background << 4 | foreground); | 0x100 recolors the screen */
#define DOSXC_CONSOLE_CURSOR  3   /* a = row, b = column */
#define DOSXC_CONSOLE_GETKEY  4   /* a != 0: wait.  Returns key or -1 */
#define DOSXC_CONSOLE_BEEP    5   /* a = hertz, b = milliseconds */
#define DOSXC_CONSOLE_MODE    6   /* a = DOSXC_CONSOLE_RAW or 0 (line editing + echo) */
#define DOSXC_CONSOLE_POLL    7   /* returns 1 when a read of the console would not wait */
#define DOSXC_CONSOLE_SIZE    8   /* returns rows << 16 | columns */
#define DOSXC_CONSOLE_GETCOLOR 9  /* returns the current attribute */
#define DOSXC_CONSOLE_MOUSE   10  /* a = struct dosxc_mouse *; returns 1 when a mouse is there */
#define DOSXC_CONSOLE_POINTER 11  /* a = struct dosxc_pointer *; returns 1 when a mouse is there */
#define DOSXC_CONSOLE_VIRTUAL 12  /* returns 1 in a terminal window */
#define DOSXC_CONSOLE_SPEAKER 13  /* a = PC speaker PIT divisor, 0 silences it */
#define DOSXC_CONSOLE_VOLUME  14  /* a = 0..100 sets the sound volume, a < 0 reads it */
#define DOSXC_CONSOLE_RAW     1

struct dosxc_mouse {
    dosxc_i32 row, column;     /* text cell under the mouse cursor */
    dosxc_u32 buttons;         /* bit 0 left, 1 right, 2 middle */
    dosxc_i32 wheel;           /* wheel movement since the last call */
};

/* Raw mouse movement since the last call, for graphical programs. */
struct dosxc_pointer {
    dosxc_i32 dx, dy;
    dosxc_i32 wheel;
    dosxc_u32 buttons;         /* bit 0 left, 1 right, 2 middle */
};

/* display(op, a, buffer, size): graphics displays, one per monitor.  Modes
   are 32 bits per pixel, 0x00RRGGBB in memory; the framebuffer address can
   be used directly.  The console is not visible while a display is in
   graphics mode, and every display returns to text mode when the program
   that set a mode ends. */
#define DOSXC_DISPLAY_INFO  1  /* a = index, buffer = struct dosxc_display (or 0); returns the display count */
#define DOSXC_DISPLAY_MODES 2  /* a = index, buffer = struct dosxc_display_mode[size] (or 0); returns the mode count */
#define DOSXC_DISPLAY_SET   3  /* a = index, buffer = struct dosxc_display_mode */
#define DOSXC_DISPLAY_TEXT  4  /* every display back to the text console */

/* DOSXC_SYS_TASK: programs that run side by side. */
#define DOSXC_TASK_START 1     /* a = path, b = argv: returns the pid */
#define DOSXC_TASK_YIELD 2
#define DOSXC_TASK_LIST 3      /* a = struct dosxc_task_info[b]: returns the count */
#define DOSXC_TASK_KILL 4      /* b = pid */
#define DOSXC_TASK_ALIVE 5     /* b = pid: 1 or 0 */
#define DOSXC_TASK_STATS 6     /* a = struct dosxc_task_stats * */
#define DOSXC_TASK_SHARE_SET 7 /* a = memory published to other programs (the desktop's) */
#define DOSXC_TASK_SHARE_GET 8 /* returns it, or 0 */
/* Terminal windows: DOSXC_TASK_START with c = console + 1 runs a program in one. */
#define DOSXC_TASK_CONSOLE_OPEN 9   /* returns the console number */
#define DOSXC_TASK_CONSOLE_READ 10  /* a = bytes, b = console, c = size: what programs wrote */
#define DOSXC_TASK_CONSOLE_KEYS 11  /* a = int keys, b = console, c = count */
#define DOSXC_TASK_CONSOLE_CLOSE 12 /* b = console: ends what runs in it */
struct dosxc_task_info {
    int pid, parent;
    dosxc_u32 background, reserved;
    dosxc_u64 memory, cpu_ns;
    char path[260];
};
struct dosxc_task_stats {
    dosxc_u64 idle_ns, now_ns, memory_total, memory_free;
    dosxc_u32 tasks, reserved;
};
struct dosxc_display_mode {
    dosxc_u32 width, height;
};
struct dosxc_display {
    char name[32];
    dosxc_u32 width, height;   /* the current mode; 0 while in text mode */
    dosxc_u32 pitch;           /* bytes per scan line */
    dosxc_u32 reserved;
    dosxc_u64 framebuffer;     /* physical (and virtual) address */
    dosxc_u32 preferred_width, preferred_height;   /* from the monitor, 0 when unknown */
};

/* drive(op, a, buffer, size): drives coming and going. */
#define DOSXC_DRIVE_EJECT  1   /* a = drive letter: write back, unmount, safe to remove */
#define DOSXC_DRIVE_NOTICE 2   /* next message ("USB drive ready as E:") into buffer; 0 when none */

/* power(op) */
#define DOSXC_POWER_OFF 1      /* write back, switch off; never returns */

/* Sockets.  Addresses are IPv4 sockaddr_in layout (network byte order). */
#define DOSXC_AF_INET 2
#define DOSXC_SOCK_STREAM 1
#define DOSXC_SOCK_DGRAM 2
#define DOSXC_POLL_IN 0x001
#define DOSXC_POLL_OUT 0x004
#define DOSXC_POLL_ERR 0x008
#define DOSXC_POLL_HUP 0x010
#define DOSXC_MSG_PEEK 0x02
#define DOSXC_MSG_WAITALL 0x100
#define DOSXC_MSG_DONTWAIT 0x40
struct dosxc_sockaddr_in {
    unsigned short family;
    unsigned short port;
    dosxc_u32 address;
    unsigned char zero[8];
};
struct dosxc_message {
    void *buffer;
    dosxc_size length;
    int flags;
    void *address;             /* sockaddr, or 0 */
    dosxc_u32 *address_length; /* in/out for receive; points at the size for send */
};
struct dosxc_sockopt {
    void *value;
    dosxc_u32 *length;
};

/* net(op, a, b) */
#define DOSXC_NET_INFO   1     /* a = adapter, b = struct dosxc_netinfo *; returns the adapter count */
#define DOSXC_NET_CONFIG 2     /* a = adapter, b = struct dosxc_netconfig * */
#define DOSXC_NET_RENEW  3     /* a = adapter: ask the DHCP server again */
#define DOSXC_NET_PING   4     /* a = IPv4 address (host order), b = timeout ms; returns microseconds */
struct dosxc_netinfo {
    char name[16];
    unsigned char mac[6];
    unsigned char link;        /* carrier present */
    unsigned char dhcp;        /* 1: addresses came from DHCP */
    dosxc_u32 speed;           /* Mbit/s */
    dosxc_u32 ip, mask, gateway, dns[2];   /* host byte order */
    dosxc_u32 lease;           /* seconds left of the DHCP lease */
    dosxc_u64 rx_packets, tx_packets, rx_bytes, tx_bytes, rx_dropped;
};
struct dosxc_netconfig {
    dosxc_u32 dhcp;            /* 1: use DHCP and ignore the rest */
    dosxc_u32 ip, mask, gateway, dns[2];   /* host byte order */
};

/* driver(op, a, buffer, size): device drivers in \\SYSTEM\\DRIVERS */
#define DOSXC_DRIVER_LIST   1  /* a = index, buffer = struct dosxc_driverinfo; returns the count */
#define DOSXC_DRIVER_LOAD   2  /* buffer = path of the .DRV file */
#define DOSXC_DRIVER_UNLOAD 3  /* buffer = file name, e.g. "E1000.DRV" */
#define DOSXC_DRIVER_LOG    4  /* buffer: copy of the driver log; returns its length */
#define DOSXC_DRIVER_STATE_LOADED 1   /* running, with at least one device */
#define DOSXC_DRIVER_STATE_IDLE   2   /* running, its hardware is not in this computer */
#define DOSXC_DRIVER_STATE_FAILED 3   /* stopped after a processor exception */
struct dosxc_driverinfo {
    char file[32];
    char name[32];
    char version[16];
    char description[64];
    dosxc_u32 state;           /* DOSXC_DRIVER_STATE_* */
    dosxc_u32 devices;
};

/* fdflags() */
#define DOSXC_FD_CLOEXEC 1

/* Wait status encoding, identical to <sys/wait.h>. */
#define DOSXC_EXITED(code) (((code) & 0xff) << 8)
#define DOSXC_SIGNALED(signal) ((signal) & 0x7f)

/* The ABI 1 system table.  The runtime still provides it to version 1
   programs; each entry matches the system call of the same name. */
struct dosxc_api {
    dosxc_u32 version;        /* 1 */
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
#define DOSXC_ENOTSOCK 88
#define DOSXC_EDESTADDRREQ 89
#define DOSXC_EMSGSIZE 90
#define DOSXC_EPROTOTYPE 91
#define DOSXC_ENOPROTOOPT 92
#define DOSXC_EPROTONOSUPPORT 93
#define DOSXC_EOPNOTSUPP 95
#define DOSXC_EAFNOSUPPORT 97
#define DOSXC_EADDRINUSE 98
#define DOSXC_EADDRNOTAVAIL 99
#define DOSXC_ENETDOWN 100
#define DOSXC_ENETUNREACH 101
#define DOSXC_ECONNABORTED 103
#define DOSXC_ECONNRESET 104
#define DOSXC_ENOBUFS 105
#define DOSXC_EISCONN 106
#define DOSXC_ENOTCONN 107
#define DOSXC_ETIMEDOUT 110
#define DOSXC_ECONNREFUSED 111
#define DOSXC_EHOSTUNREACH 113
#define DOSXC_EALREADY 114
#define DOSXC_EINPROGRESS 115

#endif
