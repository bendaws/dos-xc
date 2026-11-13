/*
 * DOS-XC /vlc/dosxc_vout.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

/* Draws the video in the Qt interface's video widget (a DOS-XC embed_window,
   passed as the HWND window handle).  The core scales the pictures to the
   placed size; this only copies them and fills the bars. */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_vout_display.h>
#include <vlc_picture_pool.h>
#include "embed.h"

struct vout_display_sys_t {
    vout_window_t *window;
    picture_pool_t *pool;
};

static void place_format(vout_display_t *vd, video_format_t *fmt, const vout_display_cfg_t *cfg)
{
    vout_display_place_t place;
    vout_display_PlacePicture(&place, &vd->source, cfg, false);
    fmt->i_width = fmt->i_visible_width = place.width > 0 ? place.width : 1;
    fmt->i_height = fmt->i_visible_height = place.height > 0 ? place.height : 1;
    fmt->i_x_offset = fmt->i_y_offset = 0;
    fmt->i_sar_num = fmt->i_sar_den = 1;
}

static picture_pool_t *Pool(vout_display_t *vd, unsigned count)
{
    vout_display_sys_t *sys = vd->sys;
    if (!sys->pool) sys->pool = picture_pool_NewFromFormat(&vd->fmt, count);
    return sys->pool;
}

static void Display(vout_display_t *vd, picture_t *pic, subpicture_t *sub)
{
    vout_display_sys_t *sys = vd->sys;
    struct embed_window *e = sys->window->handle.hwnd;
    vout_display_place_t place;
    vout_display_PlacePicture(&place, &vd->source, vd->cfg, false);

    pthread_mutex_lock(e->lock);
    struct window *top = e->top;
    if (top && e->visible && top->content.px) {
        struct rect inside = rect_both((struct rect){0, 0, e->w, e->h}, (struct rect){-e->x, -e->y, top->content.w, top->content.h});
        struct canvas c = {&top->content, e->x, e->y, inside};
        int pw = (int)vd->fmt.i_visible_width, ph = (int)vd->fmt.i_visible_height;
        int px = place.x + (place.width - pw) / 2, py = place.y + (place.height - ph) / 2;
        fill(&c, 0, 0, e->w, py, 0);
        fill(&c, 0, py + ph, e->w, e->h - py - ph, 0);
        fill(&c, 0, py, px, ph, 0);
        fill(&c, px + pw, py, e->w - px - pw, ph, 0);
        struct rect r = rect_both((struct rect){px, py, pw, ph}, inside);
        for (int y = 0; y < r.h; ++y)
            memcpy(top->content.px + (size_t)(r.y + y + e->y) * top->content.w + r.x + e->x,
                   pic->p[0].p_pixels + (size_t)(r.y - py + y) * pic->p[0].i_pitch + (size_t)(r.x - px) * 4, (size_t)r.w * 4);
        ++top->seq;
    }
    pthread_mutex_unlock(e->lock);
    picture_Release(pic);
    if (sub) subpicture_Delete(sub);
}

static int Control(vout_display_t *vd, int query, va_list args)
{
    vout_display_sys_t *sys = vd->sys;
    switch (query) {
    case VOUT_DISPLAY_CHANGE_DISPLAY_SIZE:
    case VOUT_DISPLAY_CHANGE_DISPLAY_FILLED:
    case VOUT_DISPLAY_CHANGE_ZOOM:
    case VOUT_DISPLAY_CHANGE_SOURCE_ASPECT:
    case VOUT_DISPLAY_CHANGE_SOURCE_CROP: {
        const vout_display_cfg_t *cfg = query == VOUT_DISPLAY_CHANGE_SOURCE_ASPECT || query == VOUT_DISPLAY_CHANGE_SOURCE_CROP
                                      ? vd->cfg : va_arg(args, const vout_display_cfg_t *);
        video_format_t fmt = vd->fmt;
        place_format(vd, &fmt, cfg);
        if (fmt.i_width != vd->fmt.i_width || fmt.i_height != vd->fmt.i_height) vout_display_SendEventPicturesInvalid(vd);
        return VLC_SUCCESS;
    }
    case VOUT_DISPLAY_RESET_PICTURES:
        if (sys->pool) picture_pool_Release(sys->pool);
        sys->pool = NULL;
        place_format(vd, &vd->fmt, vd->cfg);
        return VLC_SUCCESS;
    default:
        return VLC_EGENERIC;
    }
}

static int Open(vlc_object_t *object)
{
    vout_display_t *vd = (vout_display_t *)object;
    vout_window_t *window = vout_display_NewWindow(vd, VOUT_WINDOW_TYPE_HWND);
    if (!window) return VLC_EGENERIC;
    vout_display_sys_t *sys = calloc(1, sizeof(*sys));
    if (!sys) {
        vout_display_DeleteWindow(vd, window);
        return VLC_ENOMEM;
    }
    sys->window = window;

    video_format_t fmt;
    video_format_ApplyRotation(&fmt, &vd->fmt);
    fmt.i_chroma = VLC_CODEC_RGB32;
    fmt.i_rmask = 0xff0000;
    fmt.i_gmask = 0x00ff00;
    fmt.i_bmask = 0x0000ff;
    place_format(vd, &fmt, vd->cfg);

    vd->sys = sys;
    vd->fmt = fmt;
    vd->info.has_pictures_invalid = true;
    vd->pool = Pool;
    vd->prepare = NULL;
    vd->display = Display;
    vd->control = Control;
    return VLC_SUCCESS;
}

static void Close(vlc_object_t *object)
{
    vout_display_t *vd = (vout_display_t *)object;
    vout_display_sys_t *sys = vd->sys;
    if (sys->pool) picture_pool_Release(sys->pool);
    vout_display_DeleteWindow(vd, sys->window);
    free(sys);
}

vlc_module_begin()
    set_shortname("DOS-XC")
    set_description("DOS-XC desktop video output")
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VOUT)
    set_capability("vout display", 250)
    set_callbacks(Open, Close)
vlc_module_end()
