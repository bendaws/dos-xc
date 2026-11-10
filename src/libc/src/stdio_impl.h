/*
 * DOS-XC C library - stream internals
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#pragma once
#include "libc.h"
#include <stdio.h>
#include <sys/types.h>

#define F_READ 0x1
#define F_WRITE 0x2
#define F_APPEND 0x4
#define F_EOF 0x8
#define F_ERROR 0x10
#define F_LINE 0x20
#define F_UNBUFFERED 0x40
#define F_STATIC 0x80
#define F_OWN_BUFFER 0x100
#define F_PROBE 0x200          /* decide line buffering on first use */
#define F_MEMORY 0x400
#define F_OWN_MEMORY 0x800
#define PATH_MAX_STDIO 260
enum { MODE_NONE, MODE_READ, MODE_WRITE };

struct __dosxc_file {
    int fd, flags, mode;
    unsigned char *buffer;
    size_t capacity;
    size_t position, length;    /* read buffer window */
    size_t used;                /* pending output */
    unsigned char small[8];     /* unbuffered streams and ungetc */
    char *memory;               /* fmemopen / open_memstream */
    size_t memory_capacity, memory_length, memory_position;
    char **memory_out;
    size_t *memory_size_out;
    struct __dosxc_file *next;
    struct __dosxc_rlock lock;
};
#define STREAM_GUARD(f) LOCK_GUARD(&(f)->lock)
