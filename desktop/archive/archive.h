/*
 * DOS-XC /desktop/archive/archive.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once
#include <stddef.h>

enum {
    ARC_UNKNOWN, ARC_ZIP, ARC_7Z, ARC_RAR, ARC_TAR, ARC_TAR_GZ, ARC_TAR_XZ, ARC_GZ, ARC_XZ
};

/* Called now and then with the bytes done of TOTAL; nonzero cancels. */
struct arc_progress {
    int (*report)(void *context, const char *name, unsigned long long done, unsigned long long total);
    void *context;
};

int arc_detect(const char *path);                /* ARC_*, from the contents and then the name */
const char *arc_extension(int format);           /* ".zip", ".tar.gz", ... */
int arc_can_create(int format);
/* The folder name an archive extracts into: "photos" for "photos.tar.gz". */
void arc_stem(const char *path, char *out, size_t size);

/* 0, or -1 with a message in ERROR. */
int arc_extract(const char *archive, const char *folder, const struct arc_progress *progress, char *error, size_t error_size);
int arc_create(const char *archive, int format, const char *const *sources, int count,
               const struct arc_progress *progress, char *error, size_t error_size);
