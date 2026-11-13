/*
 * DOS-XC /os/pkg/unxz.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

/* XZ Embedded (os/pkg/xz, 0BSD) built as one unit: .xz streams with LZMA2 and the x86 filter. */
#define XZ_DEC_X86
#include "xz/xz_crc32.c"
#include "xz/xz_dec_stream.c"
#include "xz/xz_dec_lzma2.c"
#include "xz/xz_dec_bcj.c"
