/* gfx_shim — a flat handle API over blend2d for volt/gfx.
 *
 * blend2d's C API uses 16-byte value "core" objects passed by address, which
 * is awkward to bind from Volt. This shim hides all of that behind opaque
 * handles + plain scalars, so the Volt side sees only rawptr/int/float/cstring.
 * A Canvas owns a BLImage (PRGB32) plus a rendering context; colors are
 * 0xRRGGBB (opaque) — the shim adds the alpha. */
#ifndef GFX_SHIM_H
#define GFX_SHIM_H

typedef struct GfxCanvas GfxCanvas;

GfxCanvas *gfx_canvas_new(int w, int h); /* NULL on failure */
void gfx_canvas_free(GfxCanvas *c);
int gfx_width(GfxCanvas *c);
int gfx_height(GfxCanvas *c);

/* drawing — coordinates and sizes are pixels; rgb is 0xRRGGBB */
void gfx_clear(GfxCanvas *c, int rgb);
void gfx_fill_rect(GfxCanvas *c, double x, double y, double w, double h, int rgb);
void gfx_fill_round_rect(GfxCanvas *c, double x, double y, double w, double h, double r, int rgb);
void gfx_fill_circle(GfxCanvas *c, double cx, double cy, double radius, int rgb);
void gfx_stroke_line(GfxCanvas *c, double x0, double y0, double x1, double y1, double width, int rgb);
void gfx_linear_gradient_rect(GfxCanvas *c, double x, double y, double w, double h,
                              double x0, double y0, double x1, double y1, int rgb0, int rgb1);

/* text — load a TTF/OTF once, then draw at a baseline origin */
int gfx_load_font(GfxCanvas *c, const char *ttf_path, double size); /* 1 ok, 0 fail */
void gfx_text(GfxCanvas *c, double x, double y, const char *utf8, int rgb);
double gfx_text_width(GfxCanvas *c, const char *utf8);
double gfx_font_ascent(GfxCanvas *c);
double gfx_font_height(GfxCanvas *c);

/* present — flush pending rendering, then hand out the raw ARGB buffer */
void gfx_flush(GfxCanvas *c);
void *gfx_pixels(GfxCanvas *c); /* 0x00RRGGBB per pixel (opaque frames) */
int gfx_stride(GfxCanvas *c);   /* bytes per row */

#endif
