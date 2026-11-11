/*
 * DOS-XC /os/libc/include/sys/un.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _SYS_UN_H
#define _SYS_UN_H
#include <sys/socket.h>
struct sockaddr_un { sa_family_t sun_family; char sun_path[108]; };
#define SUN_LEN(p) (__builtin_offsetof(struct sockaddr_un, sun_path) + __builtin_strlen((p)->sun_path))
#endif
