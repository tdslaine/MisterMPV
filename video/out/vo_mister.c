/*
 * Video output to a MiSTer FPGA running the GroovyNLC core (CRT output).
 *
 * Scales/letterboxes each frame to the modeline's active area as RGB24, hands it to the
 * shared sender thread (mister_conn.c), which GroovyNLC-encodes and streams it over UDP at
 * the CRT refresh rate (interlaced modelines alternate fields).
 *
 * This file is part of mpv.
 * mpv is free software; you can redistribute it and/or modify it under the terms of the
 * GNU Lesser General Public License as published by the Free Software Foundation;
 * either version 2.1 of the License, or (at your option) any later version.
 */

#include <stdlib.h>

#include "common/msg.h"
#include "options/m_option.h"
#include "osdep/timer.h"
#include "sub/osd.h"
#include "video/mp_image.h"
#include "video/mp_image_pool.h"
#include "video/sws_utils.h"
#include "vo.h"
#include "mister_conn.h"

struct priv {
    char *host;
    char *modeline;
    int near_lvl;

    bool connected;
    struct mister_info info;
    struct mp_sws_context *sws;
    struct mp_image *img;     // RGB24, info.width x info.height
    struct mp_rect src, dst;
    struct mp_osd_res osd;
};

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct priv *p = vo->priv;

    vo->dwidth = p->info.width;
    vo->dheight = p->info.height;
    vo_get_src_dst_rects(vo, &p->src, &p->dst, &p->osd);

    p->sws->src = *params;
    p->sws->src.w = mp_rect_w(p->src);
    p->sws->src.h = mp_rect_h(p->src);
    p->sws->dst = p->img->params;
    p->sws->dst.w = mp_rect_w(p->dst);
    p->sws->dst.h = mp_rect_h(p->dst);
    if (mp_sws_reinit(p->sws) < 0)
        return -1;

    vo->want_redraw = true;
    return 0;
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;
    struct mp_image *img = p->img;

    if (frame->current) {
        mp_image_clear_rc_inv(img, p->dst);

        struct mp_image *src = frame->current;
        struct mp_rect src_rc = p->src;
        src_rc.x0 = MP_ALIGN_DOWN(src_rc.x0, src->fmt.align_x);
        src_rc.y0 = MP_ALIGN_DOWN(src_rc.y0, src->fmt.align_y);
        mp_image_crop_rc(src, src_rc);

        struct mp_image dst = *img;
        mp_image_crop_rc(&dst, p->dst);
        mp_sws_scale(p->sws, &dst, src);
    } else {
        mp_image_clear(img, 0, 0, img->w, img->h);
    }

    osd_draw_on_image(vo->osd, p->osd, frame->current ? frame->current->pts : 0, 0, img);
    mister_set_frame(img->planes[0], img->stride[0]);
    return VO_TRUE;
}

static void flip_page(struct vo *vo)
{
}

static int query_format(struct vo *vo, int format)
{
    struct priv *p = vo->priv;
    return mp_sws_supports_formats(p->sws, IMGFMT_RGB24, format);
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (p->connected)
        mister_release();
    talloc_free(p->img);
}

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;
    p->sws = mp_sws_alloc(vo);
    p->sws->log = vo->log;
    mp_sws_enable_cmdline_opts(p->sws, vo->global);

    if (!mister_acquire(p->host, p->modeline, p->near_lvl, &p->info)) {
        MP_ERR(vo, "Could not connect to the MiSTer.\n");
        return -1;
    }
    p->connected = true;

    p->img = mp_image_alloc(IMGFMT_RGB24, p->info.width, p->info.height);
    if (!p->img)
        return -1;
    mp_image_clear(p->img, 0, 0, p->img->w, p->img->h);
    mp_image_params_guess_csp(&p->img->params);
    MP_INFO(vo, "MiSTer output: %dx%d%s %.2f Hz\n", p->info.width, p->info.height,
            p->info.interlaced ? "i" : "p", p->info.refresh);
    return 0;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct priv *p = vo->priv;
    switch (request) {
    case VOCTRL_GET_DISPLAY_FPS:
        *(double *)data = p->info.refresh;
        return VO_TRUE;
    case VOCTRL_RESET:
    case VOCTRL_PAUSE:
    case VOCTRL_RESUME:
        return VO_TRUE;
    }
    return VO_NOTIMPL;
}

#define OPT_BASE_STRUCT struct priv
const struct vo_driver video_out_mister = {
    .description = "MiSTer FPGA (GroovyNLC) CRT output",
    .name = "mister",
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
    .priv_defaults = &(const struct priv) { .near_lvl = 1 },
    .options = (const struct m_option[]) {
        {"host", OPT_STRING(host)},
        {"modeline", OPT_STRING(modeline)},
        {"near", OPT_INT(near_lvl), M_RANGE(0, 3)},
        {0}
    },
    .options_prefix = "vo-mister",
};
