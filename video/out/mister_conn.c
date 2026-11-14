// Shared GroovyNLC (MiSTer FPGA) connection for the mister VO/AO.
// Frame encoding and the wire format are identical to CastEngineGMZ in our MiSTerCast for Linux
// (sibling project, same author).
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mister_conn.h"
#include "options/m_option.h"
#include "options/m_config.h"
#include "options/options.h"
#include <math.h>
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
    uint8_t *fx_buf;             // field/frame after apply_fx
    uint8_t *nlc_buf;
    size_t nlc_cap;
    pthread_t thread;
    atomic_bool stop;
    atomic_int bff;              // 1 = bottom field first
    atomic_int flicker;          // 0 off, 1 light, 2 full
    atomic_int rgb_order;
    atomic_int fx_bri, fx_con, fx_sat, fx_blk, fx_scn, fx_scs;  // levels, x100, x100, levels, percent
    pthread_mutex_t shift_lock;  // guards shift_spec
    char shift_spec[1024];       // "key:h:v,key:h:v" sync position fine-tune per modeline
    atomic_bool shift_dirty;     // re-send the modeline from the sender thread
} G = { .lock = PTHREAD_MUTEX_INITIALIZER, .gmz_lock = PTHREAD_MUTEX_INITIALIZER,
        .frame_lock = PTHREAD_MUTEX_INITIALIZER, .shift_lock = PTHREAD_MUTEX_INITIALIZER,
        .fx_con = 100, .fx_sat = 100 };

#define OPT_BASE_STRUCT struct mister_fx_opts
const struct m_sub_options mister_fx_conf = {
    .opts = (const m_option_t[]) {
        {"mister-blacklevel", OPT_INT(blacklevel), M_RANGE(0, 30)},
        {"mister-scanlines", OPT_INT(scanlines), M_RANGE(0, 100)},
        {"mister-scanstyle", OPT_INT(scanstyle), M_RANGE(0, 3)},
        {"mister-hsize", OPT_DOUBLE(hsize), M_RANGE(80, 100)},
        {"mister-vsize", OPT_DOUBLE(vsize), M_RANGE(80, 100)},
        {"mister-hoffset", OPT_INT(hoffset), M_RANGE(-72, 72)},
        {"mister-voffset", OPT_INT(voffset), M_RANGE(-28, 28)},
        {"mister-calibrate", OPT_BOOL(calibrate)},
        {"mister-sync-shift", OPT_STRING(sync_shift)},
        {0}
    },
    .defaults = &(const struct mister_fx_opts){ .hsize = 100, .vsize = 100 },
    .size = sizeof(struct mister_fx_opts),
    .change_flags = UPDATE_VIDEO,
};

void mister_set_fx(double brightness, double contrast, double saturation, int black, int scanlines,
                   int scanstyle)
{
    atomic_store(&G.fx_scs, scanstyle);
    atomic_store(&G.fx_bri, (int)lround(brightness * 1.28));          // +-100 -> +-128 levels
    atomic_store(&G.fx_con, (int)lround(contrast + 100.0));           // x100 factor
    atomic_store(&G.fx_sat, (int)lround(saturation + 100.0));         // x100 factor
    atomic_store(&G.fx_blk, black);
    atomic_store(&G.fx_scn, scanlines);
}

static inline uint8_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

// Colour/scanline pass: copies the w*h RGB24 image src -> dst.
static void apply_fx(uint8_t *dst, const uint8_t *src, int w, int h)
{
    static uint8_t lut[256];
    static int lut_key[3] = {-1, -1, -1};
    const int bri = atomic_load(&G.fx_bri), con = atomic_load(&G.fx_con);
    const int blk = atomic_load(&G.fx_blk), sat = atomic_load(&G.fx_sat);
    const int scn = atomic_load(&G.fx_scn);
    const int scs = atomic_load(&G.fx_scs);
    if (lut_key[0] != bri || lut_key[1] != con || lut_key[2] != blk) {
        for (int v = 0; v < 256; v++) {          // black level -> brightness -> contrast
            int x = clamp8(v + blk);
            x = clamp8(x + bri);
            x = clamp8((int)(((x - 128) * (con / 100.0f)) + 128.5f));
            lut[v] = (uint8_t)x;
        }
        lut_key[0] = bri; lut_key[1] = con; lut_key[2] = blk;
    }
    const float sf = sat / 100.0f;
    const float dark = scs == 3 ? 1.0f - scn / 150.0f : 1.0f - scn / 100.0f;
    const float boost = 1.0f + scn / 100.0f * 0.35f;
    for (int y = 0; y < h; y++) {
        const uint8_t *s = src + (size_t)y * w * 3;
        uint8_t *d = dst + (size_t)y * w * 3;
        // rows: styles 0-2 darken a row, style 2 also lifts the other rows
        bool line = false, lift = false;
        if (scn > 0) {
            if (scs == 0 || scs == 2) { line = y & 1; lift = scs == 2 && !(y & 1); }
            else if (scs == 1) line = (y % 3) == 2;
        }
        for (int x = 0; x < w; x++, s += 3, d += 3) {
            const int ro = atomic_load(&G.rgb_order);
            int b = lut[s[ro ? 2 : 0]], g = lut[s[1]], r = lut[s[ro ? 0 : 2]];   // default wire order is B,G,R
            if (sat != 100) {
                const float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                r = clamp8((int)(l + (r - l) * sf));
                g = clamp8((int)(l + (g - l) * sf));
                b = clamp8((int)(l + (b - l) * sf));
            }
            if (line) { r = (int)(r * dark); g = (int)(g * dark); b = (int)(b * dark); }
            else if (lift) { r = clamp8((int)(r * boost)); g = clamp8((int)(g * boost)); b = clamp8((int)(b * boost)); }
            else if (scn > 0 && scs == 3) {   // each column favours one phosphor colour
                const int c = x % 3;
                if (c != 0) r = (int)(r * dark);
                if (c != 1) g = (int)(g * dark);
                if (c != 2) b = (int)(b * dark);
            }
            d[ro ? 2 : 0] = (uint8_t)b; d[1] = (uint8_t)g; d[ro ? 0 : 2] = (uint8_t)r;
        }
    }
}

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

void mister_set_sync_shift(const char *spec)
{
    if (!spec) spec = "";
    pthread_mutex_lock(&G.shift_lock);
    if (strcmp(spec, G.shift_spec)) {
        snprintf(G.shift_spec, sizeof(G.shift_spec), "%s", spec);
        atomic_store(&G.shift_dirty, true);
    }
    pthread_mutex_unlock(&G.shift_lock);
}

// Sync position fine-tune for `key` from "key:h:v,..." (h pixels, v lines; + = picture right/down).
static void lookup_shift(const char *key, int *dh, int *dv)
{
    *dh = *dv = 0;
    pthread_mutex_lock(&G.shift_lock);
    const size_t kl = strlen(key);
    for (const char *e = G.shift_spec; e && *e; e = strchr(e, ',') ? strchr(e, ',') + 1 : NULL) {
        int h, v;
        if (!strncmp(e, key, kl) && e[kl] == ':' && sscanf(e + kl + 1, "%d:%d", &h, &v) == 2) {
            *dh = h; *dv = v;
            break;
        }
    }
    pthread_mutex_unlock(&G.shift_lock);
}

static int clampi_(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// The table modeline with the sync pulse moved by the fine-tune shift. Pulse width, totals and
// pixel clock stay as they are, so line/field rates do not change; the pulse is kept inside the
// blanking (same rule as MiSTerCast's modeline nudge). Moving the pulse later moves the picture
// the other way on the CRT, hence the minus.
static void fill_gm(const struct ml *m, gmz_modeline_t *gm, int *rdh, int *rdv)
{
    int dh, dv;
    lookup_shift(m->key, &dh, &dv);
    const int hw = m->he - m->hb, vw = m->ve - m->vb;
    const int hb = clampi_(m->hb - dh, m->ha + 2, m->ht - hw - 2);
    const int vb = clampi_(m->vb - dv, m->va + 1, m->vt - vw - 1);
    memset(gm, 0, sizeof(*gm));
    gm->pixel_clock = m->pclock;
    gm->h_active = m->ha; gm->h_begin = hb; gm->h_end = hb + hw; gm->h_total = m->ht;
    gm->v_active = m->va; gm->v_begin = vb; gm->v_end = vb + vw; gm->v_total = m->vt;
    gm->interlaced = m->interlace;
    if (rdh) *rdh = m->hb - hb;
    if (rdv) *rdv = m->vb - vb;
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
    int dh, dv;
    atomic_store(&G.shift_dirty, false);
    fill_gm(m, &gm, &dh, &dv);
    gmz_set_modeline(G.conn, &gm);
    fprintf(stderr, "[mister] connected (%s), modeline %s, sync shift %+d px %+d lines\n",
            gmz_version(), m->key, dh, dv);
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
    int bff = atomic_load(&G.bff);
    uint8_t field = bff ? 1 : 0;
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
        if (atomic_exchange(&G.shift_dirty, false)) {   // live sync position fine-tune
            gmz_modeline_t gm;
            int dh, dv;
            fill_gm(G.ml, &gm, &dh, &dv);
            pthread_mutex_lock(&G.gmz_lock);
            gmz_set_modeline(G.conn, &gm);
            pthread_mutex_unlock(&G.gmz_lock);
            fprintf(stderr, "[mister] modeline %s, sync shift %+d px %+d lines\n", G.ml->key, dh, dv);
        }

        const uint8_t *src;
        pthread_mutex_lock(&G.frame_lock);
        if (G.info.interlaced) {
            const int nb = atomic_load(&G.bff);
            if (nb != bff) { bff = nb; field ^= 1; }
            const int fl = atomic_load(&G.flicker);
            const int fh = G.info.height;
            for (int y = 0; y < h; y++) {
                const int r = y * 2 + field;
                const uint8_t *mid = G.frame + (size_t)r * row;
                uint8_t *dst = G.field + y * row;
                if (!fl) { memcpy(dst, mid, row); continue; }
                // vertical 1-6-1 / 1-2-1 across the full frame (mirrored at the edges)
                const uint8_t *up = G.frame + (size_t)(r > 0 ? r - 1 : r + 1) * row;
                const uint8_t *dn = G.frame + (size_t)(r < fh - 1 ? r + 1 : r - 1) * row;
                if (fl == 1)
                    for (size_t i = 0; i < row; i++)
                        dst[i] = (uint8_t)((up[i] + 6 * mid[i] + dn[i] + 4) >> 3);
                else
                    for (size_t i = 0; i < row; i++)
                        dst[i] = (uint8_t)((up[i] + 2 * mid[i] + dn[i] + 2) >> 2);
            }
            src = G.field;
        } else {
            src = G.frame;
        }
        if (atomic_load(&G.fx_bri) || atomic_load(&G.fx_con) != 100 || atomic_load(&G.fx_sat) != 100 ||
            atomic_load(&G.fx_blk) || atomic_load(&G.fx_scn)) {
            apply_fx(G.fx_buf, src, w, h);
            src = G.fx_buf;
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

static const struct ml *find_ml(const char *key)
{
    for (size_t i = 0; i < sizeof(MODELINES) / sizeof(MODELINES[0]); i++)
        if (!strcmp(MODELINES[i].key, key)) return &MODELINES[i];
    return NULL;
}

static void list_modelines(void)
{
    fprintf(stderr, "[mister] available modelines:");
    for (size_t i = 0; i < sizeof(MODELINES) / sizeof(MODELINES[0]); i++)
        fprintf(stderr, " %s", MODELINES[i].key);
    fprintf(stderr, "\n");
}

// Allocate buffers, connect, start the sender. Caller holds G.lock. Needs G.host/G.near_lvl.
static bool start_locked(const struct ml *ml)
{
    G.ml = ml;
    G.info.width = ml->ha;
    G.info.height = ml->va;
    G.info.interlaced = ml->interlace;
    double fr = ml->pclock * 1e6 / ((double)ml->ht * ml->vt);
    G.info.refresh = ml->interlace ? fr * 2 : fr;

    const int fh = ml->interlace ? ml->va / 2 : ml->va;
    G.frame = calloc(1, (size_t)ml->ha * ml->va * 3);
    G.field = calloc(1, (size_t)ml->ha * fh * 3);
    G.fx_buf = calloc(1, (size_t)ml->ha * fh * 3);
    nlc_params np = nlc_p(ml->ha, fh, G.near_lvl);
    G.nlc_cap = nlc_max_encoded_size(&np);
    G.nlc_buf = malloc(G.nlc_cap);
    if (!G.frame || !G.field || !G.fx_buf || !G.nlc_buf) goto fail;

    pthread_mutex_lock(&G.gmz_lock);
    bool ok = open_conn();
    pthread_mutex_unlock(&G.gmz_lock);
    if (!ok) goto fail;

    atomic_store(&G.stop, false);
    if (pthread_create(&G.thread, NULL, sender, NULL)) {
        pthread_mutex_lock(&G.gmz_lock);
        gmz_disconnect(G.conn); G.conn = NULL;
        pthread_mutex_unlock(&G.gmz_lock);
        goto fail;
    }
    return true;
fail:
    free(G.frame); free(G.field); free(G.fx_buf); free(G.nlc_buf);
    G.frame = G.field = G.fx_buf = G.nlc_buf = NULL;
    return false;
}

static void stop_locked(void)
{
    atomic_store(&G.stop, true);
    pthread_join(G.thread, NULL);
    pthread_mutex_lock(&G.gmz_lock);
    if (G.conn) gmz_disconnect(G.conn);
    G.conn = NULL;
    pthread_mutex_unlock(&G.gmz_lock);
    free(G.frame); free(G.field); free(G.fx_buf); free(G.nlc_buf);
    G.frame = G.field = G.fx_buf = G.nlc_buf = NULL;
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
    // "auto" starts on 640x480i60; the VO switches once it knows the source.
    if (!modeline || !*modeline || !strcmp(modeline, "auto")) modeline = "640x480i60";
    const struct ml *ml = find_ml(modeline);
    if (!ml) {
        fprintf(stderr, "[mister] unknown modeline '%s'\n", modeline);
        list_modelines();
        goto fail;
    }

    G.host = strdup(host);
    G.near_lvl = near_lvl < 0 ? 0 : near_lvl > 3 ? 3 : near_lvl;
    if (!start_locked(ml)) { free(G.host); G.host = NULL; goto fail; }

    G.refs = 1;
    *info = G.info;
    pthread_mutex_unlock(&G.lock);
    return true;
fail:
    pthread_mutex_unlock(&G.lock);
    return false;
}

bool mister_switch(const char *modeline, struct mister_info *info)
{
    bool ok = false;
    pthread_mutex_lock(&G.lock);
    const struct ml *ml = G.refs > 0 ? find_ml(modeline) : NULL;
    if (ml == G.ml) { *info = G.info; ok = ml != NULL; goto out; }
    if (!ml) goto out;
    stop_locked();
    ok = start_locked(ml);
    if (ok) {
        *info = G.info;
    } else {
        fprintf(stderr, "[mister] switch to %s failed, output is down\n", modeline);
    }
out:
    pthread_mutex_unlock(&G.lock);
    return ok;
}

void mister_set_rgb_order(int rgb) { atomic_store(&G.rgb_order, rgb ? 1 : 0); }
int mister_rgb_order(void) { return atomic_load(&G.rgb_order); }

void mister_set_interlace(int bff, int flicker)
{
    atomic_store(&G.bff, bff ? 1 : 0);
    atomic_store(&G.flicker, flicker < 0 ? 0 : flicker > 2 ? 2 : flicker);
}

void mister_release(void)
{
    pthread_mutex_lock(&G.lock);
    if (G.refs > 0 && --G.refs == 0) {
        stop_locked();
        free(G.host); G.host = NULL; G.ml = NULL;
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
