/*
 * Audio output to a MiSTer FPGA running the GroovyNLC core.
 * Fixed 48 kHz, S16, stereo. Shares the UDP connection with vo_mister (mister_conn.c).
 * The FPGA has no clock feedback here, so the device is paced by a software clock.
 *
 * This file is part of mpv.
 * mpv is free software; you can redistribute it and/or modify it under the terms of the
 * GNU Lesser General Public License as published by the Free Software Foundation;
 * either version 2.1 of the License, or (at your option) any later version.
 */

#include "mpv_talloc.h"

#include "audio/format.h"
#include "common/common.h"
#include "common/msg.h"
#include "options/m_option.h"
#include "options/m_config.h"
#include "osdep/timer.h"
#include "ao.h"
#include "internal.h"
#include "video/out/mister_conn.h"

#define RATE 48000
#define BUFFER_SEC 0.25

struct priv {
    char *host;
    char *modeline;
    int near_lvl;

    bool connected;
    bool paused;
    bool playing;
    double last_time;
    double buffered;   // samples
};

static void drain(struct ao *ao)
{
    struct priv *p = ao->priv;
    double now = mp_time_sec();
    if (!p->paused && p->buffered > 0) {
        p->buffered -= (now - p->last_time) * RATE;
        if (p->buffered < 0)
            p->buffered = 0;
    }
    p->last_time = now;
}

static int init(struct ao *ao)
{
    struct priv *p = ao->priv;

    struct mister_info info;
    // The audio output may connect first: give it the modeline fine-tune as well.
    struct mister_fx_opts *fx = mp_get_config_group(NULL, ao->global, &mister_fx_conf);
    mister_set_sync_shift(fx->sync_shift);
    talloc_free(fx);
    if (!mister_acquire(p->host, p->modeline, p->near_lvl, &info)) {
        MP_ERR(ao, "Could not connect to the MiSTer.\n");
        return -1;
    }
    p->connected = true;

    ao->format = AF_FORMAT_S16;
    ao->samplerate = RATE;
    mp_chmap_from_channels(&ao->channels, 2);
    ao->device_buffer = (int)(RATE * BUFFER_SEC);
    p->last_time = mp_time_sec();
    return 0;
}

static void uninit(struct ao *ao)
{
    struct priv *p = ao->priv;
    if (p->connected)
        mister_release();
}

static void reset(struct ao *ao)
{
    struct priv *p = ao->priv;
    p->paused = false;
    p->buffered = 0;
    p->playing = false;
}

static void start(struct ao *ao)
{
    struct priv *p = ao->priv;
    drain(ao);
    p->paused = false;
    p->last_time = mp_time_sec();
    p->playing = true;
}

static bool set_pause(struct ao *ao, bool paused)
{
    struct priv *p = ao->priv;
    if (p->paused != paused) {
        drain(ao);
        p->paused = paused;
        if (!paused)
            p->last_time = mp_time_sec();
    }
    return true;
}

static bool audio_write(struct ao *ao, void **data, int samples)
{
    struct priv *p = ao->priv;
    mister_push_audio(data[0], (size_t)samples * ao->sstride);
    p->buffered += samples;
    return true;
}

static void get_state(struct ao *ao, struct mp_pcm_state *state)
{
    struct priv *p = ao->priv;
    drain(ao);
    state->free_samples = MPMAX(0, ao->device_buffer - (int)p->buffered);
    state->queued_samples = (int)p->buffered;
    state->delay = p->buffered / RATE;
    state->playing = p->playing && p->buffered > 0;
}

#define OPT_BASE_STRUCT struct priv
const struct ao_driver audio_out_mister = {
    .description = "MiSTer FPGA (GroovyNLC) audio output",
    .name = "mister",
    .init = init,
    .uninit = uninit,
    .reset = reset,
    .start = start,
    .set_pause = set_pause,
    .write = audio_write,
    .get_state = get_state,
    .priv_size = sizeof(struct priv),
    .priv_defaults = &(const struct priv) { .near_lvl = 1 },
    .options = (const struct m_option[]) {
        {"host", OPT_STRING(host)},
        {"modeline", OPT_STRING(modeline)},
        {"near", OPT_INT(near_lvl), M_RANGE(0, 3)},
        {0}
    },
    .options_prefix = "ao-mister",
};
