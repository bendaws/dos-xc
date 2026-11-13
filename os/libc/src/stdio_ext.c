/*
 * DOS-XC /os/libc/src/stdio_ext.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "stdio_impl.h"
#include <stdio_ext.h>

static unsigned char *window(FILE *f) {
    return (f->flags & F_UNBUFFERED) || !f->capacity ? f->small : f->buffer;
}

size_t __fbufsize(FILE *f) { return f->capacity; }
size_t __fpending(FILE *f) { return f->mode == MODE_WRITE ? f->used : 0; }
int __flbf(FILE *f) { return (f->flags & F_LINE) != 0; }
int __freadable(FILE *f) { return (f->flags & F_READ) != 0; }
int __fwritable(FILE *f) { return (f->flags & F_WRITE) != 0; }
int __freading(FILE *f) { return f->mode == MODE_READ || !(f->flags & F_WRITE); }
int __fwriting(FILE *f) { return f->mode == MODE_WRITE || !(f->flags & F_READ); }
int __fsetlocking(FILE *f, int type) { (void)f; (void)type; return FSETLOCKING_INTERNAL; }
void _flushlbf(void) { fflush(0); }
void __fseterr(FILE *f) { f->flags |= F_ERROR; }

int __fpurge(FILE *f) {
    STREAM_GUARD(f);
    f->used = 0;
    f->position = f->length = 0;
    f->mode = MODE_NONE;
    return 0;
}
int fpurge(FILE *f) { return __fpurge(f); }

size_t __freadahead(FILE *f) { return f->mode == MODE_READ ? f->length - f->position : 0; }

const char *__freadptr(FILE *f, size_t *size) {
    if (f->mode != MODE_READ || f->position >= f->length) return 0;
    *size = f->length - f->position;
    return (const char *)window(f) + f->position;
}

void __freadptrinc(FILE *f, size_t increment) { f->position += increment; }
