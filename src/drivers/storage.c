/*
 * BenDOS /drivers/storage.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "storage.h"
extern void bios_transfer(void);
static struct fat32_volume system_volume;
static int transfer(unsigned char drive,unsigned int lba, void *buffer, int write) {
    volatile unsigned char *rpc=(volatile unsigned char *)0x7040;
    unsigned char *bounce=(unsigned char *)0x78000;
    unsigned char *bytes=buffer;
    if(write)for(unsigned i=0;i<512;++i)bounce[i]=bytes[i];
    rpc[1]=write?0x43:0x42;
    rpc[2]=drive;
    *(volatile unsigned int *)(rpc+4)=lba;
    bios_transfer();
    if(*(volatile int *)(rpc+8))return -1;
    if(!write){unsigned offset=*(volatile unsigned int *)0x7010==0x46564443&&drive==*(volatile unsigned char *)0x7014?((lba&3)*512):0;
        for(unsigned i=0;i<512;++i)bytes[i]=bounce[offset+i];}
    return 0;
}
static int read_sector(void *context,unsigned int lba,void *buffer) { (void)context; return transfer(*(volatile unsigned char *)0x7014,lba,buffer,0); }
static int write_sector(void *context,unsigned int lba,const void *buffer) { (void)context; return transfer(*(volatile unsigned char *)0x7014,lba,(void *)buffer,1); }
struct fat32_volume *storage_system_volume(void) {
    if(*(volatile unsigned int *)0x7000!=0x53464442)return 0;
    system_volume.read_sector=read_sector;system_volume.write_sector=*(volatile unsigned int *)0x7010==0x46564443?0:write_sector;
    if(fat32_mount(&system_volume,2048))return 0;
    return &system_volume;
}

int storage_disk_read(void *disk,unsigned int lba,void *buffer){return transfer((unsigned char)(__UINTPTR_TYPE__)disk,lba,buffer,0);}
int storage_disk_write(void *disk,unsigned int lba,const void *buffer){return transfer((unsigned char)(__UINTPTR_TYPE__)disk,lba,(void *)buffer,1);}
