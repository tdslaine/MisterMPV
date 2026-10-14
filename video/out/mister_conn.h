#ifndef MPV_MISTER_CONN_H
#define MPV_MISTER_CONN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Shared GroovyNLC connection used by vo_mister and ao_mister. The first caller of
// mister_acquire() decides host/modeline/near; later callers share the connection.
// A sender thread repeats the last frame at the output rate (like the MiSTerCast engine).
struct mister_info {
    int width;          // active width
    int height;         // full frame height (vactive)
    bool interlaced;
    double refresh;     // fields per second
};

bool mister_acquire(const char *host, const char *modeline, int near_lvl,
                    struct mister_info *info);
// Reconnect with another modeline key (e.g. "320x240p60"). Returns false on failure; the
// previous output is then gone, so the caller should treat that as fatal. Idempotent.
bool mister_switch(const char *modeline, struct mister_info *info);
void mister_release(void);

// Picture adjustments applied to every frame the sender transmits (live, also while paused).
// Same maths as MiSTerCast's applyColorFx. brightness/contrast/saturation use mpv's
// -100..100 scale (0 = neutral); black 0..30 lifts blacks; scanlines 0..100 % darkens odd rows.
// scanstyle: 0 = dark odd rows (1:2), 1 = dark every 3rd row, 2 = dark odd rows + bright even rows,
// 3 = RGB phosphor mask (columns).
void mister_set_fx(double brightness, double contrast, double saturation, int black, int scanlines,
                   int scanstyle);

// Global options --mister-blacklevel / --mister-scanlines (registered in options.c so they are
// settable at runtime over IPC like brightness/contrast/saturation).
struct m_sub_options;
struct mister_fx_opts {
    int blacklevel;
    int scanlines;
    int scanstyle;
    double hsize, vsize;      // CRT picture size, % of the raster (80..100)
    int hoffset, voffset;     // CRT picture shift: pixels / raster lines (field lines when interlaced)
    bool calibrate;           // show the calibration test pattern instead of the video
};
extern const struct m_sub_options mister_fx_conf;

// Interlaced picture tweaks, safe to call at any time (before or after mister_acquire):
// bff: send the bottom field first; flicker: 0=off, 1=light (1-6-1), 2=full (1-2-1)
// vertical low-pass against 1-line interlace flicker. No effect on progressive modes.
void mister_set_interlace(int bff, int flicker);

// Packed RGB24 frame, width*height*3 bytes at the given stride.
void mister_set_frame(const uint8_t *rgb, int stride);
// Raw 16-bit stereo PCM at 48 kHz.
void mister_push_audio(const void *pcm, size_t bytes);

#endif
