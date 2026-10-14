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
#include <string.h>

#include "common/msg.h"
#include "options/m_option.h"
#include "osdep/timer.h"
#include "sub/osd.h"
#include "video/mp_image.h"
#include "video/mp_image_pool.h"
#include "video/sws_utils.h"
#include "vo.h"
#include "mister_conn.h"
#include "mister_calib.h"
#include "mister_pad.h"
#include "options/m_config.h"
#include "video/csputils.h"

struct priv {
    char *host;
    char *modeline;
    int near_lvl;
    int field_order;          // 0 = TFF, 1 = BFF
    bool pad;                 // MiSTer gamepad controls playback
    int flicker;              // 0 off, 1 light, 2 full

    bool connected;
    bool auto_mode;           // pick the modeline from the source (fps / height)
    bool decided;
    struct mp_image_params src_params;
    double last_pts;
    double diffs[8];
    int ndiff;
    struct mister_info info;
    struct mp_sws_context *sws;
    struct mp_image *img;     // RGB24, info.width x info.height
    struct mp_rect src, dst;
    struct mp_osd_res osd;
    struct mp_csp_equalizer_state *eq;   // brightness/contrast/saturation
    struct m_config_cache *fx;   // black level / scanlines / CRT geometry
    struct mister_rect pic;      // picture rect in the full-frame raster (may exceed it)
    struct mister_rect pic_field; // same in the calibration raster (one field if interlaced)
    bool calibrate;
    uint8_t *calib_buf;          // field-sized pattern (interlaced only)
};

static void update_geometry(struct vo *vo);

static void update_fx(struct vo *vo)
{
    struct priv *p = vo->priv;
    struct mp_csp_params cp = MP_CSP_PARAMS_DEFAULTS;
    mp_csp_equalizer_state_get(p->eq, &cp);
    m_config_cache_update(p->fx);
    const struct mister_fx_opts *f = p->fx->opts;
    mister_set_fx(cp.brightness * 100.0, (cp.contrast - 1.0) * 100.0,
                  (cp.saturation - 1.0) * 100.0, f->blacklevel, f->scanlines);
    update_geometry(vo);
}

// Picture rect in the full-frame raster; the calibration raster is one field when interlaced.
static void update_geometry(struct vo *vo)
{
    struct priv *p = vo->priv;
    const struct mister_fx_opts *f = p->fx->opts;
    const int w = p->info.width, h = p->info.height;
    const int k = p->info.interlaced ? 2 : 1;
    struct mister_rect pf;
    mister_picture_rect(f->hsize, f->vsize, f->hoffset, f->voffset, w, h / k, &pf);
    struct mister_rect pic = {pf.x, pf.y * k, pf.w, pf.h * k};
    const bool changed = memcmp(&pic, &p->pic, sizeof(pic)) || p->calibrate != f->calibrate;
    p->pic = pic;
    p->pic_field = pf;
    p->calibrate = f->calibrate;
    if (changed && p->src_params.w > 0 && p->img) {
        mp_image_clear(p->img, 0, 0, p->img->w, p->img->h);
        vo->want_redraw = true;
        vo->dwidth = -1;   // force setup_scaling() on the next draw
    }
}

// Clip dst to the raster and shrink src by the same proportion.
static void clip_rects(struct mp_rect *dst, struct mp_rect *src, int W, int H)
{
    const int dl[2] = {dst->x0, dst->y0}, dh[2] = {dst->x1, dst->y1};
    const int lim[2] = {W, H};
    int sl[2] = {src->x0, src->y0}, sh[2] = {src->x1, src->y1};
    int nl[2], nh[2];
    for (int i = 0; i < 2; i++) {
        nl[i] = dl[i] < 0 ? 0 : dl[i];
        nh[i] = dh[i] > lim[i] ? lim[i] : dh[i];
        if (nh[i] <= nl[i]) { nl[i] = nh[i] = 0; sl[i] = sh[i] = 0; continue; }
        const double span = dh[i] - dl[i], sspan = sh[i] - sl[i];
        const int s0 = sl[i] + (int)((nl[i] - dl[i]) * sspan / span);
        const int s1 = sh[i] - (int)((dh[i] - nh[i]) * sspan / span);
        sl[i] = s0; sh[i] = s1;
    }
    *dst = (struct mp_rect){nl[0], nl[1], nh[0], nh[1]};
    *src = (struct mp_rect){sl[0], sl[1], sh[0], sh[1]};
}

static int setup_scaling(struct vo *vo)
{
    struct priv *p = vo->priv;
    struct mp_image_params *params = &p->src_params;

    vo->dwidth = p->pic.w;
    vo->dheight = p->pic.h;
    vo_get_src_dst_rects(vo, &p->src, &p->dst, &p->osd);
    p->dst.x0 += p->pic.x; p->dst.x1 += p->pic.x;
    p->dst.y0 += p->pic.y; p->dst.y1 += p->pic.y;
    clip_rects(&p->dst, &p->src, p->info.width, p->info.height);

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

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct priv *p = vo->priv;
    p->src_params = *params;
    p->decided = !p->auto_mode;
    p->ndiff = 0;
    p->last_pts = -1;
    return setup_scaling(vo);
}

// Source-matched modeline: 25/50 fps content (PAL) -> 50 Hz family, everything else -> 60 Hz.
static const char *pick_modeline(int h, double fps)
{
    const bool pal = (fps > 24.5 && fps < 26.0) || (fps > 48.5 && fps < 52.0);
    if (pal)
        return h <= 288 ? "320x288p50" : "720x576i50";
    return h <= 240 ? "320x240p60" : "640x480i60";
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void auto_decide(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;
    if (!frame->current || frame->redraw || frame->repeat)
        return;
    double pts = frame->current->pts;
    if (pts == MP_NOPTS_VALUE)
        return;
    if (p->last_pts >= 0 && pts > p->last_pts)
        p->diffs[p->ndiff++] = pts - p->last_pts;
    p->last_pts = pts;
    if (p->ndiff < 8)
        return;

    qsort(p->diffs, p->ndiff, sizeof(double), cmp_double);
    double fps = 1.0 / p->diffs[p->ndiff / 2];
    const char *ml = pick_modeline(p->src_params.h, fps);
    p->decided = true;
    MP_INFO(vo, "source %dx%d @ %.3f fps -> modeline %s\n",
            p->src_params.w, p->src_params.h, fps, ml);

    struct mister_info info;
    if (!mister_switch(ml, &info))
        return;
    if (info.width != p->info.width || info.height != p->info.height) {
        talloc_free(p->img);
        p->img = mp_image_alloc(IMGFMT_BGR24, info.width, info.height);
        mp_image_params_guess_csp(&p->img->params);
    }
    p->info = info;
    mp_image_clear(p->img, 0, 0, p->img->w, p->img->h);
    update_geometry(vo);   // the raster changed with the modeline
    setup_scaling(vo);
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;
    if (!p->decided)
        auto_decide(vo, frame);
    struct mp_image *img = p->img;

    if (vo->dwidth != p->pic.w)
        setup_scaling(vo);

    if (p->calibrate) {
        const int w = p->info.width, h = p->info.height;
        if (!p->info.interlaced) {
            mister_render_calibration(img->planes[0], w, h, &p->pic_field);
        } else {   // draw one field, then repeat every line so both fields carry it
            if (!p->calib_buf)
                p->calib_buf = talloc_size(p, (size_t)w * (h / 2) * 3);
            mister_render_calibration(p->calib_buf, w, h / 2, &p->pic_field);
            for (int y = 0; y < h; y++)
                memcpy(img->planes[0] + (size_t)y * img->stride[0],
                       p->calib_buf + (size_t)(y / 2) * w * 3, (size_t)w * 3);
        }
        mister_set_frame(img->planes[0], img->stride[0]);
        return VO_TRUE;
    }

    if (frame->current) {
        mp_image_clear_rc_inv(img, p->dst);

        // Crop a shallow copy: cropping frame->current itself would compound on every redraw.
        struct mp_image srcimg = *frame->current;
        struct mp_image *src = &srcimg;
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

    struct mp_image osd_img = *img;
    struct mp_rect pr = {MPMAX(p->pic.x, 0), MPMAX(p->pic.y, 0),
                         MPMIN(p->pic.x + p->pic.w, img->w), MPMIN(p->pic.y + p->pic.h, img->h)};
    mp_image_crop_rc(&osd_img, pr);   // subtitles live inside the calibrated picture
    osd_draw_on_image(vo->osd, p->osd, frame->current ? frame->current->pts : 0, 0, &osd_img);
    mister_set_frame(img->planes[0], img->stride[0]);
    return VO_TRUE;
}

static void flip_page(struct vo *vo)
{
}

static int query_format(struct vo *vo, int format)
{
    struct priv *p = vo->priv;
    return mp_sws_supports_formats(p->sws, IMGFMT_BGR24, format);
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

    p->auto_mode = !p->modeline || !*p->modeline || !strcmp(p->modeline, "auto");
    mister_set_interlace(p->field_order, p->flicker);
    if (!mister_acquire(p->host, p->modeline, p->near_lvl, &p->info)) {
        MP_ERR(vo, "Could not connect to the MiSTer.\n");
        return -1;
    }
    p->connected = true;
    if (p->pad)
        mister_pad_start(vo->input_ctx, p->host);
    p->eq = mp_csp_equalizer_create(p, vo->global);
    p->fx = m_config_cache_alloc(p, vo->global, &mister_fx_conf);
    update_fx(vo);

    p->img = mp_image_alloc(IMGFMT_BGR24, p->info.width, p->info.height);
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
    case VOCTRL_UPDATE_RENDER_OPTS:
        update_fx(vo);
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
    .priv_defaults = &(const struct priv) { .near_lvl = 1, .flicker = 1, .pad = true },
    .options = (const struct m_option[]) {
        {"host", OPT_STRING(host)},
        {"modeline", OPT_STRING(modeline)},
        {"near", OPT_INT(near_lvl), M_RANGE(0, 3)},
        {"field-order", OPT_CHOICE(field_order, {"tff", 0}, {"bff", 1})},
        {"pad", OPT_BOOL(pad)},
        {"flicker", OPT_CHOICE(flicker, {"off", 0}, {"light", 1}, {"full", 2})},
        {0}
    },
    .options_prefix = "vo-mister",
};
