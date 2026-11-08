/*
 * DOS-XC C library - endian.h (x86 is little-endian)
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#ifndef _ENDIAN_H
#define _ENDIAN_H
#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN 4321
#define __PDP_ENDIAN 3412
#define __BYTE_ORDER __LITTLE_ENDIAN
#define LITTLE_ENDIAN __LITTLE_ENDIAN
#define BIG_ENDIAN __BIG_ENDIAN
#define PDP_ENDIAN __PDP_ENDIAN
#define BYTE_ORDER __BYTE_ORDER

#define htobe16(x) __builtin_bswap16(x)
#define htole16(x) ((__UINT16_TYPE__)(x))
#define be16toh(x) __builtin_bswap16(x)
#define le16toh(x) ((__UINT16_TYPE__)(x))
#define htobe32(x) __builtin_bswap32(x)
#define htole32(x) ((__UINT32_TYPE__)(x))
#define be32toh(x) __builtin_bswap32(x)
#define le32toh(x) ((__UINT32_TYPE__)(x))
#define htobe64(x) __builtin_bswap64(x)
#define htole64(x) ((__UINT64_TYPE__)(x))
#define be64toh(x) __builtin_bswap64(x)
#define le64toh(x) ((__UINT64_TYPE__)(x))
#endif
