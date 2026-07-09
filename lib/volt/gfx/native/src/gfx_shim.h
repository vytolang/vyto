/* gfx_shim — a flat handle API over blend2d for volt/gfx.
 *
 * blend2d's C API uses 16-byte value "core" objects passed by address, which
 * is awkward to bind from Volt. This shim hides all of that behind opaque
 * handles + plain scalars, so the Volt side sees only rawptr/int/float/cstring.
 * A Canvas owns a BLImage (PRGB32) plus a rendering context.
 *
 * Colors are packed 0xAARRGGBB. The alpha byte 0 means OPAQUE (backward
 * compatibility with the legacy 0xRRGGBB calls) — set a non-zero alpha byte
 * to blend a translucent fill. The shim resolves the alpha before each call.
 *
 * Three font weights can be loaded per canvas (regular / medium / bold);
 * `gfx_set_font_weight` selects the active one for subsequent text draws. */
#ifndef GFX_SHIM_H
#define GFX_SHIM_H

typedef struct GfxCanvas GfxCanvas;

GfxCanvas *gfx_canvas_new(int w, int h); /* NULL on failure */
void gfx_canvas_free(GfxCanvas *c);
int gfx_width(GfxCanvas *c);
int gfx_height(GfxCanvas *c);

/* drawing — coordinates and sizes are pixels; color is 0xAARRGGBB (alpha 0 = opaque) */
void gfx_clear(GfxCanvas *c, int color);
void gfx_fill_rect(GfxCanvas *c, double x, double y, double w, double h, int color);
void gfx_fill_round_rect(GfxCanvas *c, double x, double y, double w, double h, double r, int color);
void gfx_fill_circle(GfxCanvas *c, double cx, double cy, double radius, int color);
void gfx_stroke_line(GfxCanvas *c, double x0, double y0, double x1, double y1, double width, int color);
void gfx_stroke_round_rect(GfxCanvas *c, double x, double y, double w, double h, double r, double width, int color);
void gfx_stroke_circle(GfxCanvas *c, double cx, double cy, double radius, double width, int color);
void gfx_linear_gradient_rect(GfxCanvas *c, double x, double y, double w, double h,
                              double x0, double y0, double x1, double y1, int c0, int c1);

/* soft shadow: layered translucent round-rects centered on (x,y,w,h) with the
   given radius, growing by `blur` and offset by `dy`. `color` is 0xAARRGGBB;
   alpha controls darkness. Dependency-free gaussian approximation. */
void gfx_shadow_round(GfxCanvas *c, double x, double y, double w, double h,
                      double r, double blur, double dy, int color);

/* clip stack: save state (incl. clip) and intersect the clip with a rect.
   radius==0 → rectangular clip; radius>0 falls back to the bounding rect
   (the blend2d C core exposes only rect clip). */
void gfx_clip_push(GfxCanvas *c, double x, double y, double w, double h, double r);
void gfx_clip_pop(GfxCanvas *c);

/* text — load up to three weights (0=regular, 1=medium/semi, 2=bold), then
   draw at a baseline origin with the active weight (defaults to 0). */
int gfx_load_font(GfxCanvas *c, const char *ttf_path, double size);              /* regular slot, 1 ok */
int gfx_load_font_weight(GfxCanvas *c, const char *ttf_path, double size, int weight); /* 1 ok, 0 fail */
void gfx_set_font_weight(GfxCanvas *c, int weight); /* 0/1/2 */
void gfx_text(GfxCanvas *c, double x, double y, const char *utf8, int color);
double gfx_text_width(GfxCanvas *c, const char *utf8);
double gfx_font_ascent(GfxCanvas *c);
double gfx_font_height(GfxCanvas *c);

/* present — flush pending rendering, then hand out the raw ARGB buffer */
void gfx_flush(GfxCanvas *c);
void *gfx_pixels(GfxCanvas *c); /* 0xAARRGGBB per pixel (opaque when bg cleared opaque) */
int gfx_stride(GfxCanvas *c);   /* bytes per row */

#endif