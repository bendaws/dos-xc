/*
 * BenDOS /programs/api.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once
#include "../drivers/fat32.h"
#define PROGRAM_BASE 0x4000000u
#define BATCH_BASE 0x4100000u
#define PROGRAM_LIMIT 0x20000u
#define PATH_LIMIT 260
#define KEY_UP 0x101
#define KEY_DOWN 0x102
#define KEY_RIGHT 0x103
#define KEY_LEFT 0x104
#define KEY_HOME 0x105
#define KEY_END 0x106
#define KEY_DELETE 0x108
#define KEY_F1 0x10b
#define KEY_F2 0x10c
#define KEY_F3 0x10d
struct environment_variable { char name[32], value[128]; };
struct command_context {
    unsigned abi;
    struct fat32_volume *volume;
    char drive;
    char cwd[PATH_LIMIT];
    int errorlevel, echo, batch_depth, exit_requested;
    unsigned speed;
    int bios_available;
    struct environment_variable environment[16];
    void (*write)(const char *);
    void (*putc)(char);
    int (*read_key)(void);
    void (*clear)(void);
    void (*color)(unsigned char);
    void (*cursor)(unsigned,unsigned);
    void (*beep)(unsigned,unsigned);
    int (*resolve)(const char *, char *);
    int (*execute)(const char *);
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
};
typedef int (*program_entry)(const char *,struct command_context *);
