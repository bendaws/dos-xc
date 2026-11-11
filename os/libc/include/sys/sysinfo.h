/*
 * DOS-XC /os/libc/include/sys/sysinfo.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#ifndef _SYS_SYSINFO_H
#define _SYS_SYSINFO_H
struct sysinfo {
    long uptime;
    unsigned long loads[3], totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    unsigned short procs, pad;
    unsigned long totalhigh, freehigh;
    unsigned mem_unit;
    char _f[0];
};
#ifdef __cplusplus
extern "C" {
#endif
int sysinfo(struct sysinfo *);
int get_nprocs(void);
int get_nprocs_conf(void);
long get_phys_pages(void);
long get_avphys_pages(void);
#ifdef __cplusplus
}
#endif

#endif
