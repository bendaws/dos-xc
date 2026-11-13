/*
 * DOS-XC /desktop/embed.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once
#include "wm.h"
#include <pthread.h>

/* A native child window of a Qt program (its winId()): a rectangle of the
   top-level window that another thread may draw into, such as VLC's video.
   Hold *lock while touching top's pixels; add 1 to top->seq after drawing. */
struct embed_window {
    pthread_mutex_t *lock;
    struct window *top;              /* 0 while the top-level is not open */
    int x, y, w, h;                  /* in top's content */
    int visible;
};
