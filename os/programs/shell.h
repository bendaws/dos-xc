/*
 * DOS-XC /os/programs/shell.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once

#include "../drivers/fat32.h"

void shell_run(struct fat32_volume *volume, int native_dos_available, char drive,
               uint64_t memory_base, uint64_t memory_size);

#include "api.h"
void shell_firmware(disk_count_fn, disk_info_fn, disk_io_fn, void (__attribute__((sysv_abi)) *)(void),
                    unsigned (__attribute__((sysv_abi)) *)(void),
                    int (__attribute__((sysv_abi)) *)(int *, int *, int *, unsigned *),
                    void (__attribute__((sysv_abi)) *)(void), uint64_t,
                    int (__attribute__((sysv_abi)) *)(int, unsigned, unsigned *, unsigned *, unsigned *, uint64_t *),
                    unsigned system_pci, uint64_t system_lba, uint64_t ap_page);
