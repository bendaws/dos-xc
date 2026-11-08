/*
 * DOS-XC /setup/target.h - the disk Setup installs to
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */
#pragma once
/* Partition DISK (a disk() index with SECTORS sectors) as one FAT32 volume
   and mount it.  BOOT is \SYSTEM\FAT32.BIN, MBR is \SYSTEM\MBR.BIN. */
int target_format(unsigned disk, unsigned long sectors, const unsigned char *boot, const unsigned char *mbr, int bits);
int target_mkdir(const char *path);                      /* "\DIR\SUB"; existing is fine */
int target_write(const char *path, const void *data, unsigned long size);
/* Point the boot record at the loader and write the MBR. */
int target_finish(void);
