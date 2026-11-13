/*
 * DOS-XC /vlc/player.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "wm.h"
#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <vlc/vlc.h>

#define BAR_H 34
#define RATE 48000
#define CHANNELS 2

static libvlc_instance_t *vlc;
static libvlc_media_player_t *player;
static struct window *main_w;
static char title[300] = "VLC media player";

/* The latest decoded picture, written by VLC's video thread. */
static struct {
    pthread_mutex_t lock;
    u32 *pixels;
    unsigned width, height;
    int fresh;
} frame = {PTHREAD_MUTEX_INITIALIZER, 0, 0, 0, 0};

static int volume = 80;            /* percent */
static int audio_open;
static float shown_position;
static long long shown_time, shown_length;
static int shown_playing;

static unsigned video_setup(void **opaque, char *chroma, unsigned *width, unsigned *height, unsigned *pitches, unsigned *lines) {
    (void)opaque;
    memcpy(chroma, "RV32", 4);
    pthread_mutex_lock(&frame.lock);
    free(frame.pixels);
    frame.width = *width;
    frame.height = *height;
    frame.pixels = calloc((size_t)*width * *height, 4);
    pthread_mutex_unlock(&frame.lock);
    if (!frame.pixels) return 0;
    *pitches = *width * 4;
    *lines = *height;
    return 1;
}

static void video_cleanup(void *opaque) {
    (void)opaque;
    pthread_mutex_lock(&frame.lock);
    free(frame.pixels);
    frame.pixels = 0;
    frame.width = frame.height = 0;
    frame.fresh = 1;
    pthread_mutex_unlock(&frame.lock);
}

static void *video_lock(void *opaque, void **planes) {
    (void)opaque;
    pthread_mutex_lock(&frame.lock);
    planes[0] = frame.pixels;
    return 0;
}

static void video_unlock(void *opaque, void *picture, void *const *planes) {
    (void)opaque; (void)picture; (void)planes;
    frame.fresh = 1;
    pthread_mutex_unlock(&frame.lock);
}

static void video_display(void *opaque, void *picture) { (void)opaque; (void)picture; }

/* VLC's audio thread: hand the samples to the sound device, waiting while its buffer is full. */
static void audio_play(void *opaque, const void *samples, unsigned count, int64_t pts) {
    (void)opaque; (void)pts;
    if (!audio_open) {
        if (dosxc_sys_audio(DOSXC_AUDIO_OPEN, RATE, CHANNELS) < 0) return;
        audio_open = 1;
    }
    int16_t scaled[4096 * CHANNELS];
    const int16_t *in = samples;
    while (count) {
        unsigned n = count < 4096 ? count : 4096;
        for (unsigned i = 0; i < n * CHANNELS; ++i) scaled[i] = (int16_t)(in[i] * volume / 100);
        unsigned done = 0;
        for (int waited = 0; done < n && waited < 2000; ++waited) {
            long taken = dosxc_sys_audio(DOSXC_AUDIO_WRITE, (long)(scaled + done * CHANNELS), n - done);
            if (taken < 0) return;
            done += (unsigned)taken;
            if (done < n) usleep(2000);
        }
        in += n * CHANNELS;
        count -= n;
    }
}

static void audio_close(void) {
    if (audio_open) dosxc_sys_audio(DOSXC_AUDIO_CLOSE, 0, 0);
    audio_open = 0;
}
static void audio_pause(void *opaque, int64_t pts) { (void)opaque; (void)pts; }
static void audio_resume(void *opaque, int64_t pts) { (void)opaque; (void)pts; }
static void audio_flush(void *opaque, int64_t pts) { (void)opaque; (void)pts; audio_close(); }
static void audio_drain(void *opaque) { (void)opaque; }

/* A file:// URI VLC understands for a DOS-XC path; URLs pass through. */
static void location_of(const char *path, char *out, size_t size) {
    if (strstr(path, "://")) { snprintf(out, size, "%s", path); return; }
    char full[1024];
    if (!realpath(path, full)) snprintf(full, sizeof(full), "%s", path);
    size_t n = (size_t)snprintf(out, size, "file:///");
    for (const unsigned char *p = (const unsigned char *)full; *p && n + 4 < size; ++p) {
        unsigned char c = *p == '\\' ? '/' : *p;
        if (isalnum(c) || strchr("/:-_.~", c)) out[n++] = (char)c;
        else n += (size_t)snprintf(out + n, size - n, "%%%02X", c);
    }
    out[n] = 0;
}

static void play_file(const char *path) {
    char location[2048];
    location_of(path, location, sizeof(location));
    libvlc_media_t *media = libvlc_media_new_location(vlc, location);
    if (!media) { message_box("VLC media player", "This file cannot be opened:", path); return; }
    libvlc_media_player_set_media(player, media);
    libvlc_media_release(media);
    libvlc_media_player_play(player);
    const char *name = strrchr(path, '/'), *back = strrchr(path, '\\');
    if (back > name) name = back;
    snprintf(title, sizeof(title), "%s - VLC media player", name ? name + 1 : path);
    window_title(main_w, title);
}

static void opened(const char *path, void *data) { (void)data; if (path) play_file(path); }

static struct rect video_rect(struct window *w) { return (struct rect){0, 0, w->r.w, w->r.h - BAR_H}; }
static struct rect button_rect(int i) { return (struct rect){6 + i * 62, 0, 58, 24}; }
static struct rect seek_rect(struct window *w) { return (struct rect){6 + 3 * 62 + 6, 0, w->r.w - 3 * 62 - 6 - 6 - 230, 24}; }
static struct rect volume_rect(struct window *w) { return (struct rect){w->r.w - 96, 0, 90, 24}; }

static void format_time(char *out, size_t size, long long ms) {
    if (ms < 0) ms = 0;
    long long s = ms / 1000;
    if (s >= 3600) snprintf(out, size, "%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
    else snprintf(out, size, "%lld:%02lld", s / 60, s % 60);
}

static void paint_video(struct window *w, const struct canvas *c) {
    struct rect area = video_rect(w);
    fill(c, area.x, area.y, area.w, area.h, 0x000000);
    pthread_mutex_lock(&frame.lock);
    if (frame.pixels && frame.width && frame.height && area.w > 0 && area.h > 0) {
        int dw = area.w, dh = (int)((long long)area.w * frame.height / frame.width);
        if (dh > area.h) { dh = area.h; dw = (int)((long long)area.h * frame.width / frame.height); }
        int ox = area.x + (area.w - dw) / 2, oy = area.y + (area.h - dh) / 2;
        struct surface *s = c->s;
        for (int y = 0; y < dh; ++y) {
            int ty = oy + y + c->oy;
            if (ty < c->clip.y + c->oy || ty >= c->clip.y + c->clip.h + c->oy || ty >= s->h) continue;
            const u32 *row = frame.pixels + (size_t)((long long)y * frame.height / dh) * frame.width;
            u32 *out = s->px + (size_t)ty * s->w;
            for (int x = 0; x < dw; ++x) {
                int tx = ox + x + c->ox;
                if (tx >= 0 && tx < s->w) out[tx] = row[(long long)x * frame.width / dw] & 0xffffff;
            }
        }
    }
    frame.fresh = 0;
    pthread_mutex_unlock(&frame.lock);
}

static void player_paint(struct window *w, const struct canvas *c) {
    paint_video(w, c);
    int y = w->r.h - BAR_H + 5;
    struct canvas bar = *c;
    bar.oy += y;
    fill(c, 0, w->r.h - BAR_H, w->r.w, BAR_H, C_WINDOW);
    static const char *const labels[3] = {"Open", "Play", "Stop"};
    for (int i = 0; i < 3; ++i)
        button(&bar, button_rect(i), i == 1 && shown_playing ? "Pause" : labels[i], 0, 0);
    struct rect seek = seek_rect(w);
    if (seek.w > 20) {
        bevel(&bar, seek.x, seek.y + 8, seek.w, 8, 1);
        int filled = (int)(shown_position * (float)(seek.w - 4));
        if (filled > 0) fill(&bar, seek.x + 2, seek.y + 10, filled, 4, C_SELECTION);
    }
    char now[32], length[32], line[80];
    format_time(now, sizeof(now), shown_time);
    format_time(length, sizeof(length), shown_length);
    snprintf(line, sizeof(line), "%s / %s", now, length);
    text(&bar, seek.x + seek.w + 8, 6, &font_regular, line, C_TEXT);
    struct rect v = volume_rect(w);
    bevel(&bar, v.x, v.y + 8, v.w, 8, 1);
    fill(&bar, v.x + 2, v.y + 10, (v.w - 4) * volume / 125, 4, C_SELECTION);
}

static void toggle(void) {
    if (libvlc_media_player_get_media(player)) {
        if (libvlc_media_player_is_playing(player)) libvlc_media_player_pause(player);
        else libvlc_media_player_play(player);
    } else {
        file_dialog("Open Media", 0, 0, opened, 0);
    }
}

static void seek_by(long long ms) {
    long long t = libvlc_media_player_get_time(player) + ms;
    libvlc_media_player_set_time(player, t < 0 ? 0 : t);
}

static void set_volume(int percent) {
    volume = percent < 0 ? 0 : percent > 125 ? 125 : percent;
    window_paint(main_w);
}

static void player_mouse(struct window *w, int x, int y, int kind, int value) {
    if (kind == M_WHEEL) { set_volume(volume - value * 5); return; }
    if (kind != M_DOWN && kind != M_DOUBLE) return;
    if (y < w->r.h - BAR_H) {
        if (kind == M_DOWN) toggle();
        return;
    }
    y -= w->r.h - BAR_H + 5;
    if (rect_has(button_rect(0), x, y)) file_dialog("Open Media", 0, 0, opened, 0);
    else if (rect_has(button_rect(1), x, y)) toggle();
    else if (rect_has(button_rect(2), x, y)) { libvlc_media_player_stop(player); audio_close(); video_cleanup(0); }
    struct rect seek = seek_rect(w), v = volume_rect(w);
    if (rect_has(seek, x, y) && seek.w > 4) libvlc_media_player_set_position(player, (float)(x - seek.x) / (float)seek.w);
    if (rect_has(v, x, y)) set_volume((x - v.x) * 125 / v.w);
}

static void player_key(struct window *w, int key) {
    (void)w;
    switch (key) {
    case ' ': toggle(); break;
    case 0x103: seek_by(10000); break;
    case 0x104: seek_by(-10000); break;
    case 0x101: set_volume(volume + 5); break;
    case 0x102: set_volume(volume - 5); break;
    case 15: file_dialog("Open Media", 0, 0, opened, 0); break;   /* Ctrl+O */
    case 's': case 'S': libvlc_media_player_stop(player); audio_close(); break;
    }
}

static void player_tick(struct window *w) {
    float position = libvlc_media_player_get_position(player);
    long long time = libvlc_media_player_get_time(player), length = libvlc_media_player_get_length(player);
    int playing = libvlc_media_player_is_playing(player);
    if (position != shown_position || time / 1000 != shown_time / 1000 || length != shown_length || playing != shown_playing) {
        shown_position = position < 0 ? 0 : position;
        shown_time = time;
        shown_length = length;
        shown_playing = playing;
        window_paint(w);
    }
    if (libvlc_media_player_get_state(player) == libvlc_Ended) audio_close();
}

static int new_frame(void) {
    if (!frame.fresh || !main_w) return 0;
    window_paint(main_w);
    return 1;
}

static void player_closed(struct window *w) {
    (void)w;
    libvlc_media_player_stop(player);
    audio_close();
    exit(0);
}

static const struct app_ops player_ops = {
    .paint = player_paint, .mouse = player_mouse, .key = player_key, .tick = player_tick, .closed = player_closed,
};

int main(int argc, char **argv) {
    if (gui_connect()) return 1;
    static const char *const options[] = {"--no-video-title-show", "--no-osd", "--no-snapshot-preview", "--quiet",
                                          "--no-xlib", "--avcodec-threads=0"};
    vlc = libvlc_new((int)(sizeof(options) / sizeof(*options)), options);
    if (!vlc) { message_box("VLC media player", "VLC could not start.", 0); gui_run(); return 1; }
    player = libvlc_media_player_new(vlc);
    libvlc_video_set_callbacks(player, video_lock, video_unlock, video_display, 0);
    libvlc_video_set_format_callbacks(player, video_setup, video_cleanup);
    libvlc_audio_set_callbacks(player, audio_play, audio_pause, audio_resume, audio_flush, audio_drain, 0);
    libvlc_audio_set_format(player, "S16N", RATE, CHANNELS);
    main_w = window_open(800, 450 + BAR_H, title, ICON_PROGRAM, &player_ops, 0);
    if (!main_w) return 1;
    main_w->resizable = 1;
    gui_set_poll(new_frame);
    if (argc > 1) play_file(argv[1]);
    gui_run();
    return 0;
}
