// Shared GroovyNLC (MiSTer FPGA) connection for the mister VO/AO.
// Frame encoding and the wire format are identical to MiSTerCast's CastEngineGMZ.
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mister_conn.h"
#include "nlc_codec.h"
#include "groovy_mister.h"

struct ml {
    const char *key;
    double pclock;
    int ha, hb, he, ht, va, vb, ve, vt;
    int interlace;
};

static const struct ml MODELINES[] = {
#include "mister_modelines.inc"
};

#define MAX_RECONNECTS 5

static struct {
    pthread_mutex_t lock;        // guards refs, conn lifetime
    pthread_mutex_t gmz_lock;    // serialises all gmz_* calls on conn
    pthread_mutex_t frame_lock;  // guards frame buffers
    int refs;
    gmz_conn_t conn;
    char *host;
    int near_lvl;
    const struct ml *ml;
    struct mister_info info;
    uint8_t *frame;              // full frame, packed RGB24
    uint8_t *field;              // one field, packed RGB24
    uint8_t *nlc_buf;
    size_t nlc_cap;
    pthread_t thread;
    atomic_bool stop;
} G = { .lock = PTHREAD_MUTEX_INITIALIZER, .gmz_lock = PTHREAD_MUTEX_INITIALIZER,
        .frame_lock = PTHREAD_MUTEX_INITIALIZER };

static nlc_params nlc_p(int w, int h, int near_lvl)
{
    nlc_params np;
    memset(&np, 0, sizeof(np));
    np.width = w; np.height = h;
    np.rgb = NLC_RGB888; np.color = NLC_COLOR_YCOCG;
    np.near_lvl = near_lvl;
    np.pack = NLC_PACK_RICE;
    np.tile = 16; np.width_bits = 4; np.rice_k = -1;
    return np;
}

static bool open_conn(void)
{
    if (G.conn) { gmz_disconnect(G.conn); G.conn = NULL; }
    G.conn = gmz_connect_nlc(G.host, 1500, 0, 3 /*48 kHz*/, 2, G.near_lvl, 1, 2);
    if (!G.conn) {
        fprintf(stderr, "[mister] connect to %s failed\n", G.host);
        return false;
    }
    const struct ml *m = G.ml;
    gmz_modeline_t gm;
    memset(&gm, 0, sizeof(gm));
    gm.pixel_clock = m->pclock;
    gm.h_active = m->ha; gm.h_begin = m->hb; gm.h_end = m->he; gm.h_total = m->ht;
    gm.v_active = m->va; gm.v_begin = m->vb; gm.v_end = m->ve; gm.v_total = m->vt;
    gm.interlaced = m->interlace;
    gmz_set_modeline(G.conn, &gm);
    fprintf(stderr, "[mister] connected (%s), modeline %s\n", gmz_version(), m->key);
    return true;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void *sender(void *arg)
{
    (void)arg;
    const int w = G.info.width;
    const int h = G.info.interlaced ? G.info.height / 2 : G.info.height;
    const nlc_params np = nlc_p(w, h, G.near_lvl);
    uint32_t frame = 0;
    uint8_t field = 0;
    uint64_t enc_ns = 0, send_ns = 0;
    int reconnects = 0;
    uint64_t last_ok = now_ns();
    const size_t row = (size_t)w * 3;

    while (!atomic_load(&G.stop)) {
        // begin_frame blocks on the FPGA; keep it outside gmz_lock so audio can flow.
        int pace = gmz_begin_frame(G.conn);
        if (pace == GMZ_PACE_STALLED) {
            fprintf(stderr, "[mister] FPGA stalled, reconnecting\n");
            if (now_ns() - last_ok > 10000000000ull) reconnects = 0;
            bool ok = false;
            while (!atomic_load(&G.stop) && reconnects < MAX_RECONNECTS) {
                reconnects++;
                sleep(reconnects);
                pthread_mutex_lock(&G.gmz_lock);
                bool c = open_conn();
                pthread_mutex_unlock(&G.gmz_lock);
                if (c) { ok = true; frame = 0; break; }
            }
            if (!ok) { fprintf(stderr, "[mister] giving up\n"); return NULL; }
            last_ok = now_ns();
            continue;
        }
        if (pace != GMZ_PACE_READY) continue;

        const uint8_t *src;
        pthread_mutex_lock(&G.frame_lock);
        if (G.info.interlaced) {
            for (int y = 0; y < h; y++)
                memcpy(G.field + y * row, G.frame + ((size_t)y * 2 + field) * row, row);
            src = G.field;
        } else {
            src = G.frame;
        }
        const uint16_t vsync = gmz_calc_vsync(G.conn, 2000000, enc_ns, send_ns);
        uint64_t t0 = now_ns();
        int n = nlc_encode(src, G.nlc_buf, G.nlc_cap, &np);
        uint64_t t1 = now_ns();
        pthread_mutex_unlock(&G.frame_lock);
        pthread_mutex_lock(&G.gmz_lock);
        if (n > 0)
            gmz_submit_precompressed(G.conn, G.nlc_buf, (size_t)n, frame, field, vsync);
        pthread_mutex_unlock(&G.gmz_lock);
        uint64_t t2 = now_ns();
        enc_ns = t1 - t0;
        send_ns = t2 - t1;

        if (G.info.interlaced) field ^= 1;
        frame++;
        last_ok = now_ns();
    }
    return NULL;
}

bool mister_acquire(const char *host, const char *modeline, int near_lvl,
                    struct mister_info *info)
{
    pthread_mutex_lock(&G.lock);
    if (G.refs > 0) {
        G.refs++;
        *info = G.info;
        pthread_mutex_unlock(&G.lock);
        return true;
    }

    if (!host || !*host) host = getenv("MISTER_HOST");
    if (!host || !*host) {
        fprintf(stderr, "[mister] no host: set --vo-mister-host / MISTER_HOST\n");
        goto fail;
    }
    const struct ml *ml = NULL;
    if (!modeline || !*modeline) modeline = "640x480i60";
    for (size_t i = 0; i < sizeof(MODELINES) / sizeof(MODELINES[0]); i++)
        if (!strcmp(MODELINES[i].key, modeline)) ml = &MODELINES[i];
    if (!ml) {
        fprintf(stderr, "[mister] unknown modeline '%s'. Available:", modeline);
        for (size_t i = 0; i < sizeof(MODELINES) / sizeof(MODELINES[0]); i++)
            fprintf(stderr, " %s", MODELINES[i].key);
        fprintf(stderr, "\n");
        goto fail;
    }

    G.ml = ml;
    G.host = strdup(host);
    G.near_lvl = near_lvl < 0 ? 0 : near_lvl > 3 ? 3 : near_lvl;
    G.info.width = ml->ha;
    G.info.height = ml->va;
    G.info.interlaced = ml->interlace;
    double fr = ml->pclock * 1e6 / ((double)ml->ht * ml->vt);
    G.info.refresh = ml->interlace ? fr * 2 : fr;

    const int fh = ml->interlace ? ml->va / 2 : ml->va;
    const size_t fsz = (size_t)ml->ha * ml->va * 3;
    G.frame = calloc(1, fsz);
    G.field = calloc(1, (size_t)ml->ha * fh * 3);
    nlc_params np = nlc_p(ml->ha, fh, G.near_lvl);
    G.nlc_cap = nlc_max_encoded_size(&np);
    G.nlc_buf = malloc(G.nlc_cap);
    if (!G.frame || !G.field || !G.nlc_buf) goto fail_free;

    if (!open_conn()) goto fail_free;
    atomic_store(&G.stop, false);
    if (pthread_create(&G.thread, NULL, sender, NULL)) goto fail_conn;

    G.refs = 1;
    *info = G.info;
    pthread_mutex_unlock(&G.lock);
    return true;

fail_conn:
    gmz_disconnect(G.conn); G.conn = NULL;
fail_free:
    free(G.frame); free(G.field); free(G.nlc_buf); free(G.host);
    G.frame = G.field = G.nlc_buf = NULL; G.host = NULL;
fail:
    pthread_mutex_unlock(&G.lock);
    return false;
}

void mister_release(void)
{
    pthread_mutex_lock(&G.lock);
    if (G.refs > 0 && --G.refs == 0) {
        atomic_store(&G.stop, true);
        pthread_join(G.thread, NULL);
        pthread_mutex_lock(&G.gmz_lock);
        if (G.conn) gmz_disconnect(G.conn);
        G.conn = NULL;
        pthread_mutex_unlock(&G.gmz_lock);
        free(G.frame); free(G.field); free(G.nlc_buf); free(G.host);
        G.frame = G.field = G.nlc_buf = NULL; G.host = NULL;
    }
    pthread_mutex_unlock(&G.lock);
}

void mister_set_frame(const uint8_t *rgb, int stride)
{
    if (!G.frame) return;
    const size_t row = (size_t)G.info.width * 3;
    pthread_mutex_lock(&G.frame_lock);
    for (int y = 0; y < G.info.height; y++)
        memcpy(G.frame + y * row, rgb + (size_t)y * stride, row);
    pthread_mutex_unlock(&G.frame_lock);
}

void mister_push_audio(const void *pcm, size_t bytes)
{
    // Audio header + payload must not interleave with frame datagrams: same lock as submit.
    if (!G.conn) return;
    pthread_mutex_lock(&G.gmz_lock);
    if (G.conn) gmz_submit_audio(G.conn, pcm, bytes);
    pthread_mutex_unlock(&G.gmz_lock);
}
