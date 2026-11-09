/*
 * DOS-XC /setup/install.h - what both Setup programs share
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * The text program (SETUP.EXE) and the desktop one (SETUPGUI.EXE) offer the
 * same choices: this finds the disks and packages and does the installing.
 */
#pragma once
#include <dosxc/syscall.h>
#include <stddef.h>
#include "../pkg/dsp.h"

struct disk_choice { unsigned index; unsigned long long sectors; };
struct package_choice { char file[300]; struct dsp_info info; int selected, required; };

extern struct dosxc_sysinfo setup_system;
extern char setup_root[4];
extern char setup_failure[160];
extern struct disk_choice setup_disks[16];
extern int setup_disk_count;
extern struct package_choice setup_packages[64];
extern int setup_package_count;

/* Called for each file with its name; percent is overall progress. */
typedef void (*setup_progress)(const char *name, unsigned percent, unsigned long done, unsigned long total);

void setup_init(void);                 /* sysinfo, source drive */
void setup_find_disks(void);
void setup_find_packages(void);        /* from \Packages on the media */
void setup_size_text(char *out, size_t size, unsigned long long sectors);
/* Erase DISK and install everything selected.  0, or -1 with setup_failure set. */
int setup_install(const struct disk_choice *disk, setup_progress progress, void (*status)(const char *text));
