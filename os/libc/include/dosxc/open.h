/*
 * DOS-XC /os/libc/include/dosxc/open.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _DOSXC_OPEN_H
#define _DOSXC_OPEN_H
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
#endif
