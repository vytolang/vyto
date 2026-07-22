/* gfx_shim — a flat handle API over blend2d for vyto/gfx.
 *
 * blend2d's C API uses 16-byte value "core" objects passed by address, which
 * is awkward to bind from Vyto. This shim hides all of that behind opaque
 * handles + plain scalars, so the Vyto side sees only rawptr/int/float/cstring.
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

/* multi-stop linear gradient: `colors`/`positions` are parallel arrays of
   length `n` (positions in [0,1], ascending). For OS skins needing more than
   a 2-color ramp (e.g. a glossy highlight band). gradient_v/gfx_linear_
   gradient_rect stay as the dedicated 2-stop path — this is additive, not a
   replacement. */
void gfx_linear_gradient_rect_n(GfxCanvas *c, double x, double y, double w, double h,
                                double x0, double y0, double x1, double y1,
                                const int *colors, const double *positions, int n);

/* bevel border: paired light/dark straight strokes on opposite edges (no
   corner arcs — a dependency-free approximation, not a true rounded bevel).
   `raised`: light on top+left, dark on bottom+right (a raised 3D look);
   false swaps them (a pressed/sunken look). `radius` insets the strokes off
   the corners so they don't overlap; pass the same radius as the fill. */
void gfx_bevel_round(GfxCanvas *c, double x, double y, double w, double h,
                     double radius, double width, int light, int dark, int raised);

/* soft shadow: layered translucent round-rects centered on (x,y,w,h) with the
   given radius, growing by `blur` and offset by `dy`. `color` is 0xAARRGGBB;
   alpha controls darkness. Dependency-free gaussian approximation. */
void gfx_shadow_round(GfxCanvas *c, double x, double y, double w, double h,
                      double r, double blur, double dy, int color);

/* arbitrary filled polygon: xs/ys are parallel arrays of length n (n>=3).
   For icon glyphs a rect/round-rect/circle can't express — stars, hearts,
   roofs, speech-bubble tails. */
void gfx_fill_polygon(GfxCanvas *c, const double *xs, const double *ys, int n, int color);

/* partial ring stroke (e.g. a wifi/signal arc). Angles in degrees, 0 = +x
   axis, clockwise; rx/ry let it draw an ellipse arc, pass rx==ry for a
   circular one. */
void gfx_stroke_arc(GfxCanvas *c, double cx, double cy, double rx, double ry,
                    double start_deg, double sweep_deg, double width, int color);

/* clip stack: save state (incl. clip) and intersect the clip with a rect.
   radius > 0 gives a genuine rounded clip with an antialiased edge — blend2d
   has no clip-to-path in EITHER its C or C++ API, so the corners are emulated
   by saving the pixels they can spill into and blending them back on pop (see
   the long note at gfx_clip_push). The radius degrades to a plain rectangular
   scissor under a rotated/skewed transform or if the corner copies cannot be
   allocated; nothing else does.

   Every push must be balanced by a pop — the saved corners are held until it
   comes, and pushes share blend2d's state stack with gfx_save/gfx_restore. */
void gfx_clip_push(GfxCanvas *c, double x, double y, double w, double h, double r);
void gfx_clip_pop(GfxCanvas *c);

/* affine transforms — mutate the user transform, saved/restored on the SAME
   state stack as gfx_clip_push/pop (one shared blend2d stack; keep every
   save/clip_push balanced by a restore/clip_pop). rotate is in degrees. */
void gfx_save(GfxCanvas *c);
void gfx_restore(GfxCanvas *c);
void gfx_translate(GfxCanvas *c, double dx, double dy);
void gfx_scale(GfxCanvas *c, double sx, double sy);
void gfx_rotate(GfxCanvas *c, double deg);
void gfx_reset_transform(GfxCanvas *c);

/* centered radial gradient (focal = center, inner radius 0) filling a rect —
   the radial twin of gfx_linear_gradient_rect_n. colors/positions parallel. */
void gfx_radial_gradient_rect_n(GfxCanvas *c, double x, double y, double w, double h,
                                double cx, double cy, double radius,
                                const int *colors, const double *positions, int n);

/* vector path — a flat command stream (0=move 1=line 2=quad 3=cubic 4=close)
   with a parallel coords array of each command's operands. Mirrors
   vyto/geom/path.vt so one builder feeds both tiers. */
void gfx_fill_path(GfxCanvas *c, const int *cmds, int nc,
                   const double *coords, int ncoord, int color);
void gfx_stroke_path(GfxCanvas *c, const int *cmds, int nc,
                     const double *coords, int ncoord, double width, int color);

/* text — load up to three weights (0=regular, 1=medium/semi, 2=bold), then
   draw at a baseline origin with the active weight (defaults to 0).

   A weight slot names a TYPEFACE, not a fixed size: gfx_set_font_size changes
   the size the next draw uses, independently of the weight, so one canvas can
   render a whole type scale. Faces are cached by path and sized fonts by
   (face, size), so re-selecting a size already drawn costs a lookup, not a
   font build. Passing size <= 0 restores the size the slots were loaded at. */
int gfx_load_font(GfxCanvas *c, const char *ttf_path, double size);              /* regular slot, 1 ok */
int gfx_load_font_weight(GfxCanvas *c, const char *ttf_path, double size, int weight); /* 1 ok, 0 fail */
void gfx_set_font_weight(GfxCanvas *c, int weight); /* 0/1/2 */
void gfx_set_font_size(GfxCanvas *c, double size);
double gfx_get_font_size(GfxCanvas *c);
void gfx_text(GfxCanvas *c, double x, double y, const char *utf8, int color);
double gfx_text_width(GfxCanvas *c, const char *utf8);
double gfx_font_ascent(GfxCanvas *c);
double gfx_font_height(GfxCanvas *c);

/* present — flush pending rendering, then hand out the raw ARGB buffer */
void gfx_flush(GfxCanvas *c);
void *gfx_pixels(GfxCanvas *c); /* 0xAARRGGBB per pixel (opaque when bg cleared opaque) */
int gfx_stride(GfxCanvas *c);   /* bytes per row */

/* ---- blur -----------------------------------------------------------------
   A real separable blur over the canvas pixels, not a stack of translucent
   shapes. Three box-blur passes converge on a gaussian closely enough that the
   difference is invisible, and each pass is O(pixels) via a running sum, so
   cost is independent of radius.

   gfx_blur_rect blurs a region in place.

   gfx_backdrop_blur blurs what is already behind a region and composites a
   translucent tint over it — the frosted-glass material behind sheets,
   popovers and vibrant navigation bars. Pass tint alpha 0 for pure blur.

   Both clamp the region to the canvas and are no-ops for radius < 1. */
void gfx_blur_rect(GfxCanvas *c, double x, double y, double w, double h, double radius);
void gfx_backdrop_blur(GfxCanvas *c, double x, double y, double w, double h,
                       double r, double radius, int tint);

/* ---- render snapshots (test support) --------------------------------------
   The canvas is window-independent, so a headless Surface + GfxPainter renders
   a full widget tree into this buffer with no display server. These two turn
   that buffer into something a test can assert on.

   gfx_hash is the assertion: an FNV-1a over the visible pixels only (the row
   padding inside `stride` is skipped, so the value doesn't depend on how
   blend2d chose to align rows). Printed to stdout, it plugs straight into the
   existing text-golden harness.

   gfx_write_ppm is the debugging aid — a hash tells you something changed but
   not what, so on mismatch a test writes both actual and expected as viewable
   images. PPM (binary P6) is used rather than PNG because blend2d's C core
   exposes decoding only; PPM needs no codec and every image tool reads it. */
unsigned gfx_hash(GfxCanvas *c);
int gfx_write_ppm(GfxCanvas *c, const char *path); /* 1 ok, 0 fail */

/* Read one pixel as 0xAARRGGBB (0 outside the canvas). The hash above says a
   frame moved but not how, and a PPM needs a human; this lets a test state the
   property it actually cares about — "this corner is background, that centre is
   filled" — so a failure names the defect instead of just a changed number. */
unsigned gfx_pixel_at(GfxCanvas *c, int x, int y);

/* decoded images — PNG/JPEG/BMP/QOI, all built into libblend2d already (no
   extra codec dependency). Load-once opaque handle, same lifecycle shape as
   the font weights above: load, use every frame, free when the widget dies.
   Known limitation of this vendored blend2d build, confirmed by direct
   testing: SOME PNGs fail to decode (BL_ERROR_IMAGE_UNKNOWN_FILE_FORMAT)
   even though codec *detection* succeeds — only the decode step fails. The
   exact trigger isn't fully isolated (a 16-bit-depth indexed/palette PNG
   from `convert -draw circle...` reproduced it; `identify` reporting
   "Type: Palette" is NOT by itself predictive — several confirmed-working
   test assets also report Palette). If you hit an unexpected NULL on a
   file that looks fine, regenerate it from a photo/gradient/plasma source
   (`convert -size WxH gradient:C1-C2 -alpha off -depth 8 out.png` is a
   reliable recipe, verified against this build) rather than assuming the
   path is bad. */
void *gfx_image_load_file(const char *path); /* NULL on failure (bad path or undecodable) */
/* same decode/lifecycle as gfx_image_load_file, but from an in-memory buffer
   (e.g. HTTP-fetched bytes) instead of a path — see vyto/net + GfxPainter's
   load_image_url for the caller. NULL on failure (undecodable or len<=0). */
void *gfx_image_load_bytes(const void *data, int len);
void gfx_image_free(void *img);
int gfx_image_width(void *img);
int gfx_image_height(void *img);
void gfx_draw_image(GfxCanvas *c, void *img, double x, double y, double w, double h); /* scaled into x,y,w,h */

#endif