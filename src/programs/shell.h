/*
 * DOS-XC /programs/shell.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once

#include "../drivers/fat32.h"

void shell_run(struct fat32_volume *volume, int native_dos_available, char drive,
               uint64_t memory_base, uint64_t memory_size);
