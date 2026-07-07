/* gfx_shim — flat handle API over blend2d (see gfx_shim.h). Compiled as C by
   voltc; links against the prebuilt libblend2d in native/<platform>/. */
#include "gfx_shim.h"

#include <blend2d/blend2d.h>
#include <stdlib.h>
#include <string.h>

struct GfxCanvas {
    BLImageCore img;
    BLContextCore ctx;
    BLFontFaceCore face;
    BLFontCore font;
    int w, h;
    int has_font;
};

static uint32_t opaque(int rgb) { return 0xFF000000u | ((uint32_t)rgb & 0xFFFFFFu); }

GfxCanvas *gfx_canvas_new(int w, int h) {
    if (w <= 0 || h <= 0) return NULL;
    GfxCanvas *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->w = w;
    c->h = h;
    if (bl_image_init_as(&c->img, w, h, BL_FORMAT_PRGB32) != BL_SUCCESS) { free(c); return NULL; }
    if (bl_context_init_as(&c->ctx, &c->img, NULL) != BL_SUCCESS) {
        bl_image_destroy(&c->img);
        free(c);
        return NULL;
    }
    return c;
}

void gfx_canvas_free(GfxCanvas *c) {
    if (!c) return;
    if (c->has_font) { bl_font_destroy(&c->font); bl_font_face_destroy(&c->face); }
    bl_context_destroy(&c->ctx);
    bl_image_destroy(&c->img);
    free(c);
}

int gfx_width(GfxCanvas *c) { return c ? c->w : 0; }
int gfx_height(GfxCanvas *c) { return c ? c->h : 0; }

void gfx_clear(GfxCanvas *c, int rgb) {
    bl_context_set_fill_style_rgba32(&c->ctx, opaque(rgb));
    bl_context_fill_all(&c->ctx);
}

void gfx_fill_rect(GfxCanvas *c, double x, double y, double w, double h, int rgb) {
    BLRect r = { x, y, w, h };
    bl_context_set_fill_style_rgba32(&c->ctx, opaque(rgb));
    bl_context_fill_geometry(&c->ctx, BL_GEOMETRY_TYPE_RECTD, &r);
}

void gfx_fill_round_rect(GfxCanvas *c, double x, double y, double w, double h, double r, int rgb) {
    BLRoundRect rr = { x, y, w, h, r, r };
    bl_context_set_fill_style_rgba32(&c->ctx, opaque(rgb));
    bl_context_fill_geometry(&c->ctx, BL_GEOMETRY_TYPE_ROUND_RECT, &rr);
}

void gfx_fill_circle(GfxCanvas *c, double cx, double cy, double radius, int rgb) {
    BLCircle ci = { cx, cy, radius };
    bl_context_set_fill_style_rgba32(&c->ctx, opaque(rgb));
    bl_context_fill_geometry(&c->ctx, BL_GEOMETRY_TYPE_CIRCLE, &ci);
}

void gfx_stroke_line(GfxCanvas *c, double x0, double y0, double x1, double y1, double width, int rgb) {
    BLLine ln = { x0, y0, x1, y1 };
    bl_context_set_stroke_style_rgba32(&c->ctx, opaque(rgb));
    bl_context_set_stroke_width(&c->ctx, width);
    bl_context_stroke_geometry(&c->ctx, BL_GEOMETRY_TYPE_LINE, &ln);
}

void gfx_linear_gradient_rect(GfxCanvas *c, double x, double y, double w, double h,
                              double x0, double y0, double x1, double y1, int rgb0, int rgb1) {
    BLGradientCore g;
    BLLinearGradientValues lv = { x0, y0, x1, y1 };
    bl_gradient_init_as(&g, BL_GRADIENT_TYPE_LINEAR, &lv, BL_EXTEND_MODE_PAD, NULL, 0, NULL);
    bl_gradient_add_stop_rgba32(&g, 0.0, opaque(rgb0));
    bl_gradient_add_stop_rgba32(&g, 1.0, opaque(rgb1));
    BLRect r = { x, y, w, h };
    bl_context_fill_geometry_ext(&c->ctx, BL_GEOMETRY_TYPE_RECTD, &r, (const BLUnknown *)&g);
    bl_gradient_destroy(&g);
}

int gfx_load_font(GfxCanvas *c, const char *ttf_path, double size) {
    if (c->has_font) { bl_font_destroy(&c->font); bl_font_face_destroy(&c->face); c->has_font = 0; }
    bl_font_face_init(&c->face);
    if (bl_font_face_create_from_file(&c->face, ttf_path, 0) != BL_SUCCESS) {
        bl_font_face_destroy(&c->face);
        return 0;
    }
    bl_font_init(&c->font);
    if (bl_font_create_from_face(&c->font, &c->face, (float)size) != BL_SUCCESS) {
        bl_font_destroy(&c->font);
        bl_font_face_destroy(&c->face);
        return 0;
    }
    c->has_font = 1;
    return 1;
}

void gfx_text(GfxCanvas *c, double x, double y, const char *utf8, int rgb) {
    if (!c->has_font) return;
    BLPoint p = { x, y };
    bl_context_set_fill_style_rgba32(&c->ctx, opaque(rgb));
    bl_context_fill_utf8_text_d(&c->ctx, &p, &c->font, utf8, strlen(utf8));
}

double gfx_text_width(GfxCanvas *c, const char *utf8) {
    if (!c->has_font) return 0.0;
    BLGlyphBufferCore gb;
    bl_glyph_buffer_init(&gb);
    bl_glyph_buffer_set_text(&gb, utf8, strlen(utf8), BL_TEXT_ENCODING_UTF8);
    BLTextMetrics tm;
    bl_font_get_text_metrics(&c->font, &gb, &tm);
    bl_glyph_buffer_destroy(&gb);
    return tm.advance.x;
}

double gfx_font_ascent(GfxCanvas *c) {
    if (!c->has_font) return 0.0;
    BLFontMetrics fm;
    bl_font_get_metrics(&c->font, &fm);
    return fm.ascent;
}

double gfx_font_height(GfxCanvas *c) {
    if (!c->has_font) return 0.0;
    BLFontMetrics fm;
    bl_font_get_metrics(&c->font, &fm);
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
