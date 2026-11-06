/*
 * BenDOS /setup/install.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once
#include "../drivers/fat32.h"
typedef void (*install_progress_fn)(void *,const char *,unsigned);
/* Target callbacks address the whole disk. Source callbacks address its mounted volume. */
int install_system(const struct fat32_volume *source,void *target,uint32_t sectors,
                   fat32_read_sector_fn read,fat32_write_sector_fn write,
                   install_progress_fn progress,void *progress_context);
