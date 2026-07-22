/* gfx_shim — flat handle API over blend2d (see gfx_shim.h). Compiled as C by
   vytoc; links against the prebuilt libblend2d in native/<platform>/. */
#include "gfx_shim.h"

#include <blend2d/blend2d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GFX_WEIGHTS 3   /* 0=regular, 1=medium/semi, 2=bold */

struct GfxCanvas {
    BLImageCore img;
    BLContextCore ctx;
    BLFontFaceCore face[GFX_WEIGHTS];
    BLFontCore font[GFX_WEIGHTS];
    int has_font[GFX_WEIGHTS];
    int active_weight;
    int w, h;
    /* Scratch layer for shadow rasterisation, kept across calls: creating a
       BLImage + BLContext per shadow dominated frame time (~5ms each). Grows
       to the largest shadow seen and is never shrunk. */
    BLImageCore sl_img;
    BLContextCore sl_ctx;
    int sl_w, sl_h;
};

/* Resolve a 0xAARRGGBB color. Alpha means alpha: 0 is transparent. (This used
   to remap 0 to opaque so bare 0xRRGGBB literals kept working, which made a
   fully transparent fill inexpressible — see the model note in surface.vt.) */
static uint32_t rgba32_of(int color) {
    return (uint32_t)color;
}

/* silently keep the legacy `opaque()` helper for any user of the header */
static uint32_t opaque(int rgb) { return 0xFF000000u | ((uint32_t)rgb & 0xFFFFFFu); }

GfxCanvas *gfx_canvas_new(int w, int h) {
    /* clamp instead of returning NULL: most gfx_* entry points deref the
       handle unchecked, so a 0-sized window during resize must still yield
       a live (1x1) canvas rather than a null-deref time bomb */
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    GfxCanvas *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->w = w;
    c->h = h;
    c->active_weight = 0;
    if (bl_image_init_as(&c->img, w, h, BL_FORMAT_PRGB32) != BL_SUCCESS) { free(c); return NULL; }
    if (bl_context_init_as(&c->ctx, &c->img, NULL) != BL_SUCCESS) {
        bl_image_destroy(&c->img);
        free(c);
        return NULL;
    }
    return c;
}

static void destroy_fonts(GfxCanvas *c) {
    for (int i = 0; i < GFX_WEIGHTS; i++) {
        if (c->has_font[i]) {
            bl_font_destroy(&c->font[i]);
            bl_font_face_destroy(&c->face[i]);
            c->has_font[i] = 0;
        }
    }
}

static void free_shadow_layer(GfxCanvas *c) {
    if (c->sl_w > 0) {
        bl_context_destroy(&c->sl_ctx);
        bl_image_destroy(&c->sl_img);
        c->sl_w = 0;
        c->sl_h = 0;
    }
}

void gfx_canvas_free(GfxCanvas *c) {
    if (!c) return;
    free_shadow_layer(c);
    destroy_fonts(c);
    bl_context_destroy(&c->ctx);
    bl_image_destroy(&c->img);
    free(c);
}

int gfx_width(GfxCanvas *c) { return c ? c->w : 0; }
int gfx_height(GfxCanvas *c) { return c ? c->h : 0; }

void gfx_clear(GfxCanvas *c, int color) {
    bl_context_set_fill_style_rgba32(&c->ctx, rgba32_of(color));
    bl_context_fill_all(&c->ctx);
}

void gfx_fill_rect(GfxCanvas *c, double x, double y, double w, double h, int color) {
    BLRect r = { x, y, w, h };
    bl_context_set_fill_style_rgba32(&c->ctx, rgba32_of(color));
    bl_context_fill_geometry(&c->ctx, BL_GEOMETRY_TYPE_RECTD, &r);
}

void gfx_fill_round_rect(GfxCanvas *c, double x, double y, double w, double h, double r, int color) {
    if (r < 0.0) r = 0.0;
    BLRoundRect rr = { x, y, w, h, r, r };
    bl_context_set_fill_style_rgba32(&c->ctx, rgba32_of(color));
    bl_context_fill_geometry(&c->ctx, BL_GEOMETRY_TYPE_ROUND_RECT, &rr);
}

void gfx_fill_circle(GfxCanvas *c, double cx, double cy, double radius, int color) {
    BLCircle ci = { cx, cy, radius };
    bl_context_set_fill_style_rgba32(&c->ctx, rgba32_of(color));
    bl_context_fill_geometry(&c->ctx, BL_GEOMETRY_TYPE_CIRCLE, &ci);
}

void gfx_stroke_line(GfxCanvas *c, double x0, double y0, double x1, double y1, double width, int color) {
    BLLine ln = { x0, y0, x1, y1 };
    bl_context_set_stroke_width(&c->ctx, width);
    bl_context_stroke_geometry_rgba32(&c->ctx, BL_GEOMETRY_TYPE_LINE, &ln, rgba32_of(color));
}

void gfx_stroke_round_rect(GfxCanvas *c, double x, double y, double w, double h, double r, double width, int color) {
    if (r < 0.0) r = 0.0;
    BLRoundRect rr = { x, y, w, h, r, r };
    bl_context_set_stroke_width(&c->ctx, width);
    bl_context_stroke_geometry_rgba32(&c->ctx, BL_GEOMETRY_TYPE_ROUND_RECT, &rr, rgba32_of(color));
}

void gfx_stroke_circle(GfxCanvas *c, double cx, double cy, double radius, double width, int color) {
    BLCircle ci = { cx, cy, radius };
    bl_context_set_stroke_width(&c->ctx, width);
    bl_context_stroke_geometry_rgba32(&c->ctx, BL_GEOMETRY_TYPE_CIRCLE, &ci, rgba32_of(color));
}

void gfx_linear_gradient_rect(GfxCanvas *c, double x, double y, double w, double h,
                              double x0, double y0, double x1, double y1, int c0, int c1) {
    BLGradientCore g;
    BLLinearGradientValues lv = { x0, y0, x1, y1 };
    bl_gradient_init_as(&g, BL_GRADIENT_TYPE_LINEAR, &lv, BL_EXTEND_MODE_PAD, NULL, 0, NULL);
    bl_gradient_add_stop_rgba32(&g, 0.0, rgba32_of(c0));
    bl_gradient_add_stop_rgba32(&g, 1.0, rgba32_of(c1));
    BLRect r = { x, y, w, h };
    bl_context_fill_geometry_ext(&c->ctx, BL_GEOMETRY_TYPE_RECTD, &r, (const BLUnknown *)&g);
    bl_gradient_destroy(&g);
}

void gfx_linear_gradient_rect_n(GfxCanvas *c, double x, double y, double w, double h,
                                double x0, double y0, double x1, double y1,
                                const int *colors, const double *positions, int n) {
    if (n <= 0) return;
    BLGradientCore g;
    BLLinearGradientValues lv = { x0, y0, x1, y1 };
    bl_gradient_init_as(&g, BL_GRADIENT_TYPE_LINEAR, &lv, BL_EXTEND_MODE_PAD, NULL, 0, NULL);
    for (int i = 0; i < n; i++) {
        bl_gradient_add_stop_rgba32(&g, positions[i], rgba32_of(colors[i]));
    }
    BLRect r = { x, y, w, h };
    bl_context_fill_geometry_ext(&c->ctx, BL_GEOMETRY_TYPE_RECTD, &r, (const BLUnknown *)&g);
    bl_gradient_destroy(&g);
}

/* Straight-edge bevel: no arc primitive is used, so the four strokes are
   inset by `radius` off each corner (matching a round-rect's flat-edge
   span) rather than following the curve — reads fine at typical UI radii
   (2-4px), and stays dependency-free. */
void gfx_bevel_round(GfxCanvas *c, double x, double y, double w, double h,
                     double radius, double width, int light, int dark, int raised) {
    if (radius < 0.0) radius = 0.0;
    uint32_t top_left_color = raised ? rgba32_of(light) : rgba32_of(dark);
    uint32_t bot_right_color = raised ? rgba32_of(dark) : rgba32_of(light);
    double hw = width / 2.0;
    bl_context_set_stroke_width(&c->ctx, width);
    BLLine top = { x + radius, y + hw, x + w - radius, y + hw };
    bl_context_stroke_geometry_rgba32(&c->ctx, BL_GEOMETRY_TYPE_LINE, &top, top_left_color);
    BLLine left = { x + hw, y + radius, x + hw, y + h - radius };
    bl_context_stroke_geometry_rgba32(&c->ctx, BL_GEOMETRY_TYPE_LINE, &left, top_left_color);
    BLLine bottom = { x + radius, y + h - hw, x + w - radius, y + h - hw };
    bl_context_stroke_geometry_rgba32(&c->ctx, BL_GEOMETRY_TYPE_LINE, &bottom, bot_right_color);
    BLLine right = { x + w - hw, y + radius, x + w - hw, y + h - radius };
    bl_context_stroke_geometry_rgba32(&c->ctx, BL_GEOMETRY_TYPE_LINE, &right, bot_right_color);
}

/* Arbitrary filled polygon — for icon glyphs a rect/round-rect/circle can't
   express (stars, hearts, roofs, speech-bubble tails). blend2d's C API has
   no C++ BLArrayView<T> template to reach for; in plain C it's the untyped
   `{ const void* data; size_t size; }` the BL_DEFINE_ARRAY_VIEW macro
   expands to (core/api.h) — build a BLPoint[] and hand blend2d that. */
void gfx_fill_polygon(GfxCanvas *c, const double *xs, const double *ys, int n, int color) {
    if (n < 3) return;
    BLPoint *pts = (BLPoint *)malloc(sizeof(BLPoint) * (size_t)n);
    if (!pts) return;
    for (int i = 0; i < n; i++) {
        pts[i].x = xs[i];
        pts[i].y = ys[i];
    }
    BLArrayView view;
    view.data = pts;
    view.size = (size_t)n;
    bl_context_set_fill_style_rgba32(&c->ctx, rgba32_of(color));
    bl_context_fill_geometry(&c->ctx, BL_GEOMETRY_TYPE_POLYGOND, &view);
    free(pts);
}

/* Partial ring stroke (a wifi/signal-style arc) via blend2d's BLArc +
   BL_GEOMETRY_TYPE_ARC. Angles are degrees at this boundary (0 = +x axis,
   clockwise) and converted to radians here — every other angle-free
   primitive in this shim already favors plain, ergonomic units at the
   Vyto-facing edge. */
void gfx_stroke_arc(GfxCanvas *c, double cx, double cy, double rx, double ry,
                    double start_deg, double sweep_deg, double width, int color) {
    const double deg2rad = 3.14159265358979323846 / 180.0;
    BLArc a = { cx, cy, rx, ry, start_deg * deg2rad, sweep_deg * deg2rad };
    bl_context_set_stroke_width(&c->ctx, width);
    bl_context_stroke_geometry_rgba32(&c->ctx, BL_GEOMETRY_TYPE_ARC, &a, rgba32_of(color));
}

/* ---- blur -----------------------------------------------------------------
   Separable box blur, three passes, on premultiplied BGRA. Premultiplication
   is what makes this correct to run channel-independently: colour is already
   scaled by alpha, so averaging cannot bleed the colour of transparent pixels
   into their neighbours (the classic dark-halo artifact).

   Each pass is a running sum, so cost is O(pixels) regardless of radius. */

static void box_blur_h(uint8_t *p, intptr_t stride, int x0, int y0, int w, int h, int rad) {
    if (rad < 1 || w <= 1) return;
    int win = rad * 2 + 1;
    /* Multiply by a fixed-point reciprocal instead of dividing per sample --
       the divide was the hottest instruction in the blur. 24 fractional bits
       with round-to-nearest reproduces an exact divide over the whole input
       range (verified exhaustively for every odd window up to 121); 16 bits
       drifts by a level. The product is computed in 64-bit because 255<<24
       sits just under the 32-bit ceiling. */
    uint64_t recip = (1ull << 24) / (uint64_t)win;
    uint8_t *row_copy = (uint8_t *)malloc((size_t)w * 4);
    if (!row_copy) return;
    for (int y = 0; y < h; y++) {
        uint8_t *row = p + (intptr_t)(y0 + y) * stride + (intptr_t)x0 * 4;
        memcpy(row_copy, row, (size_t)w * 4);
        for (int ch = 0; ch < 4; ch++) {
            int sum = row_copy[ch] * (rad + 1);
            for (int i = 1; i <= rad; i++) sum += row_copy[(i < w ? i : w - 1) * 4 + ch];
            for (int x = 0; x < w; x++) {
                row[x * 4 + ch] = (uint8_t)(((uint64_t)sum * recip + (1ull << 23)) >> 24);
                int add = row_copy[(x + rad + 1 < w ? x + rad + 1 : w - 1) * 4 + ch];
                int sub = row_copy[(x - rad > 0 ? x - rad : 0) * 4 + ch];
                sum += add - sub;
            }
        }
    }
    free(row_copy);
}

/* Vertical pass over a strip of columns at a time. Walking one column at a
   time touched a new cache line per pixel; a strip keeps each row's span
   contiguous, which is what makes this pass affordable. */
#define BLUR_STRIP 16

static void box_blur_v(uint8_t *p, intptr_t stride, int x0, int y0, int w, int h, int rad) {
    if (rad < 1 || h <= 1) return;
    int win = rad * 2 + 1;
    uint64_t recip = (1ull << 24) / (uint64_t)win;
    int strip_bytes = BLUR_STRIP * 4;
    uint8_t *colbuf = (uint8_t *)malloc((size_t)h * (size_t)strip_bytes);
    int *sum = (int *)malloc((size_t)strip_bytes * sizeof(int));
    if (!colbuf || !sum) { free(colbuf); free(sum); return; }

    for (int xs = 0; xs < w; xs += BLUR_STRIP) {
        int nc = (w - xs < BLUR_STRIP) ? (w - xs) : BLUR_STRIP;
        int nb = nc * 4;                       /* bytes of interest per row */
        uint8_t *base = p + (intptr_t)y0 * stride + (intptr_t)(x0 + xs) * 4;
        for (int y = 0; y < h; y++)
            memcpy(colbuf + (size_t)y * strip_bytes, base + (intptr_t)y * stride, (size_t)nb);

        for (int b = 0; b < nb; b++) {
            int acc = colbuf[b] * (rad + 1);
            for (int i = 1; i <= rad; i++)
                acc += colbuf[(size_t)(i < h ? i : h - 1) * strip_bytes + b];
            sum[b] = acc;
        }
        for (int y = 0; y < h; y++) {
            uint8_t *dst = base + (intptr_t)y * stride;
            const uint8_t *addr = colbuf + (size_t)(y + rad + 1 < h ? y + rad + 1 : h - 1) * strip_bytes;
            const uint8_t *subr = colbuf + (size_t)(y - rad > 0 ? y - rad : 0) * strip_bytes;
            for (int b = 0; b < nb; b++) {
                dst[b] = (uint8_t)(((uint64_t)sum[b] * recip + (1ull << 23)) >> 24);
                sum[b] += addr[b] - subr[b];
            }
        }
    }
    free(colbuf);
    free(sum);
}


/* Single-channel variants for the shadow path. A shadow is one constant
   colour at varying coverage, so only alpha carries information -- blurring
   all four channels did 4x the necessary work. RGB is reconstructed from the
   blurred alpha afterwards (see shadow_recolor). */
static void box_blur_h1(uint8_t *p, intptr_t stride, int w, int h, int rad, int off) {
    if (rad < 1 || w <= 1) return;
    int win = rad * 2 + 1;
    uint64_t recip = (1ull << 24) / (uint64_t)win;
    uint8_t *cp = (uint8_t *)malloc((size_t)w);
    if (!cp) return;
    for (int y = 0; y < h; y++) {
        uint8_t *row = p + (intptr_t)y * stride + off;
        for (int x = 0; x < w; x++) cp[x] = row[x * 4];
        int sum = cp[0] * (rad + 1);
        for (int i = 1; i <= rad; i++) sum += cp[i < w ? i : w - 1];
        for (int x = 0; x < w; x++) {
            row[x * 4] = (uint8_t)(((uint64_t)sum * recip + (1ull << 23)) >> 24);
            sum += cp[x + rad + 1 < w ? x + rad + 1 : w - 1] - cp[x - rad > 0 ? x - rad : 0];
        }
    }
    free(cp);
}

static void box_blur_v1(uint8_t *p, intptr_t stride, int w, int h, int rad, int off) {
    if (rad < 1 || h <= 1) return;
    int win = rad * 2 + 1;
    uint64_t recip = (1ull << 24) / (uint64_t)win;
    uint8_t *cp = (uint8_t *)malloc((size_t)h * BLUR_STRIP);
    int *sum = (int *)malloc(BLUR_STRIP * sizeof(int));
    if (!cp || !sum) { free(cp); free(sum); return; }
    for (int xs = 0; xs < w; xs += BLUR_STRIP) {
        int nc = (w - xs < BLUR_STRIP) ? (w - xs) : BLUR_STRIP;
        uint8_t *base = p + (intptr_t)xs * 4 + off;
        for (int y = 0; y < h; y++) {
            uint8_t *src = base + (intptr_t)y * stride;
            for (int i = 0; i < nc; i++) cp[(size_t)y * BLUR_STRIP + i] = src[i * 4];
        }
        for (int i = 0; i < nc; i++) {
            int acc = cp[i] * (rad + 1);
            for (int k = 1; k <= rad; k++) acc += cp[(size_t)(k < h ? k : h - 1) * BLUR_STRIP + i];
            sum[i] = acc;
        }
        for (int y = 0; y < h; y++) {
            uint8_t *dst = base + (intptr_t)y * stride;
            const uint8_t *ad = cp + (size_t)(y + rad + 1 < h ? y + rad + 1 : h - 1) * BLUR_STRIP;
            const uint8_t *sb = cp + (size_t)(y - rad > 0 ? y - rad : 0) * BLUR_STRIP;
            for (int i = 0; i < nc; i++) {
                dst[i * 4] = (uint8_t)(((uint64_t)sum[i] * recip + (1ull << 23)) >> 24);
                sum[i] += ad[i] - sb[i];
            }
        }
    }
    free(cp);
    free(sum);
}

/* Rebuild premultiplied BGRA from the blurred alpha and the shadow's colour. */
static void shadow_recolor(uint8_t *p, intptr_t stride, int w, int h, uint32_t col) {
    unsigned cr = (col >> 16) & 0xFFu, cg = (col >> 8) & 0xFFu, cb = col & 0xFFu;
    for (int y = 0; y < h; y++) {
        uint8_t *row = p + (intptr_t)y * stride;
        for (int x = 0; x < w; x++) {
            unsigned a = row[x * 4 + 3];
            row[x * 4 + 0] = (uint8_t)((cb * a + 127u) / 255u);
            row[x * 4 + 1] = (uint8_t)((cg * a + 127u) / 255u);
            row[x * 4 + 2] = (uint8_t)((cr * a + 127u) / 255u);
        }
    }
}

/* Clamp a region to the canvas; returns 0 when nothing is left to do. */
static int clamp_region(GfxCanvas *c, double x, double y, double w, double h,
                        int *ox, int *oy, int *ow, int *oh) {
    int x0 = (int)floor(x), y0 = (int)floor(y);
    int x1 = (int)ceil(x + w), y1 = (int)ceil(y + h);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    if (x1 <= x0 || y1 <= y0) return 0;
    *ox = x0; *oy = y0; *ow = x1 - x0; *oh = y1 - y0;
    return 1;
}

static void blur_region(GfxCanvas *c, int x, int y, int w, int h, int rad) {
    BLImageData d;
    gfx_flush(c);
    bl_image_get_data(&c->img, &d);
    uint8_t *p = (uint8_t *)d.pixel_data;
    for (int pass = 0; pass < 3; pass++) {
        box_blur_h(p, (intptr_t)d.stride, x, y, w, h, rad);
        box_blur_v(p, (intptr_t)d.stride, x, y, w, h, rad);
    }
}

void gfx_blur_rect(GfxCanvas *c, double x, double y, double w, double h, double radius) {
    int rad = (int)(radius + 0.5);
    int bx, by, bw, bh;
    if (rad < 1) return;
    if (!clamp_region(c, x, y, w, h, &bx, &by, &bw, &bh)) return;
    blur_region(c, bx, by, bw, bh, rad);
}

/* Signed distance to a rounded-rect boundary; negative inside. */
static double rrect_sd(double px, double py, double x, double y,
                       double w, double h, double r) {
    double hw = w * 0.5, hh = h * 0.5;
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    double dx = fabs(px - (x + hw)) - (hw - r);
    double dy = fabs(py - (y + hh)) - (hh - r);
    double ax = dx > 0.0 ? dx : 0.0;
    double ay = dy > 0.0 ? dy : 0.0;
    double outside = sqrt(ax * ax + ay * ay);
    double inside = (dx > dy ? dx : dy);
    if (inside > 0.0) inside = 0.0;
    return outside + inside - r;
}

void gfx_backdrop_blur(GfxCanvas *c, double x, double y, double w, double h,
                       double r, double radius, int tint) {
    int bx, by, bw, bh;
    int rad = (int)(radius + 0.5);
    if (r < 0.0) r = 0.0;

    if (clamp_region(c, x, y, w, h, &bx, &by, &bw, &bh) && rad >= 1) {
        BLImageData d;
        gfx_flush(c);
        bl_image_get_data(&c->img, &d);
        uint8_t *p = (uint8_t *)d.pixel_data;
        intptr_t stride = (intptr_t)d.stride;

        /* Blur is rectangular, but a frosted panel is rounded: keep a copy of
           the region so the corners outside the shape can be put back. Without
           this the blur visibly bleeds past the panel's rounded edge. */
        size_t rowbytes = (size_t)bw * 4;
        uint8_t *saved = (uint8_t *)malloc(rowbytes * (size_t)bh);
        if (!saved) return;
        for (int yy = 0; yy < bh; yy++)
            memcpy(saved + (size_t)yy * rowbytes,
                   p + (intptr_t)(by + yy) * stride + (intptr_t)bx * 4, rowbytes);

        blur_region(c, bx, by, bw, bh, rad);

        /* restore outside the rounded shape, with a 1px antialiased edge */
        for (int yy = 0; yy < bh; yy++) {
            uint8_t *row = p + (intptr_t)(by + yy) * stride + (intptr_t)bx * 4;
            const uint8_t *src = saved + (size_t)yy * rowbytes;
            for (int xx = 0; xx < bw; xx++) {
                double sd = rrect_sd((double)(bx + xx) + 0.5, (double)(by + yy) + 0.5,
                                     x, y, w, h, r);
                double cov = 0.5 - sd;          /* 1 inside, 0 outside, ramp across a pixel */
                if (cov >= 1.0) continue;       /* fully inside: keep the blur */
                if (cov <= 0.0) {               /* fully outside: restore */
                    memcpy(row + xx * 4, src + xx * 4, 4);
                    continue;
                }
                unsigned a = (unsigned)(cov * 255.0 + 0.5);
                for (int ch = 0; ch < 4; ch++) {
                    unsigned nb = row[xx * 4 + ch], ob = src[xx * 4 + ch];
                    row[xx * 4 + ch] = (uint8_t)((nb * a + ob * (255u - a) + 127u) / 255u);
                }
            }
        }
        free(saved);
    }

    /* tint over the blurred backdrop, clipped to the rounded shape */
    if (((uint32_t)tint >> 24) != 0u) {
        BLRoundRect rrect = { x, y, w, h, r, r };
        bl_context_set_fill_style_rgba32(&c->ctx, rgba32_of(tint));
        bl_context_fill_geometry(&c->ctx, BL_GEOMETRY_TYPE_ROUND_RECT, &rrect);
    }
}

/* Soft drop shadow: rasterise the round-rect into a scratch layer, blur it,
   then composite. This replaces a stack of four translucent round-rects that
   grew outward with fading alpha -- cheap, but it banded visibly and its
   spread did not track `blur` the way a real gaussian does. */
void gfx_shadow_round(GfxCanvas *c, double x, double y, double w, double h,
                      double r, double blur, double dy, int color) {
    if (blur < 0.0) blur = 0.0;
    if (r < 0.0) r = 0.0;
    uint32_t col = rgba32_of(color);
    if ((col >> 24) == 0u) col = 0x30000000u | (col & 0xFFFFFFu); /* sane default darkness */
    if (blur < 1.0) {   /* no blur requested: a plain offset fill is exact */
        BLRoundRect rrect = { x, y + dy, w, h, r, r };
        bl_context_set_fill_style_rgba32(&c->ctx, col);
        bl_context_fill_geometry(&c->ctx, BL_GEOMETRY_TYPE_ROUND_RECT, &rrect);
        return;
    }

    int rad = (int)(blur + 0.5);
    int pad = rad * 3 + 2;                 /* three box passes spread ~3*rad */
    int lw = (int)ceil(w) + pad * 2;
    int lh = (int)ceil(h) + pad * 2;
    if (lw <= 0 || lh <= 0) return;

    /* grow the cached scratch layer if needed, then reuse it */
    if (lw > c->sl_w || lh > c->sl_h) {
        int nw = lw > c->sl_w ? lw : c->sl_w;
        int nh = lh > c->sl_h ? lh : c->sl_h;
        free_shadow_layer(c);
        if (bl_image_init_as(&c->sl_img, nw, nh, BL_FORMAT_PRGB32) != BL_SUCCESS) return;
        if (bl_context_init_as(&c->sl_ctx, &c->sl_img, NULL) != BL_SUCCESS) {
            bl_image_destroy(&c->sl_img);
            return;
        }
        c->sl_w = nw;
        c->sl_h = nh;
    }
    BLContextCore *lctxp = &c->sl_ctx;
    /* The scratch layer is shared across shadows and is written through its
       raw pixel pointer, which bypasses blend2d's copy-on-write. A blit queued
       by an earlier shadow may not have executed yet, and would then sample
       the pixels we are about to overwrite -- so drain the main context first.
       (Skipping this made frames non-deterministic run to run.) */
    bl_context_flush(&c->ctx, BL_CONTEXT_FLUSH_SYNC);
    /* clear only the sub-rect this shadow uses, not the whole cached layer */
    BLRect clear_rc = { 0.0, 0.0, (double)lw, (double)lh };
    bl_context_set_comp_op(lctxp, BL_COMP_OP_SRC_COPY);
    bl_context_set_fill_style_rgba32(lctxp, 0x00000000u);
    bl_context_fill_geometry(lctxp, BL_GEOMETRY_TYPE_RECTD, &clear_rc);
    bl_context_set_comp_op(lctxp, BL_COMP_OP_SRC_OVER);
    /* rasterise pure coverage; the colour is applied after the blur */
    BLRoundRect shape = { (double)pad, (double)pad, w, h, r, r };
    bl_context_set_fill_style_rgba32(lctxp, 0xFFFFFFFFu);
    bl_context_fill_geometry(lctxp, BL_GEOMETRY_TYPE_ROUND_RECT, &shape);
    bl_context_flush(lctxp, BL_CONTEXT_FLUSH_SYNC);

    BLImageData ld;
    bl_image_get_data(&c->sl_img, &ld);
    uint8_t *lp = (uint8_t *)ld.pixel_data;
    for (int pass = 0; pass < 3; pass++) {
        box_blur_h1(lp, (intptr_t)ld.stride, lw, lh, rad, 3);
        box_blur_v1(lp, (intptr_t)ld.stride, lw, lh, rad, 3);
    }
    shadow_recolor(lp, (intptr_t)ld.stride, lw, lh, col);

    /* Knock the caster's own shape out of the shadow, the way CSS box-shadow
       does. It is invisible under an opaque panel, but a translucent one would
       otherwise blur and show its own shadow as a dark patch behind the glass. */
    for (int yy = 0; yy < lh; yy++) {
        uint8_t *row = lp + (intptr_t)yy * ld.stride;
        for (int xx = 0; xx < lw; xx++) {
            double sd = rrect_sd((double)xx + 0.5, (double)yy + 0.5,
                                 (double)pad, (double)pad - dy, w, h, r);
            double cov = 0.5 - sd;               /* 1 inside the caster */
            if (cov <= 0.0) continue;
            if (cov >= 1.0) { row[xx * 4 + 0] = 0; row[xx * 4 + 1] = 0;
                              row[xx * 4 + 2] = 0; row[xx * 4 + 3] = 0; continue; }
            unsigned keep = (unsigned)((1.0 - cov) * 255.0 + 0.5);
            for (int ch = 0; ch < 4; ch++)
                row[xx * 4 + ch] = (uint8_t)((row[xx * 4 + ch] * keep + 127u) / 255u);
        }
    }

    BLPoint at = { x - (double)pad, y + dy - (double)pad };
    BLRectI src = { 0, 0, lw, lh };
    bl_context_blit_image_d(&c->ctx, &at, &c->sl_img, &src);
}

/* clip stack — blend2d C core exposes only rect clip; rounded radius degrades
   to a rectangular scissor (acceptable per the graceful-degradation rule). */
void gfx_clip_push(GfxCanvas *c, double x, double y, double w, double h, double r) {
    (void)r; /* rounded clip not available in the C core → rect scissor */
    BLRect rc = { x, y, w, h };
    bl_context_save(&c->ctx, NULL);
    bl_context_clip_to_rect_d(&c->ctx, &rc);
}

void gfx_clip_pop(GfxCanvas *c) {
    bl_context_restore(&c->ctx, NULL);
}

/* ---- affine transforms -------------------------------------------------- *
 * blend2d's user transform is mutated in place by apply_transform_op and is
 * saved/restored by bl_context_save/restore — the SAME state stack gfx_clip_
 * push/pop use. So gfx_save/gfx_restore nest with clip pushes on one stack:
 * every save (or clip_push) must be balanced by a restore (or clip_pop). */
void gfx_save(GfxCanvas *c) { bl_context_save(&c->ctx, NULL); }
void gfx_restore(GfxCanvas *c) { bl_context_restore(&c->ctx, NULL); }

void gfx_translate(GfxCanvas *c, double dx, double dy) {
    double v[2] = { dx, dy };
    bl_context_apply_transform_op(&c->ctx, BL_TRANSFORM_OP_TRANSLATE, v);
}
void gfx_scale(GfxCanvas *c, double sx, double sy) {
    double v[2] = { sx, sy };
    bl_context_apply_transform_op(&c->ctx, BL_TRANSFORM_OP_SCALE, v);
}
void gfx_rotate(GfxCanvas *c, double deg) {
    /* degrees at the Vyto-facing edge, like gfx_stroke_arc */
    double rad = deg * (3.14159265358979323846 / 180.0);
    bl_context_apply_transform_op(&c->ctx, BL_TRANSFORM_OP_ROTATE, &rad);
}
void gfx_reset_transform(GfxCanvas *c) {
    bl_context_apply_transform_op(&c->ctx, BL_TRANSFORM_OP_RESET, NULL);
}

/* ---- radial gradient ---------------------------------------------------- *
 * Centered radial (focal = center, inner radius 0), filling the given rect —
 * the radial twin of gfx_linear_gradient_rect_n. */
void gfx_radial_gradient_rect_n(GfxCanvas *c, double x, double y, double w, double h,
                                double cx, double cy, double radius,
                                const int *colors, const double *positions, int n) {
    if (n <= 0 || radius <= 0.0) return;
    BLGradientCore g;
    /* (x0,y0,r0) = the outer circle (center + radius); (x1,y1,r1) = the focal
       circle, here coincident with the center at radius 0 for a plain radial */
    BLRadialGradientValues rv = { cx, cy, cx, cy, radius, 0.0 };
    bl_gradient_init_as(&g, BL_GRADIENT_TYPE_RADIAL, &rv, BL_EXTEND_MODE_PAD, NULL, 0, NULL);
    for (int i = 0; i < n; i++) {
        bl_gradient_add_stop_rgba32(&g, positions[i], rgba32_of(colors[i]));
    }
    BLRect r = { x, y, w, h };
    bl_context_fill_geometry_ext(&c->ctx, BL_GEOMETRY_TYPE_RECTD, &r, (const BLUnknown *)&g);
    bl_gradient_destroy(&g);
}

/* ---- vector path -------------------------------------------------------- *
 * A path arrives as a flat command stream (`cmds`: 0=move 1=line 2=quad
 * 3=cubic 4=close) plus a parallel `coords` array holding each command's
 * operands in order. Rebuild a transient BLPath and fill/stroke it. This
 * mirrors the vyto/geom/path.vt Path exactly so one builder serves both the
 * rich tier (here) and the lean tier (Path.flatten → polygon). */
static int gfx_build_path(BLPathCore *p, const int *cmds, int nc,
                          const double *coords, int ncoord) {
    bl_path_init(p);
    int ci = 0;
    for (int k = 0; k < nc; k++) {
        switch (cmds[k]) {
        case 0: if (ci + 2 > ncoord) return 0;
            bl_path_move_to(p, coords[ci], coords[ci + 1]); ci += 2; break;
        case 1: if (ci + 2 > ncoord) return 0;
            bl_path_line_to(p, coords[ci], coords[ci + 1]); ci += 2; break;
        case 2: if (ci + 4 > ncoord) return 0;
            bl_path_quad_to(p, coords[ci], coords[ci + 1], coords[ci + 2], coords[ci + 3]);
            ci += 4; break;
        case 3: if (ci + 6 > ncoord) return 0;
            bl_path_cubic_to(p, coords[ci], coords[ci + 1], coords[ci + 2],
                             coords[ci + 3], coords[ci + 4], coords[ci + 5]);
            ci += 6; break;
        case 4: bl_path_close(p); break;
        default: break;
        }
    }
    return 1;
}

void gfx_fill_path(GfxCanvas *c, const int *cmds, int nc,
                   const double *coords, int ncoord, int color) {
    BLPathCore p;
    if (!gfx_build_path(&p, cmds, nc, coords, ncoord)) { bl_path_destroy(&p); return; }
    bl_context_set_fill_style_rgba32(&c->ctx, rgba32_of(color));
    bl_context_fill_geometry(&c->ctx, BL_GEOMETRY_TYPE_PATH, &p);
    bl_path_destroy(&p);
}

void gfx_stroke_path(GfxCanvas *c, const int *cmds, int nc,
                     const double *coords, int ncoord, double width, int color) {
    BLPathCore p;
    if (!gfx_build_path(&p, cmds, nc, coords, ncoord)) { bl_path_destroy(&p); return; }
    bl_context_set_stroke_width(&c->ctx, width);
    bl_context_stroke_geometry_rgba32(&c->ctx, BL_GEOMETRY_TYPE_PATH, &p, rgba32_of(color));
    bl_path_destroy(&p);
}

int gfx_load_font(GfxCanvas *c, const char *ttf_path, double size) {
    return gfx_load_font_weight(c, ttf_path, size, 0);
}

int gfx_load_font_weight(GfxCanvas *c, const char *ttf_path, double size, int weight) {
    if (weight < 0 || weight >= GFX_WEIGHTS) weight = 0;
    /* release a previous font in this slot */
    if (c->has_font[weight]) {
        bl_font_destroy(&c->font[weight]);
        bl_font_face_destroy(&c->face[weight]);
        c->has_font[weight] = 0;
    }
    bl_font_face_init(&c->face[weight]);
    if (bl_font_face_create_from_file(&c->face[weight], ttf_path, 0) != BL_SUCCESS) {
        bl_font_face_destroy(&c->face[weight]);
        return 0;
    }
    bl_font_init(&c->font[weight]);
    if (bl_font_create_from_face(&c->font[weight], &c->face[weight], (float)size) != BL_SUCCESS) {
        bl_font_destroy(&c->font[weight]);
        bl_font_face_destroy(&c->face[weight]);
        return 0;
    }
    c->has_font[weight] = 1;
    if (!c->has_font[c->active_weight]) c->active_weight = weight;
    return 1;
}

void gfx_set_font_weight(GfxCanvas *c, int weight) {
    if (weight < 0 || weight >= GFX_WEIGHTS) weight = 0;
    if (c->has_font[weight]) c->active_weight = weight;
    else if (c->has_font[0]) c->active_weight = 0;
}

static BLFontCore *active_font(GfxCanvas *c) {
    if (c->has_font[c->active_weight]) return &c->font[c->active_weight];
    for (int i = 0; i < GFX_WEIGHTS; i++) if (c->has_font[i]) return &c->font[i];
    return NULL;
}

void gfx_text(GfxCanvas *c, double x, double y, const char *utf8, int color) {
    BLFontCore *f = active_font(c);
    if (!f) return;
    BLPoint p = { x, y };
    bl_context_set_fill_style_rgba32(&c->ctx, rgba32_of(color));
    bl_context_fill_utf8_text_d(&c->ctx, &p, f, utf8, strlen(utf8));
}

double gfx_text_width(GfxCanvas *c, const char *utf8) {
    BLFontCore *f = active_font(c);
    if (!f) return 0.0;
    BLGlyphBufferCore gb;
    bl_glyph_buffer_init(&gb);
    bl_glyph_buffer_set_text(&gb, utf8, strlen(utf8), BL_TEXT_ENCODING_UTF8);
    BLTextMetrics tm;
    bl_font_get_text_metrics(f, &gb, &tm);
    bl_glyph_buffer_destroy(&gb);
    return tm.advance.x;
}

double gfx_font_ascent(GfxCanvas *c) {
    BLFontCore *f = active_font(c);
    if (!f) return 0.0;
    BLFontMetrics fm;
    bl_font_get_metrics(f, &fm);
    return fm.ascent;
}

double gfx_font_height(GfxCanvas *c) {
    BLFontCore *f = active_font(c);
    if (!f) return 0.0;
    BLFontMetrics fm;
    bl_font_get_metrics(f, &fm);
    return fm.ascent + fm.descent + fm.line_gap;
}

void gfx_flush(GfxCanvas *c) {
    bl_context_flush(&c->ctx, BL_CONTEXT_FLUSH_SYNC);
}

void *gfx_pixels(GfxCanvas *c) {
    BLImageData d;
    bl_image_get_data(&c->img, &d);
    return d.pixel_data;
}

int gfx_stride(GfxCanvas *c) {
    BLImageData d;
    bl_image_get_data(&c->img, &d);
    return (int)d.stride;
}

/* ---- render snapshots (see header) ---------------------------------------
   Both flush first: the context batches, so reading pixel_data without a sync
   flush can hand back a partially-rendered frame. */

unsigned gfx_hash(GfxCanvas *c) {
    BLImageData d;
    gfx_flush(c);
    bl_image_get_data(&c->img, &d);
    const unsigned char *p = (const unsigned char *)d.pixel_data;
    /* FNV-1a over visible pixels only — stride padding is skipped so the hash
       is a property of the image, not of blend2d's row alignment. */
    unsigned h = 2166136261u;
    for (int y = 0; y < c->h; y++) {
        const unsigned char *row = p + (size_t)y * (size_t)d.stride;
        for (int i = 0; i < c->w * 4; i++) {
            h ^= row[i];
            h *= 16777619u;
        }
    }
    return h;
}

int gfx_write_ppm(GfxCanvas *c, const char *path) {
    BLImageData d;
    FILE *f;
    unsigned char *row;
    gfx_flush(c);
    bl_image_get_data(&c->img, &d);
    f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "P6\n%d %d\n255\n", c->w, c->h);
    row = (unsigned char *)malloc((size_t)c->w * 3);
    if (!row) { fclose(f); return 0; }
    for (int y = 0; y < c->h; y++) {
        const unsigned char *src = (const unsigned char *)d.pixel_data + (size_t)y * (size_t)d.stride;
        for (int x = 0; x < c->w; x++) {
            /* PRGB32 is BGRA in memory on little-endian; PPM wants RGB. The
               canvas is opaque in practice (cleared before each frame), so the
               premultiplied channels are already the display values. */
            row[x * 3 + 0] = src[x * 4 + 2];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 0];
        }
        fwrite(row, 1, (size_t)c->w * 3, f);
    }
    free(row);
    fclose(f);
    return 1;
}

/* ---- decoded images (PNG/JPEG/BMP/QOI — all built into libblend2d, no
   extra codec dependency) — a load-once opaque handle, same shape as the
   font lifecycle above. */

typedef struct GfxImage GfxImage;
struct GfxImage {
    BLImageCore img;
};

void *gfx_image_load_file(const char *path) {
    GfxImage *im = (GfxImage *)malloc(sizeof(GfxImage));
    if (!im) return NULL;
    bl_image_init(&im->img);
    /* NULL codecs = auto-detect by sniffing the file header against the
       built-in codec registry (PNG/JPEG/BMP/QOI) */
    if (bl_image_read_from_file(&im->img, path, NULL) != BL_SUCCESS) {
        bl_image_destroy(&im->img);
        free(im);
        return NULL;
    }
    return im;
}

void *gfx_image_load_bytes(const void *data, int len) {
    if (!data || len <= 0) return NULL;
    GfxImage *im = (GfxImage *)malloc(sizeof(GfxImage));
    if (!im) return NULL;
    bl_image_init(&im->img);
    if (bl_image_read_from_data(&im->img, data, (size_t)len, NULL) != BL_SUCCESS) {
        bl_image_destroy(&im->img);
        free(im);
        return NULL;
    }
    return im;
}

void gfx_image_free(void *img) {
    if (!img) return;
    GfxImage *im = (GfxImage *)img;
    bl_image_destroy(&im->img);
    free(im);
}

int gfx_image_width(void *img) {
    if (!img) return 0;
    BLImageData d;
    bl_image_get_data(&((GfxImage *)img)->img, &d);
    return d.size.w;
}

int gfx_image_height(void *img) {
    if (!img) return 0;
    BLImageData d;
    bl_image_get_data(&((GfxImage *)img)->img, &d);
    return d.size.h;
}

void gfx_draw_image(GfxCanvas *c, void *img, double x, double y, double w, double h) {
    if (!img) return;
    BLRect r = { x, y, w, h };
    bl_context_blit_scaled_image_d(&c->ctx, &r, &((GfxImage *)img)->img, NULL);
}