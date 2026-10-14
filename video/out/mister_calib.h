#ifndef MPV_MISTER_CALIB_H
#define MPV_MISTER_CALIB_H

#include <stdint.h>

struct mister_rect { int x, y, w, h; };   // x/y may be negative, x+w / y+h may exceed the raster

// Where the picture lands in an out_w x out_h raster (see CrtGeometry in MiSTerCast).
// hsize/vsize 80..100 %, hoff -72..72 px, voff -28..28 raster lines.
void mister_picture_rect(double hsize, double vsize, int hoff, int voff,
                         int out_w, int out_h, struct mister_rect *r);

// Draw the calibration pattern (packed B,G,R) for `r` into an out_w x out_h raster.
void mister_render_calibration(uint8_t *bgr, int out_w, int out_h, const struct mister_rect *r);

#endif
