/*
 * DOS-XC /os/pkg/dsp.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once
#include <stddef.h>
#include <stdio.h>

#define DSP_MAGIC "DSPK"
#define DSP_VERSION 1

struct dsp_info {
    char name[64], version[32], author[96], description[160], path[256], start_in[32];
    char env[512];                /* "NAME=value" lines separated by '\n' */
    char driver[256];             /* ';'-separated .DRV files, moved to \SYSTEM\DRIVERS */
    int arch;
};

struct dsp_entry {
    char name[260];               /* with '/' separators; directories end in '/' */
    unsigned method;              /* 0 stored, 8 deflated */
    unsigned long crc, compressed, size, offset;
};

struct dsp {
    FILE *file;
    struct dsp_info info;
    struct dsp_entry *entries;
    unsigned count;
};

/* Package metadata text <-> struct; returns 0 or -1 (missing name/version/arch). */
int dsp_parse_info(const char *text, size_t length, struct dsp_info *info);
int dsp_format_info(const struct dsp_info *info, char *out, size_t size);
int dsp_valid_name(const char *name);
int dsp_compare_versions(const char *a, const char *b);

/* Reading.  Errors leave a message in ERROR. */
int dsp_open(struct dsp *package, const char *path, char *error, size_t error_size);
int dsp_read_info(const char *path, struct dsp_info *info, char *error, size_t error_size);
int dsp_read_entry(struct dsp *package, unsigned index, unsigned char **data, size_t *size);
void dsp_close(struct dsp *package);

/* Writing. */
struct dsp_writer;
struct dsp_writer *dsp_create(const char *path, const struct dsp_info *info);
int dsp_add(struct dsp_writer *writer, const char *name, const void *data, size_t size);
int dsp_finish(struct dsp_writer *writer);

/* Raw DEFLATE (RFC 1951) and CRC-32. */
int dsp_inflate(const unsigned char *in, size_t in_size, unsigned char *out, size_t out_size);
size_t dsp_deflate(const unsigned char *in, size_t in_size, unsigned char *out, size_t out_size);
unsigned long dsp_crc32(unsigned long crc, const void *data, size_t size);
