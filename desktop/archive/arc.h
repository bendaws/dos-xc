/*
 * DOS-XC /desktop/archive/arc.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once
#include "archive.h"
#include <stdint.h>
#include <stdio.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#ifdef __dosxc__
#define ARC_SEP '\\'
#else
#define ARC_SEP '/'
#endif

struct job {
    const struct arc_progress *progress;
    char *error;
    size_t error_size;
    u64 done, total;
    int cancelled;
    unsigned ticks;
    const char *folder;               /* extracting into */
    FILE *out;                        /* the file being extracted, or 0 */
    char name[300];                   /* its name in the archive */
    u32 crc;
    u64 written;
    long long mtime;
    char out_path[700];
};

int job_fail(struct job *job, const char *format, ...);
int job_tick(struct job *job);        /* -1 when cancelled */
void job_status(struct job *job, const char *name);

/* A buffered reader of LIMIT bytes of a file from OFFSET. */
struct src {
    FILE *f;
    struct job *job;
    u8 buf[65536];
    size_t pos, len;
    u64 left;
    int error, ended;
};
void src_open(struct src *s, FILE *f, u64 offset, u64 limit, struct job *job);
int src_fill(struct src *s);          /* 0 at the end */
static inline int src_byte(struct src *s) {
    if (s->pos == s->len && !src_fill(s)) return -1;
    return s->buf[s->pos++];
}
size_t src_read(struct src *s, void *out, size_t n);
int src_skip(struct src *s, u64 n);

/* Where decoded bytes go. */
struct sink { int (*write)(struct sink *, const u8 *, size_t); };

/* Extraction: one entry at a time.  NAME uses '/' or '\' separators. */
int out_begin(struct job *job, const char *name, int directory, long long mtime);
int out_write(struct job *job, const void *data, size_t n);
int out_end(struct job *job);          /* closes the file; 0 or -1 */
void out_abort(struct job *job);
int out_copy(struct job *job, const char *name, const char *target, long long mtime);   /* a hard link: copy TARGET */
struct sink *out_sink(struct job *job);

/* Creation: everything the sources hold, parents before children. */
struct arc_item {
    char *name;                        /* in the archive, '/'-separated */
    char *path;                        /* on disk */
    int directory;
    u64 size;
    long long mtime;
};
/* Reads ITEM's SIZE bytes through FN, with progress; CRC (if not 0) gets their CRC-32. */
int arc_feed(struct job *job, const struct arc_item *item, int (*fn)(void *, const u8 *, size_t), void *context, u32 *crc);
int arc_walk(struct job *job, const char *const *sources, int count, struct arc_item **items, int *n);
void arc_walk_free(struct arc_item *items, int n);

struct fsink { struct sink sink; FILE *f; u64 count; int error; };
void fsink_init(struct fsink *s, FILE *f);

u32 crc32_update(u32 crc, const void *data, size_t n);
u64 crc64_update(u64 crc, const void *data, size_t n);
void dos_time(long long t, u16 *date, u16 *time);
long long from_dos_time(u16 date, u16 time);

static inline u16 get16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static inline u32 get32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static inline u64 get64(const u8 *p) { return get32(p) | (u64)get32(p + 4) << 32; }
static inline void put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static inline void put32(u8 *p, u32 v) { put16(p, v); put16(p + 2, v >> 16); }
static inline void put64(u8 *p, u64 v) { put32(p, (u32)v); put32(p + 4, (u32)(v >> 32)); }

int arc_seek(FILE *f, u64 offset);
u64 arc_tell(FILE *f);
u64 arc_file_size(FILE *f);

/* Codecs. */
/* REST gets the bytes read past the end (at most 8), when REST_COUNT is not 0. */
int inflate_stream(struct src *in, struct sink *out, struct job *job, u8 *rest, int *rest_count);
struct deflater;
struct deflater *deflate_new(struct sink *out);
int deflate_write(struct deflater *d, const u8 *data, size_t n);
int deflate_finish(struct deflater *d);          /* frees D */
void deflate_free(struct deflater *d);

int lzma_decode(struct src *in, const u8 props[5], u64 size, struct sink *out, struct job *job);
int lzma2_decode(struct src *in, u8 prop, u64 size, struct sink *out, struct job *job);
struct lzma2_enc;
struct lzma2_enc *lzma2_enc_new(struct sink *out, u8 *dict_prop);
int lzma2_enc_write(struct lzma2_enc *e, const u8 *data, size_t n);
int lzma2_enc_finish(struct lzma2_enc *e);       /* writes the end marker; frees E */
void lzma2_enc_free(struct lzma2_enc *e);

int xz_decode(struct src *in, struct sink *out, struct job *job);
struct xz_enc;
struct xz_enc *xz_enc_new(struct sink *out);
int xz_enc_write(struct xz_enc *x, const u8 *data, size_t n);
int xz_enc_finish(struct xz_enc *x);
void xz_enc_free(struct xz_enc *x);

/* Formats. */
int zip_extract(struct job *job, FILE *f);
int zip_create(struct job *job, FILE *f, struct arc_item *items, int n);
int tar_extract(struct job *job, FILE *f, int format);
int tar_create(struct job *job, FILE *f, int format, struct arc_item *items, int n);
int gz_extract(struct job *job, FILE *f, const char *name, int format);
int sevenz_extract(struct job *job, FILE *f);
int sevenz_create(struct job *job, FILE *f, struct arc_item *items, int n);
int rar_extract(struct job *job, FILE *f);
