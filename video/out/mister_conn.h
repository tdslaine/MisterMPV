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
void mister_release(void);

// Packed RGB24 frame, width*height*3 bytes at the given stride.
void mister_set_frame(const uint8_t *rgb, int stride);
// Raw 16-bit stereo PCM at 48 kHz.
void mister_push_audio(const void *pcm, size_t bytes);

#endif
