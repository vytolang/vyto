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

/* ------------------------------------------------------------------------ */
#else

#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
/* stb's own sprintf is only needed for the HDR writer, which is not exposed
   here; without this stb pulls in <stdio.h>'s sprintf and warns on some
   toolchains. Keep the HDR path out entirely. */
#define STBI_WRITE_NO_STDIO_HDR
#include "stb/stb_image_write.h"

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

#endif /* VT_NO_LIBC */
