/*
 * DOS-XC /os/pkg/dsp.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once
#include <stddef.h>
#include <stdio.h>

#define DSP_MAGIC "DSPK"
#define DSP_VERSION 2          /* 1: a ZIP archive; 2: a file list and one .xz stream */

struct dsp_info {
    char name[64], version[32], author[96], description[160], path[256], start_in[32];
    char env[512];                /* "NAME=value" lines separated by '\n' */
    char driver[256];             /* ';'-separated .DRV files, moved to \SYSTEM\DRIVERS */
    char provides[128];           /* ';'-separated roles it fills, such as "desktop" */
    char requires[256];           /* ';'-separated "name" or "name>=version" */
    char firewall[256];           /* ';'-separated programs the firewall lets send */
    int arch;
    int required;                 /* Required=1: updated, never removed */
};

struct dsp_entry {
    char name[260];               /* with '/' separators; directories end in '/' */
    unsigned method;              /* 0 stored, 8 deflated; version 2: 14 (in the .xz stream) */
    unsigned long crc, compressed, size, offset;   /* version 2: OFFSET counts in the unpacked stream */
};

struct dsp {
    FILE *file;
    struct dsp_info info;
    struct dsp_entry *entries;
    unsigned count, format;
    unsigned long stream_start, position;
    struct dsp_xz *xz;
};

/* Package metadata text <-> struct; returns 0 or -1 (missing name/version/arch). */
int dsp_parse_info(const char *text, size_t length, struct dsp_info *info);
int dsp_format_info(const struct dsp_info *info, char *out, size_t size);
int dsp_valid_name(const char *name);
int dsp_compare_versions(const char *a, const char *b);

/* The next ';'-separated item of LIST in OUT, trimmed; returns what follows it, or 0 at the end. */
const char *dsp_next_item(const char *list, char *out, size_t size);
/* Does INFO meet REQUIREMENT ("name" or "name>=version")?  A Provides= role
   meets only requirements without a version. */
int dsp_meets(const struct dsp_info *info, const char *requirement);

/* Reading.  Errors leave a message in ERROR.  Version 2 packages read
   fastest in order: reading an earlier entry unpacks from the start again. */
int dsp_open(struct dsp *package, const char *path, char *error, size_t error_size);
int dsp_read_info(const char *path, struct dsp_info *info, char *error, size_t error_size);
int dsp_read_entry(struct dsp *package, unsigned index, unsigned char **data, size_t *size);
void dsp_close(struct dsp *package);

/* Writing version 2: FILES list the data in XZ (an .xz stream of the files, in order). */
struct dsp_file { char name[260]; unsigned long size, crc; };
int dsp_write_solid(const char *path, const struct dsp_info *info, const struct dsp_file *files, unsigned count, FILE *xz);

/* Writing version 1 (deflated), when no .xz encoder is at hand. */
struct dsp_writer;
struct dsp_writer *dsp_create(const char *path, const struct dsp_info *info);
int dsp_add(struct dsp_writer *writer, const char *name, const void *data, size_t size);
int dsp_finish(struct dsp_writer *writer);

/* Raw DEFLATE (RFC 1951) and CRC-32. */
int dsp_inflate(const unsigned char *in, size_t in_size, unsigned char *out, size_t out_size);
size_t dsp_deflate(const unsigned char *in, size_t in_size, unsigned char *out, size_t out_size);
unsigned long dsp_crc32(unsigned long crc, const void *data, size_t size);
