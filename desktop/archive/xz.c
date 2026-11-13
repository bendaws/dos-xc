/*
 * DOS-XC /desktop/archive/xz.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

/* Reading: XZ Embedded (os/pkg/xz, 0BSD) as one unit.  Writing: one block of LZMA2. */
#pragma GCC diagnostic ignored "-Wunused-function"
#define XZ_EXTERN static
#define XZ_USE_CRC64
#define XZ_DEC_ANY_CHECK
#define XZ_DEC_CONCATENATED
#define XZ_DEC_X86
#define XZ_DEC_ARM
#define XZ_DEC_ARM64
#include "../../os/pkg/xz/xz_crc32.c"
#include "../../os/pkg/xz/xz_dec_stream.c"
#include "../../os/pkg/xz/xz_dec_lzma2.c"
#include "../../os/pkg/xz/xz_dec_bcj.c"

#include "arc.h"

static void xz_crc64_init(void) {}
static uint64_t xz_crc64(const uint8_t *buf, size_t size, uint64_t crc) { return crc64_update(crc, buf, size); }

int xz_decode(struct src *in, struct sink *out, struct job *job) {
    xz_crc32_init();
    xz_crc64_init();
    struct xz_dec *x = xz_dec_init(XZ_DYNALLOC, 1u << 28);
    u8 *buffer = malloc(65536);
    if (!x || !buffer) { if (x) xz_dec_end(x); free(buffer); return job_fail(job, "Out of memory."); }
    struct xz_buf b = {in->buf + in->pos, 0, in->len - in->pos, buffer, 0, 65536};
    int finished = 0, r = -1;
    for (;;) {
        if (b.in_pos == b.in_size && !finished) {
            in->pos = in->len;
            finished = !src_fill(in);
            b.in = in->buf;
            b.in_pos = 0;
            b.in_size = finished ? 0 : in->len;
            if (in->error) break;
        }
        enum xz_ret ret = xz_dec_catrun(x, &b, finished);
        if (b.out_pos) {
            if (out->write(out, buffer, b.out_pos)) break;
            b.out_pos = 0;
        }
        if (ret == XZ_STREAM_END) { r = 0; break; }
        if (ret == XZ_UNSUPPORTED_CHECK) continue;
        if (ret != XZ_OK) {
            if (ret == XZ_MEMLIMIT_ERROR || ret == XZ_MEM_ERROR) job_fail(job, "Unpacking it needs more memory than there is.");
            else if (ret == XZ_OPTIONS_ERROR) job_fail(job, "It uses .xz options DOS-XC does not support.");
            break;
        }
    }
    in->pos = (size_t)(b.in - in->buf) + b.in_pos;
    xz_dec_end(x);
    free(buffer);
    if (r && !job->cancelled) job_fail(job, "The .xz data is damaged.");
    return r;
}

struct xz_enc {
    struct sink counter, *out;
    struct lzma2_enc *lz;
    u64 packed, unpacked;
    u32 crc;
    u8 prop;
    int error;
};

static int count_write(struct sink *s, const u8 *data, size_t n) {
    struct xz_enc *x = (struct xz_enc *)s;
    x->packed += n;
    return x->out->write(x->out, data, n);
}

static int vli(u8 *p, u64 v) {
    int n = 0;
    while (v >= 0x80) { p[n++] = (u8)(v | 0x80); v >>= 7; }
    p[n++] = (u8)v;
    return n;
}

struct xz_enc *xz_enc_new(struct sink *out) {
    struct xz_enc *x = calloc(1, sizeof(*x));
    if (!x) return 0;
    x->counter.write = count_write;
    x->out = out;
    x->lz = lzma2_enc_new(&x->counter, &x->prop);
    if (!x->lz) { free(x); return 0; }
    u8 head[12 + 12] = {0xfd, '7', 'z', 'X', 'Z', 0, 0, 1};
    put32(head + 8, crc32_update(0, head + 6, 2));
    u8 *block = head + 12;
    block[0] = 12 / 4 - 1;
    block[1] = 0;
    block[2] = 0x21;
    block[3] = 1;
    block[4] = x->prop;
    put32(block + 8, crc32_update(0, block, 8));
    if (out->write(out, head, sizeof(head))) { xz_enc_free(x); return 0; }
    return x;
}

int xz_enc_write(struct xz_enc *x, const u8 *data, size_t n) {
    x->crc = crc32_update(x->crc, data, n);
    x->unpacked += n;
    return lzma2_enc_write(x->lz, data, n);
}

int xz_enc_finish(struct xz_enc *x) {
    int r = lzma2_enc_finish(x->lz);
    x->lz = 0;
    u8 tail[64];
    int n = 0;
    while ((x->packed + (u64)n) % 4) tail[n++] = 0;
    put32(tail + n, x->crc);
    n += 4;
    u8 *index = tail + n;
    int k = 0;
    index[k++] = 0;
    index[k++] = 1;
    k += vli(index + k, 12 + x->packed + 4);
    k += vli(index + k, x->unpacked);
    while (k % 4) index[k++] = 0;
    put32(index + k, crc32_update(0, index, (size_t)k));
    k += 4;
    u8 *footer = index + k;
    put32(footer + 4, (u32)(k / 4 - 1));
    footer[8] = 0;
    footer[9] = 1;
    put32(footer, crc32_update(0, footer + 4, 6));
    footer[10] = 'Y';
    footer[11] = 'Z';
    n += k + 12;
    if (!r && x->out->write(x->out, tail, (size_t)n)) r = -1;
    free(x);
    return r;
}

void xz_enc_free(struct xz_enc *x) {
    if (!x) return;
    lzma2_enc_free(x->lz);
    free(x);
}
