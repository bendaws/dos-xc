/*
 * DOS-XC C library - sys/types.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H
#define __NEED_size_t
#define __NEED_ssize_t
#define __NEED_off_t
#define __NEED_pid_t
#define __NEED_mode_t
#define __NEED_uid_t
#define __NEED_dev_t
#define __NEED_time_t
#define __NEED_clock_t
#define __NEED_suseconds_t
#define __NEED_clockid_t
#define __NEED_intptr_t
#define __NEED_struct_timespec
#include <sys/_types.h>
#include <sys/select.h>
#include <endian.h>
typedef unsigned int id_t;
typedef int key_t;
typedef char *caddr_t;
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef unsigned short ushort;
typedef unsigned int uint;
typedef unsigned long ulong;
typedef __INT8_TYPE__ int8_t;
typedef __INT16_TYPE__ int16_t;
typedef __INT32_TYPE__ int32_t;
typedef __INT64_TYPE__ int64_t;
typedef __UINT8_TYPE__ u_int8_t;
typedef __UINT16_TYPE__ u_int16_t;
typedef __UINT32_TYPE__ u_int32_t;
typedef __UINT64_TYPE__ u_int64_t;
typedef long long quad_t;
typedef unsigned long long u_quad_t;
#endif
