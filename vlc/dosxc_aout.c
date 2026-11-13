/*
 * DOS-XC /vlc/dosxc_aout.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

/* Plays 16-bit stereo through DOSXC_SYS_AUDIO.  The sound device runs at
   48 kHz only, so other rates are resampled here (linear interpolation). */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_aout.h>
#include <dosxc/syscall.h>
#include <unistd.h>

#define DEVICE_RATE 48000
#define CHUNK 2048

struct aout_sys_t {
    unsigned rate;
    uint64_t written;                /* device frames handed over since open */
    uint64_t step;                   /* source frames per device frame, 16.16 */
    uint64_t phase;
    int16_t last[2];                 /* the frame before this block */
    float volume;
    bool mute, open;
};

static int device_open(aout_sys_t *sys)
{
    if (dosxc_sys_audio(DOSXC_AUDIO_OPEN, DEVICE_RATE, 2) < 0) return -1;
    sys->open = true;
    sys->written = 0;
    sys->phase = 0;
    sys->last[0] = sys->last[1] = 0;
    return 0;
}

static void device_close(aout_sys_t *sys)
{
    if (sys->open) dosxc_sys_audio(DOSXC_AUDIO_CLOSE, 0, 0);
    sys->open = false;
}

static void device_write(aout_sys_t *sys, const int16_t *frames, unsigned n)
{
    unsigned done = 0;
    for (int waited = 0; done < n && waited < 1000; ++waited) {
        long taken = dosxc_sys_audio(DOSXC_AUDIO_WRITE, (long)(frames + done * 2), n - done);
        if (taken < 0) return;
        done += (unsigned)taken;
        if (done < n) usleep(2000);
    }
    sys->written += done;
}

static int TimeGet(audio_output_t *aout, mtime_t *delay)
{
    aout_sys_t *sys = aout->sys;
    dosxc_u64 played = 0;
    if (!sys->open || dosxc_sys_audio(DOSXC_AUDIO_POSITION, (long)&played, 0) < 0) return -1;
    if (played > sys->written) played = sys->written;
    *delay = (mtime_t)(sys->written - played) * CLOCK_FREQ / DEVICE_RATE;
    return 0;
}

static void Play(audio_output_t *aout, block_t *block)
{
    aout_sys_t *sys = aout->sys;
    if (!sys->open && device_open(sys)) { block_Release(block); return; }
    int gain = sys->mute ? 0 : (int)(sys->volume * sys->volume * sys->volume * 256.f);
    const int16_t *in = (const int16_t *)block->p_buffer;
    unsigned count = block->i_nb_samples;
    int16_t out[CHUNK * 2];
    unsigned n = 0;
    /* Device frame k lies between source frames i - 1 and i (i - 1 is LAST when i is 0). */
    for (;;) {
        uint64_t i = sys->phase >> 16;
        if (i >= count) break;
        int frac = (int)(sys->phase & 0xffff);
        for (int ch = 0; ch < 2; ++ch) {
            int a = i ? in[(i - 1) * 2 + ch] : sys->last[ch], b = in[i * 2 + ch];
            int v = (a + (int)(((int64_t)(b - a) * frac) >> 16)) * gain >> 8;
            out[n * 2 + ch] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        sys->phase += sys->step;
        if (++n == CHUNK) { device_write(sys, out, n); n = 0; }
    }
    if (n) device_write(sys, out, n);
    if (count) {
        sys->last[0] = in[(count - 1) * 2];
        sys->last[1] = in[(count - 1) * 2 + 1];
    }
    sys->phase -= (uint64_t)count << 16;
    block_Release(block);
}

static void Pause(audio_output_t *aout, bool paused, mtime_t date)
{
    (void)aout; (void)paused; (void)date;
}

static void Flush(audio_output_t *aout, bool wait)
{
    aout_sys_t *sys = aout->sys;
    mtime_t delay;
    if (wait) {
        for (int i = 0; i < 500 && !TimeGet(aout, &delay) && delay > 0; ++i) usleep(10000);
        return;
    }
    device_close(sys);
}

static int VolumeSet(audio_output_t *aout, float volume)
{
    aout->sys->volume = volume;
    aout_VolumeReport(aout, volume);
    return 0;
}

static int MuteSet(audio_output_t *aout, bool mute)
{
    aout->sys->mute = mute;
    aout_MuteReport(aout, mute);
    return 0;
}

static int Start(audio_output_t *aout, audio_sample_format_t *fmt)
{
    aout_sys_t *sys = aout->sys;
    if (!AOUT_FMT_LINEAR(fmt) || !fmt->i_rate) return VLC_EGENERIC;
    fmt->i_format = VLC_CODEC_S16N;
    fmt->i_physical_channels = AOUT_CHANS_STEREO;
    aout_FormatPrepare(fmt);
    sys->rate = fmt->i_rate;
    sys->step = ((uint64_t)fmt->i_rate << 16) / DEVICE_RATE;
    if (device_open(sys)) {
        msg_Err(aout, "the sound device is busy or missing");
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

static void Stop(audio_output_t *aout)
{
    device_close(aout->sys);
}

static int Open(vlc_object_t *object)
{
    audio_output_t *aout = (audio_output_t *)object;
    aout_sys_t *sys = calloc(1, sizeof(*sys));
    if (!sys) return VLC_ENOMEM;
    sys->volume = 1.f;
    aout->sys = sys;
    aout->start = Start;
    aout->stop = Stop;
    aout->time_get = TimeGet;
    aout->play = Play;
    aout->pause = Pause;
    aout->flush = Flush;
    aout->volume_set = VolumeSet;
    aout->mute_set = MuteSet;
    aout_VolumeReport(aout, sys->volume);
    aout_MuteReport(aout, false);
    return VLC_SUCCESS;
}

static void Close(vlc_object_t *object)
{
    free(((audio_output_t *)object)->sys);
}

vlc_module_begin()
    set_shortname("DOS-XC")
    set_description("DOS-XC sound output")
    set_category(CAT_AUDIO)
    set_subcategory(SUBCAT_AUDIO_AOUT)
    set_capability("audio output", 250)
    set_callbacks(Open, Close)
vlc_module_end()
