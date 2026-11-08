/*
 * DOS-XC C library - internal declarations
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#pragma once
#include <dosxc/abi.h>
#include <dosxc/syscall.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

/* System calls return -errno on failure. */
static inline long __result(long value) {
    if (value < 0) {
        errno = (int)-value;
        return -1;
    }
    return value;
}
#define hidden __attribute__((__visibility__("hidden")))

extern char **environ;
extern const char *__progname;
hidden void __stdio_exit(void);
hidden void __run_exit_handlers(void);
hidden int __fd_stat(int fd, void *dosxc_stat);
hidden void __stat_convert(const struct dosxc_stat *in, void *out);
hidden int __record_child(int status);
hidden extern int __console_raw;
#include <sys/types.h>
hidden ssize_t __console_input(char *buffer, ssize_t n);
