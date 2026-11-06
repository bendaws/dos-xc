/*
 * BenDOS /drivers/storage.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once
#include "fat32.h"
struct fat32_volume *storage_system_volume(void);

int storage_disk_read(void *,unsigned int,void *);
int storage_disk_write(void *,unsigned int,const void *);
