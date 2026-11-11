/*
 * DOS-XC /os/libc/include/linux/random.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _LINUX_RANDOM_H
#define _LINUX_RANDOM_H
#include <sys/random.h>
#ifndef GRND_NONBLOCK
#define GRND_NONBLOCK 1
#define GRND_RANDOM 2
#endif
#endif
