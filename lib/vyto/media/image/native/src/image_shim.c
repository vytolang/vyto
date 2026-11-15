/* vyto/media/image native backing — see image_shim.h for the conventions.

   vytoc compiles every native/src/*.c for every target with no per-file filter
   (CLAUDE.md), so the whole file is bracketed: a freestanding stub arm first,
   then the real implementation.

   Note that vytoc compiles native/src FLAT and NON-RECURSIVELY (src/main.c),
   so src/stb/ is invisible to the glob and stb_image_write.h is never compiled
   as a translation unit of its own — this file is the only place its
   implementation is instantiated, which is exactly how a single-header library
   is meant to be used. The only include path is -Inative/src, which is what
   makes "stb/stb_image_write.h" resolve. */

#include "image_shim.h"

/* ------------------------------------------------------------------------ */
#ifdef VT_NO_LIBC
/* Freestanding: stb_image_write needs malloc, realloc, free, memset, memcpy,
   sprintf and (for the file entry points) all of stdio. --freestanding splices
   -DVT_NO_LIBC into every package shim's compile line, so an unguarded include
   would break the build of any program that merely imports vyto/media/image.

   Keep every symbol so such a program still links, and make each one fail
   exactly as a disk-full or permission error would — writers 0, constructors
   NULL. vyto/media/image therefore degrades to "unsupported on this target"
   with no #ifdef anywhere in image.vt.

   Deliberately no #include at all, not even <stddef.h>, so this arm cannot
   regress into depending on a hosted header. */

int vimg_write_png(const char *p, const void *x, int w, int h)
                        { (void)p; (void)x; (void)w; (void)h; return 0; }
int vimg_write_bmp(const char *p, const void *x, int w, int h)
                        { (void)p; (void)x; (void)w; (void)h; return 0; }
int vimg_write_tga(const char *p, const void *x, int w, int h)
                        { (void)p; (void)x; (void)w; (void)h; return 0; }
int vimg_write_jpg(const char *p, const void *x, int w, int h, int q)
                        { (void)p; (void)x; (void)w; (void)h; (void)q; return 0; }

void *vimg_encode_png(const void *x, int w, int h)
                        { (void)x; (void)w; (void)h; return 0; }
void *vimg_encode_bmp(const void *x, int w, int h)
                        { (void)x; (void)w; (void)h; return 0; }
void *vimg_encode_tga(const void *x, int w, int h)
                        { (void)x; (void)w; (void)h; return 0; }
void *vimg_encode_jpg(const void *x, int w, int h, int q)
                        { (void)x; (void)w; (void)h; (void)q; return 0; }

const void *vimg_buf_data(void *h)  { (void)h; return 0; }
int         vimg_buf_len(void *h)   { (void)h; return 0; }
void        vimg_buf_free(void *h)  { (void)h; }
int vimg_buf_take(void *h, void *d, int c) { (void)h; (void)d; (void)c; return 0; }

void vimg_set_png_compression(int level) { (void)level; }
int  vimg_available(void)                { return 0; }

int vimg_info(const void *d, int n, void *wh)
                        { (void)d; (void)n; (void)wh; return 0; }
int vimg_decode_into(const void *d, int n, void *px, int w, int h)
                        { (void)d; (void)n; (void)px; (void)w; (void)h; return 0; }
int vimg_shrink(const void *s, int sw, int sh, void *d, int dw, int dh)
                        { (void)s; (void)sw; (void)sh; (void)d; (void)dw; (void)dh; return 0; }
int vimg_decode_available(void)          { return 0; }

/* ------------------------------------------------------------------------ */
#else

#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
/* Paths are UTF-8 on every target; on Windows stb then opens them with
   _wfopen instead of the ANSI-code-page fopen. Ignored elsewhere. */
#define STBIW_WINDOWS_UTF8
/* stb's own sprintf is only needed for the HDR writer, which is not exposed
   here; without this stb pulls in <stdio.h>'s sprintf and warns on some
   toolchains. Keep the HDR path out entirely. */
#define STBI_WRITE_NO_STDIO_HDR
#include "stb/stb_image_write.h"

/* Decoding: PNG, JPEG, BMP and GIF (first frame), from memory only, so no
   stdio and no other formats are compiled in. STBI_MAX_DIMENSIONS caps what
   stb will even try; vimg_info lets a caller refuse earlier still. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 16384
#include "stb/stb_image.h"

/* ---- pixel repacking ---------------------------------------------------

   Vyto hands us i32[] of 0xAARRGGBB. That is a NUMBER, so its bytes in memory
   depend on the host's endianness; stb wants bytes in R,G,B,A order regardless.
   Shifting out of the integer rather than casting to unsigned char * is what
   makes this correct on both, and it is the same argument vyto/hash makes for
   reading a byte at a time.

   Returns a malloc'd w*h*4 buffer the caller frees, or NULL. */
static unsigned char *vimg_pack_rgba(const void *px, int w, int h)
{
    const unsigned int *src = (const unsigned int *)px;
    unsigned char *out;
    long long n, i;

    if (!px || w <= 0 || h <= 0) return 0;

    /* Overflow guard before the multiply reaches size_t: a 40000x40000 image
       is 6.4 GB and the product would wrap on a 32-bit size_t. */
    n = (long long)w * (long long)h;
    if (n > 0x7FFFFFFF / 4) return 0;

    out = (unsigned char *)malloc((size_t)(n * 4));
    if (!out) return 0;

    for (i = 0; i < n; i++) {
        unsigned int v = src[i];
        out[i * 4 + 0] = (unsigned char)((v >> 16) & 0xFF);  /* R */
        out[i * 4 + 1] = (unsigned char)((v >>  8) & 0xFF);  /* G */
        out[i * 4 + 2] = (unsigned char)( v        & 0xFF);  /* B */
        out[i * 4 + 3] = (unsigned char)((v >> 24) & 0xFF);  /* A */
    }
    return out;
}

/* ---- encode to a file --------------------------------------------------- */

int vimg_write_png(const char *path, const void *px, int w, int h)
{
    unsigned char *rgba;
    int rc;
    if (!path) return 0;
    rgba = vimg_pack_rgba(px, w, h);
    if (!rgba) return 0;
    rc = stbi_write_png(path, w, h, 4, rgba, w * 4);
    free(rgba);
    return rc ? 1 : 0;
}

int vimg_write_bmp(const char *path, const void *px, int w, int h)
{
    unsigned char *rgba;
    int rc;
    if (!path) return 0;
    rgba = vimg_pack_rgba(px, w, h);
    if (!rgba) return 0;
    rc = stbi_write_bmp(path, w, h, 4, rgba);
    free(rgba);
    return rc ? 1 : 0;
}

int vimg_write_tga(const char *path, const void *px, int w, int h)
{
    unsigned char *rgba;
    int rc;
    if (!path) return 0;
    rgba = vimg_pack_rgba(px, w, h);
    if (!rgba) return 0;
    rc = stbi_write_tga(path, w, h, 4, rgba);
    free(rgba);
    return rc ? 1 : 0;
}

int vimg_write_jpg(const char *path, const void *px, int w, int h, int quality)
{
    unsigned char *rgba;
    int rc;
    if (!path) return 0;
    rgba = vimg_pack_rgba(px, w, h);
    if (!rgba) return 0;
    /* comp 4 with a JPEG writer: stb reads only the first three components,
       so the alpha is dropped rather than composited. JPEG has no alpha
       channel, and there is no sensible background colour to pick here. */
    rc = stbi_write_jpg(path, w, h, 4, rgba, quality);
    free(rgba);
    return rc ? 1 : 0;
}

/* ---- encode to memory ---------------------------------------------------

   stb's *_to_func variants call back with chunks, so the callback accumulates
   into a growing buffer. Doubling, so an encode is O(n) rather than O(n^2) —
   the CLAUDE.md StringBuilder argument, in C. */

typedef struct {
    unsigned char *data;
    int len;
    int cap;
    int failed;
} VimgBuf;

static void vimg_sink(void *context, void *data, int size)
{
    VimgBuf *b = (VimgBuf *)context;
    if (b->failed || size <= 0) return;

    if (b->len + size > b->cap) {
        int want = b->cap ? b->cap : 4096;
        unsigned char *grown;
        while (want < b->len + size) {
            if (want > 0x7FFFFFFF / 2) { b->failed = 1; return; }
            want *= 2;
        }
        grown = (unsigned char *)realloc(b->data, (size_t)want);
        if (!grown) { b->failed = 1; return; }
        b->data = grown;
        b->cap = want;
    }
    memcpy(b->data + b->len, data, (size_t)size);
    b->len += size;
}

/* kind: 0 png, 1 bmp, 2 tga, 3 jpg */
static void *vimg_encode(const void *px, int w, int h, int kind, int quality)
{
    unsigned char *rgba;
    VimgBuf *b;
    int rc;

    rgba = vimg_pack_rgba(px, w, h);
    if (!rgba) return 0;

    b = (VimgBuf *)calloc(1, sizeof(VimgBuf));
    if (!b) { free(rgba); return 0; }

    switch (kind) {
        case 1:  rc = stbi_write_bmp_to_func(vimg_sink, b, w, h, 4, rgba); break;
        case 2:  rc = stbi_write_tga_to_func(vimg_sink, b, w, h, 4, rgba); break;
        case 3:  rc = stbi_write_jpg_to_func(vimg_sink, b, w, h, 4, rgba, quality); break;
        default: rc = stbi_write_png_to_func(vimg_sink, b, w, h, 4, rgba, w * 4); break;
    }
    free(rgba);

    if (!rc || b->failed) {
        free(b->data);
        free(b);
        return 0;
    }
    return b;
}

void *vimg_encode_png(const void *px, int w, int h) { return vimg_encode(px, w, h, 0, 0); }
void *vimg_encode_bmp(const void *px, int w, int h) { return vimg_encode(px, w, h, 1, 0); }
void *vimg_encode_tga(const void *px, int w, int h) { return vimg_encode(px, w, h, 2, 0); }
void *vimg_encode_jpg(const void *px, int w, int h, int q) { return vimg_encode(px, w, h, 3, q); }

const void *vimg_buf_data(void *h)
{
    VimgBuf *b = (VimgBuf *)h;
    if (!b) return 0;
    return b->data;
}

int vimg_buf_len(void *h)
{
    VimgBuf *b = (VimgBuf *)h;
    if (!b) return 0;
    return b->len;
}

void vimg_buf_free(void *h)
{
    VimgBuf *b = (VimgBuf *)h;
    if (!b) return;
    free(b->data);
    free(b);
}

int vimg_buf_take(void *h, void *dst, int cap)
{
    VimgBuf *b = (VimgBuf *)h;
    int n;
    if (!b) return 0;
    n = b->len;
    if (n > cap) n = cap;
    if (n > 0 && dst && b->data) memcpy(dst, b->data, (size_t)n);
    free(b->data);
    free(b);
    return n;
}

/* ---- knobs -------------------------------------------------------------- */

void vimg_set_png_compression(int level)
{
    if (level < 0) level = 0;
    if (level > 9) level = 9;
    stbi_write_png_compression_level = level;
}

int vimg_available(void) { return 1; }

/* ---- decode ---------------------------------------------------------------

   Two calls, so the caller allocates: vimg_info reads the dimensions from the
   header (cheap: nothing is decompressed), the caller checks them against its
   own limit and allocates w*h ints, and vimg_decode_into fills them. */

int vimg_info(const void *data, int len, void *wh)
{
    int w = 0, h = 0, comp = 0;
    int *out = (int *)wh;
    if (!data || len <= 0 || !out) return 0;
    if (!stbi_info_from_memory((const stbi_uc *)data, len, &w, &h, &comp)) return 0;
    out[0] = w;
    out[1] = h;
    return 1;
}

/* Decode into px, w*h ints of 0xAARRGGBB. The dimensions must be what
   vimg_info said; anything else is refused rather than overrun. */
int vimg_decode_into(const void *data, int len, void *px, int w, int h)
{
    int dw = 0, dh = 0, comp = 0;
    long long n, i;
    unsigned char *rgba;
    unsigned int *dst = (unsigned int *)px;
    if (!data || len <= 0 || !px || w <= 0 || h <= 0) return 0;
    rgba = stbi_load_from_memory((const stbi_uc *)data, len, &dw, &dh, &comp, 4);
    if (!rgba) return 0;
    if (dw != w || dh != h) { stbi_image_free(rgba); return 0; }
    n = (long long)w * (long long)h;
    for (i = 0; i < n; i++) {
        dst[i] = ((unsigned int)rgba[i * 4 + 3] << 24) |
                 ((unsigned int)rgba[i * 4 + 0] << 16) |
                 ((unsigned int)rgba[i * 4 + 1] <<  8) |
                  (unsigned int)rgba[i * 4 + 2];
    }
    stbi_image_free(rgba);
    return 1;
}

int vimg_decode_available(void) { return 1; }

/* ---- shrink -----------------------------------------------------------------

   Area-average downscale (a box filter): every destination pixel is the mean
   of the source pixels it covers, with fractional coverage at the edges.
   That is the right filter for making pictures SMALLER, which is all this is
   for (a photo to a product thumbnail); it does not upscale. Colour is
   averaged weighted by alpha, so transparent pixels do not darken edges.
   Pixel boundaries are in 16.16 fixed point. */
int vimg_shrink(const void *src, int sw, int sh, void *dst, int dw, int dh)
{
    const unsigned int *s = (const unsigned int *)src;
    unsigned int *d = (unsigned int *)dst;
    int dx, dy;
    if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return 0;
    if (dw > sw || dh > sh) return 0;
    for (dy = 0; dy < dh; dy++) {
        long long y0 = ((long long)dy * sh << 16) / dh;
        long long y1 = ((long long)(dy + 1) * sh << 16) / dh;
        for (dx = 0; dx < dw; dx++) {
            long long x0 = ((long long)dx * sw << 16) / dw;
            long long x1 = ((long long)(dx + 1) * sw << 16) / dw;
            double a = 0, r = 0, g = 0, b = 0, area = 0;
            int sy, sx;
            for (sy = (int)(y0 >> 16); sy < sh && ((long long)sy << 16) < y1; sy++) {
                long long top = (long long)sy << 16, bot = top + 65536;
                double wy = (double)((bot < y1 ? bot : y1) - (top > y0 ? top : y0)) / 65536.0;
                for (sx = (int)(x0 >> 16); sx < sw && ((long long)sx << 16) < x1; sx++) {
                    long long lft = (long long)sx << 16, rgt = lft + 65536;
                    double wx = (double)((rgt < x1 ? rgt : x1) - (lft > x0 ? lft : x0)) / 65536.0;
                    double wgt = wx * wy;
                    unsigned int v = s[(long long)sy * sw + sx];
                    double pa = (double)((v >> 24) & 0xFF);
                    a += pa * wgt;
                    r += (double)((v >> 16) & 0xFF) * pa * wgt;
                    g += (double)((v >>  8) & 0xFF) * pa * wgt;
                    b += (double)( v        & 0xFF) * pa * wgt;
                    area += wgt;
                }
            }
            if (area <= 0) { d[(long long)dy * dw + dx] = 0; continue; }
            {
                unsigned int oa = (unsigned int)(a / area + 0.5);
                unsigned int or_ = 0, og = 0, ob = 0;
                if (a > 0) {
                    or_ = (unsigned int)(r / a + 0.5);
                    og = (unsigned int)(g / a + 0.5);
                    ob = (unsigned int)(b / a + 0.5);
                }
                if (oa > 255) oa = 255;
                if (or_ > 255) or_ = 255;
                if (og > 255) og = 255;
                if (ob > 255) ob = 255;
                d[(long long)dy * dw + dx] = (oa << 24) | (or_ << 16) | (og << 8) | ob;
            }
        }
    }
    return 1;
}

#endif /* VT_NO_LIBC */
