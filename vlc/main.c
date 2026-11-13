/*
 * DOS-XC /vlc/main.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

/* VLC.EXE: libvlc with its Qt interface, as VLC's own vlc program starts it. */

#include <pthread.h>
#include <stdlib.h>
#include <vlc/vlc.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t done = PTHREAD_COND_INITIALIZER;
static int quitting;

static void quit(void *data)
{
    (void)data;
    pthread_mutex_lock(&lock);
    quitting = 1;
    pthread_cond_signal(&done);
    pthread_mutex_unlock(&lock);
}

int main(int argc, char **argv)
{
    setenv("QT_QPA_PLATFORM", "dosxc", 0);
    const char *args[argc + 5];
    int n = 0;
    args[n++] = "--no-ignore-config";
    args[n++] = "--media-library";
    args[n++] = "--no-qt-privacy-ask";
    args[n++] = "--avcodec-threads=0";
    args[n++] = "--intf=qt";
    for (int i = 1; i < argc; ++i) args[n++] = argv[i];
    libvlc_instance_t *vlc = libvlc_new(n, args);
    if (!vlc) return 1;
    libvlc_set_user_agent(vlc, "VLC media player", "VLC/3.0.21 DOS-XC");
    libvlc_set_app_id(vlc, "org.VideoLAN.VLC", "3.0.21", "vlc");
    libvlc_set_exit_handler(vlc, quit, NULL);
    if (libvlc_add_intf(vlc, "qt")) {
        libvlc_release(vlc);
        return 1;
    }
    libvlc_playlist_play(vlc, -1, 0, NULL);
    pthread_mutex_lock(&lock);
    while (!quitting) pthread_cond_wait(&done, &lock);
    pthread_mutex_unlock(&lock);
    libvlc_release(vlc);
    return 0;
}
