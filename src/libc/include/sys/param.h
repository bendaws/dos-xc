/*
 * DOS-XC C library - sys/param.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#ifndef _SYS_PARAM_H
#define _SYS_PARAM_H
#include <limits.h>
#include <endian.h>
#define MAXPATHLEN PATH_MAX
#define MAXSYMLINKS 20
#define MAXHOSTNAMELEN 64
#define NOFILE OPEN_MAX
#define NBBY 8
#define HZ 100
#define DEV_BSIZE 512
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#define powerof2(x) ((((x) - 1) & (x)) == 0)
#endif
