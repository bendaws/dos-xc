/*
 * DOS-XC /os/libc/include/ar.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _AR_H
#define _AR_H
#define ARMAG "!<arch>\n"
#define SARMAG 8
#define ARFMAG "`\n"
/* A member's header in an "ar" archive (all fields are ASCII text). */
struct ar_hdr {
    char ar_name[16];
    char ar_date[12];
    char ar_uid[6], ar_gid[6];
    char ar_mode[8];
    char ar_size[10];
    char ar_fmag[2];
};
#endif
