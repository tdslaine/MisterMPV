// CRT picture size/position calibration for the mister VO. Port of MiSTerCast's CrtGeometry:
// the picture is scaled to hsize/vsize percent of the output raster, centred, then shifted.
// For interlaced modes the raster is one *field*, so vertical units are field lines.
#include <math.h>
#include <string.h>

#include "mister_calib.h"

static int even_at_least(double x, int floor_)
{
    int v = (int)lround(x / 2.0) * 2;
    return v > floor_ ? v : floor_;
}

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void mister_picture_rect(double hsize, double vsize, int hoff, int voff,
                         int out_w, int out_h, struct mister_rect *r)
{
    hsize = round(clampd(hsize, 80.0, 100.0) * 10.0) / 10.0;
    vsize = round(clampd(vsize, 80.0, 100.0) * 10.0) / 10.0;
    hoff = clampi(hoff, -72, 72);
    voff = clampi(voff, -28, 28);
    r->w = even_at_least(out_w * hsize / 100.0, 2);
    if (r->w > out_w) r->w = out_w;
    r->h = even_at_least(out_h * vsize / 100.0, 2);
    if (r->h > out_h) r->h = out_h;
    r->x = (out_w - r->w) / 2 + hoff;
    r->y = (out_h - r->h) / 2 + voff;
}

struct rgb { uint8_t r, g, b; };
static const struct rgb WHITE = {235, 235, 235};
static const struct rgb GRID = {112, 112, 112};
static const struct rgb ACTION = {235, 200, 0};
static const struct rgb TITLE = {0, 200, 235};
#define COLS 16
#define ROWS 12
#define LINEW 2

struct canvas { uint8_t *p; int w, h; int rgb; };

static void fill(const struct canvas *c, int x, int y, int fw, int fh, struct rgb col)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + fw > c->w ? c->w : x + fw, y1 = y + fh > c->h ? c->h : y + fh;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) {
            uint8_t *d = c->p + ((size_t)yy * c->w + xx) * 3;
            if (c->rgb) { d[0] = col.r; d[1] = col.g; d[2] = col.b; }
            else        { d[0] = col.b; d[1] = col.g; d[2] = col.r; }   // default wire order is B,G,R
        }
}

void mister_render_calibration(uint8_t *bgr, int out_w, int out_h, const struct mister_rect *r,
                               int rgb_order)
{
    memset(bgr, 0, (size_t)out_w * out_h * 3);
    if (r->w <= 0 || r->h <= 0) return;
    const struct canvas c = {bgr, out_w, out_h, rgb_order};
#define PX(f) (r->x + (int)((f) * r->w + 0.5))
#define PY(f) (r->y + (int)((f) * r->h + 0.5))

    for (int i = 1; i < COLS; i++) {
        int x = PX((double)i / COLS);
        fill(&c, x - LINEW / 2, r->y, LINEW, r->h, GRID);
    }
    for (int j = 1; j < ROWS; j++)
        fill(&c, r->x, PY((double)j / ROWS), r->w, 1, GRID);

    const double insets[] = {0.025, 0.05, 0.0};
    const struct rgb cols[] = {ACTION, TITLE, WHITE};
    for (int k = 0; k < 3; k++) {   // 95 % action-safe, 90 % title-safe, outer edge last
        const int x0 = PX(insets[k]), y0 = PY(insets[k]);
        const int x1 = PX(1 - insets[k]), y1 = PY(1 - insets[k]);
        fill(&c, x0, y0, LINEW, y1 - y0, cols[k]);
        fill(&c, x1 - LINEW, y0, LINEW, y1 - y0, cols[k]);
        fill(&c, x0, y0, x1 - x0, 1, cols[k]);
        fill(&c, x0, y1 - 1, x1 - x0, 1, cols[k]);
    }

    const int cx = PX(0.5), cy = PY(0.5);
    fill(&c, cx - LINEW / 2, PY(0.5 - 2.0 / ROWS), LINEW, PY(0.5 + 2.0 / ROWS) - PY(0.5 - 2.0 / ROWS), WHITE);
    fill(&c, PX(0.5 - 2.0 / COLS), cy, PX(0.5 + 2.0 / COLS) - PX(0.5 - 2.0 / COLS), 1, WHITE);

    // Circle 0.8 of the picture height across (0.6 of the width in 4:3).
    const double ccx = r->x + 0.5 * r->w, ccy = r->y + 0.5 * r->h;
    const double rx = 0.3 * r->w, ry = 0.4 * r->h;
    int y0 = (int)(ccy - ry) - 1, y1 = (int)(ccy + ry) + 2;
    int x0 = (int)(ccx - rx) - 1, x1 = (int)(ccx + rx) + 2;
    if (y0 < 0) y0 = 0;
    if (x0 < 0) x0 = 0;
    if (y1 > out_h) y1 = out_h;
    if (x1 > out_w) x1 = out_w;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            const double fx = x + 0.5, fy = y + 0.5;
            double dx = (fx - ccx) / rx, dy = (fy - ccy) / ry;
            const double ix = rx - LINEW, iy = ry - 1;
            int in_outer = dx * dx + dy * dy <= 1.0;
            int in_inner = 0;
            if (ix > 0 && iy > 0) {
                dx = (fx - ccx) / ix; dy = (fy - ccy) / iy;
                in_inner = dx * dx + dy * dy <= 1.0;
            }
            if (in_outer && !in_inner)
                fill(&c, x, y, 1, 1, WHITE);
        }
#undef PX
#undef PY
}
