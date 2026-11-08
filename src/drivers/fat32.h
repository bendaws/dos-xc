/*
 * DOS-XC /drivers/fat32.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;

typedef int (__attribute__((sysv_abi)) *fat32_read_sector_fn)(void *context, uint32_t lba, void *buffer);
typedef int (__attribute__((sysv_abi)) *fat32_write_sector_fn)(void *context, uint32_t lba, const void *buffer);

enum fat_type {
    FAT_TYPE_12 = 12,
    FAT_TYPE_16 = 16,
    FAT_TYPE_32 = 32
};

struct fat32_volume {
    void *context;
    fat32_read_sector_fn read_sector;
    fat32_write_sector_fn write_sector;
    uint32_t partition_lba;
    uint32_t total_sectors;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t fat_lba;
    uint32_t sectors_per_fat;
    uint32_t fat_count;
    uint32_t data_lba;
    uint32_t root_cluster;
    uint32_t root_lba;
    uint32_t root_sectors;
    uint32_t cluster_count;
    enum fat_type type;
    uint32_t free_clusters, next_free, fsinfo_sector;
};

int fat32_mount(struct fat32_volume *volume, uint32_t partition_lba);
int fat32_read(const struct fat32_volume *volume, const char *path, void *buffer, uint32_t size);
int fat32_write(const struct fat32_volume *volume, const char *path, const void *buffer, uint32_t size);
int fat32_mkdir(const struct fat32_volume *volume, const char *path);
int fat32_delete(const struct fat32_volume *volume, const char *path);

struct fat32_info {
    char name[260];
    uint32_t size;
    uint16_t date, time;
    uint8_t attributes;
};
typedef int (*fat32_list_fn)(void *context, const struct fat32_info *info);
int fat32_stat(const struct fat32_volume *, const char *, struct fat32_info *);
int fat32_list(const struct fat32_volume *, const char *, fat32_list_fn, void *);
int fat32_rename(const struct fat32_volume *, const char *, const char *);
int fat32_rmdir(const struct fat32_volume *, const char *);
uint64_t fat32_free_bytes(const struct fat32_volume *);
int fat32_copy(const struct fat32_volume *, const char *, const char *);

int fat32_read_at(const struct fat32_volume *, const char *, uint32_t, void *, uint32_t);
int fat32_copy_to(const struct fat32_volume *, const char *, const struct fat32_volume *, const char *);
int fat32_file_lba(const struct fat32_volume *,const char *,uint32_t *,uint32_t *);
