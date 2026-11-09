/*
 * DOS-XC /libc/include/dosxc/syscall.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * System calls through interrupt 0x80 (ABI version 2).
 *
 *   x86-64:  rax = number; arguments in rdi, rsi, rdx, r10, r8, r9.
 *            The result is returned in rax.
 *   i386:    eax = number; arguments in ebx, ecx, edx, esi, edi, ebp.
 *            The result is returned in eax.  64-bit arguments take two
 *            consecutive registers (low half first); calls with a 64-bit
 *            result (seek, time, nanoseconds) return it in edx:eax.
 *
 * Every other register, including the x87/SSE state and the flags, is
 * preserved.  Calls that can fail return a negative errno value.  The
 * dosxc_sys_* functions below wrap every call with C types.
 */

#ifndef _DOSXC_SYSCALL_H
#define _DOSXC_SYSCALL_H

#include <dosxc/abi.h>

#ifndef __ASSEMBLER__

#if defined(__x86_64__)
typedef long __dosxc_word;
#define __DOSXC_SYSCALL(r, n, ...) __asm__ volatile("int $0x80" : "=a"(r) : "a"((__dosxc_word)(n)), ##__VA_ARGS__ : "memory")
#define __DOSXC_R1 "D"
#define __DOSXC_R2 "S"
#define __DOSXC_R3 "d"
#elif defined(__i386__)
typedef long __dosxc_word;
#define __DOSXC_SYSCALL(r, n, ...) __asm__ volatile("int $0x80" : "=a"(r) : "a"((__dosxc_word)(n)), ##__VA_ARGS__ : "memory")
#define __DOSXC_R1 "b"
#define __DOSXC_R2 "c"
#define __DOSXC_R3 "d"
#else
#error "DOS-XC system calls need an x86 or x86-64 target"
#endif

static __inline__ __dosxc_word __dosxc_syscall0(__dosxc_word n) {
    __dosxc_word r;
    __DOSXC_SYSCALL(r, n);
    return r;
}
static __inline__ __dosxc_word __dosxc_syscall1(__dosxc_word n, __dosxc_word a) {
    __dosxc_word r;
    __DOSXC_SYSCALL(r, n, __DOSXC_R1(a));
    return r;
}
static __inline__ __dosxc_word __dosxc_syscall2(__dosxc_word n, __dosxc_word a, __dosxc_word b) {
    __dosxc_word r;
    __DOSXC_SYSCALL(r, n, __DOSXC_R1(a), __DOSXC_R2(b));
    return r;
}
static __inline__ __dosxc_word __dosxc_syscall3(__dosxc_word n, __dosxc_word a, __dosxc_word b, __dosxc_word c) {
    __dosxc_word r;
    __DOSXC_SYSCALL(r, n, __DOSXC_R1(a), __DOSXC_R2(b), __DOSXC_R3(c));
    return r;
}

#if defined(__x86_64__)
static __inline__ __dosxc_word __dosxc_syscall4(__dosxc_word n, __dosxc_word a, __dosxc_word b, __dosxc_word c,
                                                __dosxc_word d) {
    __dosxc_word r;
    register __dosxc_word r10 __asm__("r10") = d;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "memory");
    return r;
}
static __inline__ __dosxc_word __dosxc_syscall5(__dosxc_word n, __dosxc_word a, __dosxc_word b, __dosxc_word c,
                                                __dosxc_word d, __dosxc_word e) {
    __dosxc_word r;
    register __dosxc_word r10 __asm__("r10") = d;
    register __dosxc_word r8 __asm__("r8") = e;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8) : "memory");
    return r;
}
#endif

#if defined(__i386__)
static __inline__ __dosxc_word __dosxc_syscall5(__dosxc_word n, __dosxc_word a, __dosxc_word b, __dosxc_word c,
                                                __dosxc_word d, __dosxc_word e) {
    __dosxc_word r;
    __DOSXC_SYSCALL(r, n, "b"(a), "c"(b), "d"(c), "S"(d), "D"(e));
    return r;
}
static __inline__ __dosxc_word __dosxc_syscall4(__dosxc_word n, __dosxc_word a, __dosxc_word b, __dosxc_word c,
                                                __dosxc_word d) {
    __dosxc_word r;
    __DOSXC_SYSCALL(r, n, "b"(a), "c"(b), "d"(c), "S"(d));
    return r;
}
/* A call whose result is 64 bits wide (edx:eax). */
static __inline__ dosxc_i64 __dosxc_syscall_wide(__dosxc_word n) {
    dosxc_i64 r;
    __asm__ volatile("int $0x80" : "=A"(r) : "a"(n) : "memory");
    return r;
}
#define __DOSXC_LOW(x) ((__dosxc_word)(dosxc_u32)(dosxc_u64)(x))
#define __DOSXC_HIGH(x) ((__dosxc_word)(dosxc_u32)((dosxc_u64)(x) >> 32))
#endif

/* ---- Typed wrappers ---- */

static __inline__ __attribute__((__noreturn__)) void dosxc_sys_exit(int wait_status) {
    __dosxc_syscall1(DOSXC_SYS_EXIT, wait_status);
    __builtin_unreachable();
}
static __inline__ int dosxc_sys_spawn(const char *path, char *const argv[], char *const envp[]) {
    return (int)__dosxc_syscall3(DOSXC_SYS_SPAWN, (__dosxc_word)path, (__dosxc_word)argv, (__dosxc_word)envp);
}
static __inline__ int dosxc_sys_getpid(void) { return (int)__dosxc_syscall0(DOSXC_SYS_GETPID); }
static __inline__ int dosxc_sys_getppid(void) { return (int)__dosxc_syscall0(DOSXC_SYS_GETPPID); }
static __inline__ int dosxc_sys_system(const char *command_line) {
    return (int)__dosxc_syscall1(DOSXC_SYS_SYSTEM, (__dosxc_word)command_line);
}

static __inline__ int dosxc_sys_open(const char *path, int flags, int mode) {
    return (int)__dosxc_syscall3(DOSXC_SYS_OPEN, (__dosxc_word)path, flags, mode);
}
static __inline__ int dosxc_sys_close(int fd) { return (int)__dosxc_syscall1(DOSXC_SYS_CLOSE, fd); }
static __inline__ dosxc_ssize dosxc_sys_read(int fd, void *buffer, dosxc_size size) {
    return (dosxc_ssize)__dosxc_syscall3(DOSXC_SYS_READ, fd, (__dosxc_word)buffer, (__dosxc_word)size);
}
static __inline__ dosxc_ssize dosxc_sys_write(int fd, const void *buffer, dosxc_size size) {
    return (dosxc_ssize)__dosxc_syscall3(DOSXC_SYS_WRITE, fd, (__dosxc_word)buffer, (__dosxc_word)size);
}
static __inline__ dosxc_i64 dosxc_sys_seek(int fd, dosxc_i64 offset, int whence) {
#if defined(__x86_64__)
    return __dosxc_syscall3(DOSXC_SYS_SEEK, fd, offset, whence);
#else
    dosxc_i64 r;
    __asm__ volatile("int $0x80"
                     : "=A"(r)
                     : "a"((__dosxc_word)DOSXC_SYS_SEEK), "b"((__dosxc_word)fd), "c"(__DOSXC_LOW(offset)),
                       "d"(__DOSXC_HIGH(offset)), "S"((__dosxc_word)whence)
                     : "memory");
    return r;
#endif
}
static __inline__ int dosxc_sys_dup(int fd, int minimum) { return (int)__dosxc_syscall2(DOSXC_SYS_DUP, fd, minimum); }
static __inline__ int dosxc_sys_dup2(int fd, int target) { return (int)__dosxc_syscall2(DOSXC_SYS_DUP2, fd, target); }
static __inline__ int dosxc_sys_fdflags(int fd, int set, int flags) {
    return (int)__dosxc_syscall3(DOSXC_SYS_FDFLAGS, fd, set, flags);
}
static __inline__ int dosxc_sys_status_flags(int fd) { return (int)__dosxc_syscall1(DOSXC_SYS_STATUS_FLAGS, fd); }
static __inline__ int dosxc_sys_pipe(int fds[2]) { return (int)__dosxc_syscall1(DOSXC_SYS_PIPE, (__dosxc_word)fds); }
static __inline__ int dosxc_sys_isatty(int fd) { return (int)__dosxc_syscall1(DOSXC_SYS_ISATTY, fd); }
static __inline__ int dosxc_sys_ftruncate(int fd, dosxc_i64 length) {
#if defined(__x86_64__)
    return (int)__dosxc_syscall2(DOSXC_SYS_FTRUNCATE, fd, length);
#else
    return (int)__dosxc_syscall3(DOSXC_SYS_FTRUNCATE, fd, __DOSXC_LOW(length), __DOSXC_HIGH(length));
#endif
}
static __inline__ int dosxc_sys_fsync(int fd) { return (int)__dosxc_syscall1(DOSXC_SYS_FSYNC, fd); }
static __inline__ int dosxc_sys_fstat(int fd, struct dosxc_stat *status) {
    return (int)__dosxc_syscall2(DOSXC_SYS_FSTAT, fd, (__dosxc_word)status);
}

static __inline__ int dosxc_sys_stat(const char *path, struct dosxc_stat *status) {
    return (int)__dosxc_syscall2(DOSXC_SYS_STAT, (__dosxc_word)path, (__dosxc_word)status);
}
static __inline__ int dosxc_sys_unlink(const char *path) { return (int)__dosxc_syscall1(DOSXC_SYS_UNLINK, (__dosxc_word)path); }
static __inline__ int dosxc_sys_rename(const char *from, const char *to) {
    return (int)__dosxc_syscall2(DOSXC_SYS_RENAME, (__dosxc_word)from, (__dosxc_word)to);
}
static __inline__ int dosxc_sys_mkdir(const char *path) { return (int)__dosxc_syscall1(DOSXC_SYS_MKDIR, (__dosxc_word)path); }
static __inline__ int dosxc_sys_rmdir(const char *path) { return (int)__dosxc_syscall1(DOSXC_SYS_RMDIR, (__dosxc_word)path); }
static __inline__ int dosxc_sys_chdir(const char *path) { return (int)__dosxc_syscall1(DOSXC_SYS_CHDIR, (__dosxc_word)path); }
static __inline__ int dosxc_sys_getcwd(char *buffer, dosxc_size size) {
    return (int)__dosxc_syscall2(DOSXC_SYS_GETCWD, (__dosxc_word)buffer, (__dosxc_word)size);
}
static __inline__ dosxc_ssize dosxc_sys_listdir(const char *path, struct dosxc_dirent *entries, dosxc_size capacity) {
    return (dosxc_ssize)__dosxc_syscall3(DOSXC_SYS_LISTDIR, (__dosxc_word)path, (__dosxc_word)entries,
                                         (__dosxc_word)capacity);
}
static __inline__ int dosxc_sys_resolve(const char *path, char *buffer, dosxc_size size) {
    return (int)__dosxc_syscall3(DOSXC_SYS_RESOLVE, (__dosxc_word)path, (__dosxc_word)buffer, (__dosxc_word)size);
}

static __inline__ void *dosxc_sys_alloc(dosxc_size size) {
    return (void *)__dosxc_syscall1(DOSXC_SYS_ALLOC, (__dosxc_word)size);
}
static __inline__ int dosxc_sys_release(void *block) { return (int)__dosxc_syscall1(DOSXC_SYS_RELEASE, (__dosxc_word)block); }

static __inline__ dosxc_i64 dosxc_sys_time(void) {
#if defined(__x86_64__)
    return __dosxc_syscall0(DOSXC_SYS_TIME);
#else
    return __dosxc_syscall_wide(DOSXC_SYS_TIME);
#endif
}
static __inline__ dosxc_u64 dosxc_sys_nanoseconds(void) {
#if defined(__x86_64__)
    return (dosxc_u64)__dosxc_syscall0(DOSXC_SYS_NANOSECONDS);
#else
    return (dosxc_u64)__dosxc_syscall_wide(DOSXC_SYS_NANOSECONDS);
#endif
}
static __inline__ void dosxc_sys_sleep(dosxc_u64 nanoseconds) {
#if defined(__x86_64__)
    __dosxc_syscall1(DOSXC_SYS_SLEEP, (__dosxc_word)nanoseconds);
#else
    __dosxc_syscall2(DOSXC_SYS_SLEEP, __DOSXC_LOW(nanoseconds), __DOSXC_HIGH(nanoseconds));
#endif
}
static __inline__ int dosxc_sys_console(int operation, long a, long b) {
    return (int)__dosxc_syscall3(DOSXC_SYS_CONSOLE, operation, a, b);
}
static __inline__ int dosxc_sys_sysinfo(struct dosxc_sysinfo *info) {
    return (int)__dosxc_syscall1(DOSXC_SYS_SYSINFO, (__dosxc_word)info);
}


/* ---- DOS-XC system services ---- */

/* Runs a 16-bit DOS program on the system drive; returns its exit code. */
static __inline__ int dosxc_sys_rundos(const char *path, const char *arguments) {
    return (int)__dosxc_syscall2(DOSXC_SYS_RUNDOS, (__dosxc_word)path, (__dosxc_word)arguments);
}
static __inline__ int dosxc_sys_reboot(void) { return (int)__dosxc_syscall0(DOSXC_SYS_REBOOT); }
static __inline__ int dosxc_sys_environ(int operation, const char *name, char *buffer, dosxc_size size) {
    return (int)__dosxc_syscall4(DOSXC_SYS_ENVIRON, operation, (__dosxc_word)name, (__dosxc_word)buffer,
                                 (__dosxc_word)size);
}
static __inline__ int dosxc_sys_shell(int operation, const void *a, long b) {
    return (int)__dosxc_syscall3(DOSXC_SYS_SHELL, operation, (__dosxc_word)a, b);
}
static __inline__ int dosxc_sys_volumes(struct dosxc_volume *entries, unsigned capacity) {
    return (int)__dosxc_syscall2(DOSXC_SYS_VOLUMES, (__dosxc_word)entries, capacity);
}
static __inline__ int dosxc_sys_disk(int operation, unsigned disk, dosxc_u32 lba, void *buffer, unsigned count) {
    return (int)__dosxc_syscall5(DOSXC_SYS_DISK, operation, disk, (__dosxc_word)lba, (__dosxc_word)buffer, count);
}
static __inline__ int dosxc_sys_power(int operation) {
    return (int)__dosxc_syscall1(DOSXC_SYS_POWER, operation);
}
static __inline__ int dosxc_sys_drive(int operation, int letter, char *buffer, dosxc_size size) {
    return (int)__dosxc_syscall4(DOSXC_SYS_DRIVE, operation, letter, (__dosxc_word)buffer, (__dosxc_word)size);
}
/* Sockets (<sys/socket.h> is the portable interface to these). */
static __inline__ int dosxc_sys_net(int operation, long a, void *b) {
    return (int)__dosxc_syscall3(DOSXC_SYS_NET, operation, a, (__dosxc_word)b);
}
static __inline__ int dosxc_sys_driver(int operation, long a, void *buffer, dosxc_size size) {
    return (int)__dosxc_syscall4(DOSXC_SYS_DRIVER, operation, a, (__dosxc_word)buffer, (__dosxc_word)size);
}
static __inline__ int dosxc_sys_display(int operation, long a, void *buffer, dosxc_size size) {
    return (int)__dosxc_syscall4(DOSXC_SYS_DISPLAY, operation, a, (__dosxc_word)buffer, (__dosxc_word)size);
}
static __inline__ long dosxc_sys_task(int operation, void *a, long b, long c) {
    return (long)__dosxc_syscall4(DOSXC_SYS_TASK, operation, (__dosxc_word)a, b, c);
}
static __inline__ int dosxc_sys_fdpoll(int fd, int events) {
    return (int)__dosxc_syscall2(DOSXC_SYS_FDPOLL, fd, events);
}
static __inline__ int dosxc_sys_setfl(int fd, int flags) {
    return (int)__dosxc_syscall2(DOSXC_SYS_SETFL, fd, flags);
}

/* The DOS form of a path: "C:\\DIR\\FILE". */
static __inline__ int dosxc_sys_fullpath(const char *path, char *buffer, dosxc_size size) {
    return (int)__dosxc_syscall3(DOSXC_SYS_FULLPATH, (__dosxc_word)path, (__dosxc_word)buffer, (__dosxc_word)size);
}

#endif /* !__ASSEMBLER__ */
#endif
