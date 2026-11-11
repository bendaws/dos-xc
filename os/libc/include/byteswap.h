/*
 * DOS-XC /os/libc/include/byteswap.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _BYTESWAP_H
#define _BYTESWAP_H
#define bswap_16(x) __builtin_bswap16(x)
#define bswap_32(x) __builtin_bswap32(x)
#define bswap_64(x) __builtin_bswap64(x)
#endif
