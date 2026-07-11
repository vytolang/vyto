/* gfx_shim — flat handle API over blend2d (see gfx_shim.h). Compiled as C by
   voltc; links against the prebuilt libblend2d in native/<platform>/. */
#include "gfx_shim.h"

#include <blend2d/blend2d.h>
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
};

/* Resolve a 0xAARRGGBB color: alpha byte 0 → opaque (0xFF) for backward
   compatibility with legacy 0xRRGGBB fills. */
static uint32_t rgba32_of(int color) {
    uint32_t a = ((uint32_t)color >> 24) & 0xFFu;
    if (a == 0u) a = 0xFFu;
    uint32_t rgb = (uint32_t)color & 0xFFFFFFu;
    return (a << 24) | rgb;
}

/* silently keep the legacy `opaque()` helper for any user of the header */
static uint32_t opaque(int rgb) { return 0xFF000000u | ((uint32_t)rgb & 0xFFFFFFu); }

GfxCanvas *gfx_canvas_new(int w, int h) {
    if (w <= 0 || h <= 0) return NULL;
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

void gfx_canvas_free(GfxCanvas *c) {
    if (!c) return;
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
   Volt-facing edge. */
void gfx_stroke_arc(GfxCanvas *c, double cx, double cy, double rx, double ry,
                    double start_deg, double sweep_deg, double width, int color) {
    const double deg2rad = 3.14159265358979323846 / 180.0;
    BLArc a = { cx, cy, rx, ry, start_deg * deg2rad, sweep_deg * deg2rad };
    bl_context_set_stroke_width(&c->ctx, width);
    bl_context_stroke_geometry_rgba32(&c->ctx, BL_GEOMETRY_TYPE_ARC, &a, rgba32_of(color));
}

/* Soft shadow: draw the round-rect `layers` times, growing outward by a
   fraction of `blur` and fading the alpha, offset by `dy`. blend2d composites
   each translucent fill over the existing pixels, producing a gaussian-ish
   halo. Dependency-free (no blur filter) and reads fine at the gallery scale. */
void gfx_shadow_round(GfxCanvas *c, double x, double y, double w, double h,
                      double r, double blur, double dy, int color) {
    if (blur < 0.0) blur = 0.0;
    if (r < 0.0) r = 0.0;
    const int layers = 4;
    uint32_t base = rgba32_of(color);
    uint32_t base_a = (base >> 24) & 0xFFu;
    if (base_a == 0u) base_a = 0x30u; /* a sane default darkness */
    for (int i = 0; i < layers; i++) {
        double t = (layers == 1) ? 0.0 : (double)i / (double)(layers - 1);
        double grow = blur * (0.25 + 0.75 * t);
        double rr = r + grow;
        /* outer layers fainter, inner layers stronger → soft falloff */
        uint32_t a = (uint32_t)((double)base_a * (0.55 - 0.35 * t));
        if (a < 1u) a = 1u;
        uint32_t col = (a << 24) | (base & 0xFFFFFFu);
        BLRoundRect rrect = { x - grow, y - grow + dy, w + 2.0 * grow, h + 2.0 * grow, rr, rr };
        bl_context_set_fill_style_rgba32(&c->ctx, col);
        bl_context_fill_geometry(&c->ctx, BL_GEOMETRY_TYPE_ROUND_RECT, &rrect);
    }
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
    /* degrees at the Volt-facing edge, like gfx_stroke_arc */
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
 * mirrors the volt/geom/path.vt Path exactly so one builder serves both the
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