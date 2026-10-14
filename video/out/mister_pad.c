// MiSTer gamepad -> mpv commands. Wire button positions follow the Groovy core's joystick
// bitfield (see groovy_mister.h): d-pad on bits 0-3, B1.. on 0x10 and up; the MiSTer maps
// whatever pad is plugged in to this generic layout (B1=cross/A, B2=circle/B, B3=square/X,
// B4=triangle/Y, B5/B6=L1/R1, B7=select, B8=start, B9/B10=L2/R2).
//
//   D-pad L/R   seek -/+10 s, hold = scan          D-pad U/D   volume +/-5
//   B1 / B8     pause                              B2          stop (quit)
//   B3          next audio track                   B4          next subtitle track
//   B5 / B6     previous / next playlist entry     B7          status on screen
//   B9 / B10    seek -/+60 s
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/common.h"
#include "common/msg.h"
#include "input/input.h"
#include "mister_pad.h"
#include "groovy_mister.h"

struct pad {
    char *host;
    atomic_bool stop;
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void feed(struct mp_input_src *src, const char *cmd)
{
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "%s\n", cmd);
    if (n > 0 && n < (int)sizeof(buf))
        mp_input_src_feed_cmd_text(src, buf, n);
}

static const char STATUS[] =
    "show-text \"${?playlist-count:${playlist-pos-1}/${playlist-count}  }${media-title}\\n"
    "${time-pos} / ${duration}  (-${time-remaining})${?pause==yes:  (paused)}\\n"
    "Audio: ${current-tracks/audio/lang:?} ${current-tracks/audio/title:}\\n"
    "Subs: ${current-tracks/sub/lang:off} ${current-tracks/sub/title:}\" 5000";

struct btn { uint16_t mask; const char *cmd; };
static const struct btn PRESS[] = {
    {GMZ_JOY_B1,  "osd-auto cycle pause"},
    {GMZ_JOY_B8,  "osd-auto cycle pause"},
    {GMZ_JOY_B2,  "quit"},
    {GMZ_JOY_B3,  "osd-auto cycle audio"},
    {GMZ_JOY_B4,  "osd-auto cycle sub"},
    {GMZ_JOY_B5,  "osd-auto playlist-prev"},
    {GMZ_JOY_B6,  "osd-auto playlist-next"},
    {GMZ_JOY_B7,  STATUS},
    {GMZ_JOY_B9,  "osd-auto seek -60"},
    {GMZ_JOY_B10, "osd-auto seek 60"},
    {GMZ_JOY_UP,  "osd-auto add volume 5"},
    {GMZ_JOY_DOWN, "osd-auto add volume -5"},
    {GMZ_JOY_LEFT, "osd-auto seek -10"},
    {GMZ_JOY_RIGHT, "osd-auto seek 10"},
};

static void pad_cancel(struct mp_input_src *src)
{
    struct pad *p = src->priv;
    atomic_store(&p->stop, true);
}

static void pad_uninit(struct mp_input_src *src)
{
    struct pad *p = src->priv;
    free(p->host);
    free(p);
}

static void pad_loop(struct mp_input_src *src, void *ctx)
{
    struct pad *p = ctx;
    gmz_input_t in = gmz_input_bind(p->host);
    if (!in) {
        MP_WARN(src, "MiSTer gamepad: cannot open input stream\n");
        free(p->host);
        free(p);
        return;
    }
    src->priv = p;
    src->cancel = pad_cancel;
    src->uninit = pad_uninit;
    mp_input_src_init_done(src);
    MP_INFO(src, "MiSTer gamepad control active\n");

    uint16_t known = 0;
    for (size_t i = 0; i < sizeof(PRESS) / sizeof(PRESS[0]); i++)
        known |= PRESS[i].mask;

    // The FPGA only sends a packet when the state changes, so `cur` is the last known
    // state and the hold logic below has to run on every tick, not only on new packets.
    uint16_t cur = 0, prev = 0;
    uint64_t held_since = 0, last_scan = 0;
    while (!atomic_load(&p->stop)) {
        if (gmz_input_poll(in))
            cur = gmz_input_joy(in).joy1;
        const uint16_t down = cur & (uint16_t)~prev;
        for (size_t i = 0; i < sizeof(PRESS) / sizeof(PRESS[0]); i++)
            if (down & PRESS[i].mask)
                feed(src, PRESS[i].cmd);
        if (down & ~known) {   // a button without a function: show its bit so it can be mapped
            char buf[96];
            snprintf(buf, sizeof(buf), "show-text \"Pad button 0x%04x (unmapped)\" 2000",
                     down & ~known);
            feed(src, buf);
        }
        // Holding left/right scans: after 400 ms jump 20 s every 300 ms.
        const uint16_t lr = cur & (GMZ_JOY_LEFT | GMZ_JOY_RIGHT);
        if (lr == GMZ_JOY_LEFT || lr == GMZ_JOY_RIGHT) {
            const uint64_t t = now_ms();
            if (!(prev & lr)) held_since = t;        // just pressed
            if (t - held_since > 400 && t - last_scan > 300) {
                feed(src, lr == GMZ_JOY_LEFT ? "osd-auto seek -20" : "osd-auto seek 20");
                last_scan = t;
            }
        } else {
            held_since = 0;
        }
        prev = cur;
        struct timespec ts = {0, 8 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    gmz_input_close(in);
}

void mister_pad_start(struct input_ctx *ictx, const char *host)
{
    if (!ictx || !host || !*host)
        return;
    struct pad *p = calloc(1, sizeof(*p));
    if (!p)
        return;
    p->host = strdup(host);
    if (mp_input_add_thread_src(ictx, p, pad_loop) < 0) {
        free(p->host);
        free(p);
    }
}
