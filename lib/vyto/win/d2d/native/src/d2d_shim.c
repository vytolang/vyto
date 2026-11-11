/* d2d_shim.c — Direct2D + DirectWrite behind a flat handle API, for
 * vyto/win/d2d's D2DPainter.
 *
 * Every C file under native/src compiles on every target (CLAUDE.md), so the real body
 * is inside `#if defined(_WIN32)` and the #else arm returns each entry point's
 * failure sentinel. The stub arm includes nothing but <stdint.h>, which keeps
 * a --freestanding build of anything importing this package compiling too.
 *
 * Render targets. The default is a software render target over an IWICBitmap
 * we own, in 32bppBGR with D2D1_ALPHA_MODE_IGNORE: memory is BGRX, which is
 * byte-for-byte what vs_blit_rect takes, and an opaque target is what lets
 * DirectWrite use ClearType. Presenting is lock -> Surface.blitRect -> unlock,
 * the same path GfxPainter uses, so vsurf's persistent backbuffer and the
 * partial-repaint invariant in ui/core.vt are untouched. It also works under
 * VS_HEADLESS, where vsurf has no DC at all, and it is what gives blur and
 * backdrop their pixel access.
 *
 * An ID2D1DCRenderTarget bound to vsurf's memdc was measured as the
 * alternative on Windows 7 (2026-11-11) and dropped: a full redraw took
 * 9.3 ms against 5.9 ms here, with a cold first frame near 200 ms.
 *
 * COM from C. d2d1.h always defines its C macros; dwrite.h and wincodec.h need
 * COBJMACROS. Struct-returning vtable methods (GetSize, GetPixelSize,
 * GetPixelFormat) are declared by-value in mingw's C vtables, which does not
 * match the real ABI — never call them; sizes are tracked here instead. The
 * GUIDs are private copies so the shim needs no libuuid data symbols, and so
 * CLSID_WICImagingFactory1 is named explicitly: newer SDKs alias the unsuffixed
 * CLSID to factory2, which Windows 7 does not have.
 */
#include <stdint.h>

#if defined(_WIN32) && !defined(VT_NO_LIBC)

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define COBJMACROS
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const GUID d2d_IID_ID2D1Factory =
    {0x06152247, 0x6f50, 0x465a, {0x92, 0x45, 0x11, 0x8b, 0xfd, 0x3b, 0x60, 0x07}};
static const GUID d2d_IID_IDWriteFactory =
    {0xb859ee5a, 0xd838, 0x4b5b, {0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48}};
static const GUID d2d_CLSID_WICImagingFactory1 =
    {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
static const GUID d2d_IID_IWICImagingFactory =
    {0xec5ec8a9, 0xc395, 0x4314, {0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70}};
static const GUID d2d_GUID_WICPixelFormat32bppBGR =
    {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0e}};
static const GUID d2d_GUID_WICPixelFormat32bppPBGRA =
    {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10}};

/* Render-target modes, as d2d_mode reports them. */
#define D2D_MODE_WIC_BGR   1   /* opaque WIC target, ClearType text */
#define D2D_MODE_WIC_PBGRA 2   /* fallback if BGR+IGNORE is refused: grayscale text */

#define D2D_MAX_CLIP   64
#define D2D_FMT_CACHE  16
#define D2D_WCACHE     2048        /* power of two */
#define D2D_CAP_ROUND  256         /* WIC bitmap capacity granularity, px */

/* One text format per (size, weight), with the metrics the painter needs. */
typedef struct {
    float size;
    int weight;
    IDWriteTextFormat *fmt;
    float asc, desc, gap;
} D2dFmt;

/* Width-cache slot. h == 0 marks an empty slot. Keyed on a 64-bit hash of the
   UTF-8 plus its length, size and weight: a collision would need all four to
   agree, which is accepted as negligible for a measurement cache. */
typedef struct {
    uint64_t h;
    float size;
    int32_t weight;
    int32_t len;
    float width;
} D2dWidth;

/* One clip_push. Kept so the stack can be replayed: Direct2D requires every
   clip to be popped before EndDraw, and a frame can end mid-stack (a present
   or a pixel read with a clip still pushed). */
#define D2D_CLIP_AXIS  0       /* PushAxisAlignedClip */
#define D2D_CLIP_ROUND 1       /* PushLayer with a rounded-rect mask */
#define D2D_CLIP_CORNERS 2     /* axis-aligned clip + saved corners, blended back on pop */
typedef struct {
    int kind;
    D2D1_RECT_F rect;
    ID2D1Geometry *geom;       /* rounded clips only; device-independent */
    D2D1_MATRIX_3X2_F m;       /* the transform in force at push time */
    int bx, by, bw, bh;        /* device-space box, intersected with the parent */
    /* D2D_CLIP_CORNERS: the pixels the corners will spill into, saved at
       push. One block for a pill (the corners would overlap), else four. */
    uint8_t *saved;
    int nblk;
    int blk[4][4];             /* x, y, w, h in device pixels */
    double sx, sy, sw, sh, sr; /* the rounded shape, in device pixels */
} D2dClip;

/* A cached gradient: the stop collection plus one brush of each kind made
   from it, re-pointed per draw. Buttons draw the same gradient every frame,
   and building a stop collection is the expensive part. Render-target
   resources, so release_target drops them all. */
#define D2D_GRADS      16
#define D2D_GRAD_STOPS 8
typedef struct {
    UINT n;                            /* 0 = empty slot */
    D2D1_GRADIENT_STOP st[D2D_GRAD_STOPS];
    ID2D1GradientStopCollection *coll;
    ID2D1LinearGradientBrush *lin;
    ID2D1RadialGradientBrush *rad;
} D2dGrad;

/* A cached drop shadow: the blurred, recoloured, knocked-out shadow of one
   rounded rect, as a bitmap. A widget's shadow is the same every frame, so
   after the first draw a shadow costs one DrawBitmap. Keyed to quarter
   pixels; evicted least-recently-used.

   Bounded by pixel bytes as well as by count: a 300x200 card's shadow is
   about 370 KB, so a count limit alone let 32 large shadows hold over 10 MB.
   A shadow bigger than half the budget is drawn uncached rather than
   evicting everything else. */
#define D2D_SHADOWS       32
#define D2D_SHADOW_BUDGET (2u * 1024u * 1024u)
typedef struct {
    ID2D1Bitmap *bmp;                  /* NULL = empty slot */
    int qw, qh, qr, qdy, rad;
    uint32_t col;
    int lw, lh, pad;
    unsigned used;
} D2dShadow;

/* One save(): the transform to restore, and how deep the clip stack was, so
   a restore also drops clips pushed since — blend2d keeps clips and
   transforms on one state stack, and callers may rely on that. */
typedef struct {
    D2D1_MATRIX_3X2_F m;
    int clip_depth;
} D2dSave;

#define D2D_MAX_SAVE 32

typedef struct D2dCtx {
    ID2D1RenderTarget *rt;
    IWICBitmap *bmp;               /* WIC modes only */
    IWICBitmapLock *lock;
    ID2D1SolidColorBrush *brush;
    int mode;
    int w, h;                      /* logical size: what is presented */
    int cap_w, cap_h;              /* WIC bitmap size, grow-only */
    int drawing;
    unsigned gen;                  /* bumped whenever rt is recreated */
    D2dClip clip[D2D_MAX_CLIP];
    int nclip;                     /* clips applied (<= D2D_MAX_CLIP) */
    int clip_depth;                /* pushes outstanding, may exceed nclip */
    ID2D1Layer *layers[D2D_MAX_CLIP]; /* one per stack slot, made on demand */
    D2D1_MATRIX_3X2_F xf;          /* current transform */
    D2dSave saves[D2D_MAX_SAVE];
    int nsave;                     /* saves recorded (<= D2D_MAX_SAVE) */
    int save_depth;                /* saves outstanding, may exceed nsave */
    WCHAR family[64];
    DWRITE_FONT_METRICS fm[3];
    int have_fm[3];
    DWRITE_FONT_WEIGHT slot_w[3];  /* the weight each slot really asks for */
    D2dGrad grads[D2D_GRADS];
    int grad_next;
    D2dShadow shadows[D2D_SHADOWS];
    unsigned tick;
    size_t shadow_bytes;           /* pixel bytes held by the shadow cache */
    int ops;                       /* draws queued since the last flush */
    float size;
    int weight;
    D2dFmt fmts[D2D_FMT_CACHE];
    int nfmt, fmt_next;
    D2dWidth *wc;
    int wc_used;
    UINT stride;
} D2dCtx;

/* Process-wide factories, created on first use and kept for the process's
   lifetime: they are shared by every painter, and tearing them down at exit
   buys nothing. */
static ID2D1Factory *g_d2d;
static IDWriteFactory *g_dw;
static IWICImagingFactory *g_wic;
static IDWriteFontCollection *g_fonts;
static int g_init_done, g_init_ok;
/* Render-target generations, unique process-wide, so an image's cached
   Direct2D bitmap can never be mistaken for one made by another target. */
static unsigned g_gen;

static int ensure_factories(void) {
    if (g_init_done) return g_init_ok;
    g_init_done = 1;
    /* WIC is created through COM. S_FALSE (already initialised) and
       RPC_E_CHANGED_MODE (someone chose MTA first) both leave COM usable. */
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return 0;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                 &d2d_IID_ID2D1Factory, NULL, (void **)&g_d2d)))
        return 0;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                   &d2d_IID_IDWriteFactory, (IUnknown **)&g_dw)))
        return 0;
    if (FAILED(CoCreateInstance(&d2d_CLSID_WICImagingFactory1, NULL,
                                CLSCTX_INPROC_SERVER, &d2d_IID_IWICImagingFactory,
                                (void **)&g_wic)))
        return 0;
    if (FAILED(IDWriteFactory_GetSystemFontCollection(g_dw, &g_fonts, FALSE)))
        return 0;
    g_init_ok = 1;
    return 1;
}

/* ---- small helpers ------------------------------------------------------ */

static D2D1_COLOR_F colf(int32_t c) {
    uint32_t u = (uint32_t)c;
    D2D1_COLOR_F f;
    f.a = (float)((u >> 24) & 255) / 255.0f;
    f.r = (float)((u >> 16) & 255) / 255.0f;
    f.g = (float)((u >> 8) & 255) / 255.0f;
    f.b = (float)(u & 255) / 255.0f;
    return f;
}

static D2D1_RECT_F rectf(double x, double y, double w, double h) {
    D2D1_RECT_F r;
    r.left = (float)x;
    r.top = (float)y;
    r.right = (float)(x + w);
    r.bottom = (float)(y + h);
    return r;
}

static int round_up(int v, int g) { return ((v + g - 1) / g) * g; }

static D2D1_MATRIX_3X2_F mat_identity(void) {
    D2D1_MATRIX_3X2_F m;
    m._11 = 1.0f; m._12 = 0.0f;
    m._21 = 0.0f; m._22 = 1.0f;
    m._31 = 0.0f; m._32 = 0.0f;
    return m;
}

/* a then b, in Direct2D's row-vector convention (p' = p * a * b). */
static D2D1_MATRIX_3X2_F mat_mul(const D2D1_MATRIX_3X2_F *a, const D2D1_MATRIX_3X2_F *b) {
    D2D1_MATRIX_3X2_F r;
    r._11 = a->_11 * b->_11 + a->_12 * b->_21;
    r._12 = a->_11 * b->_12 + a->_12 * b->_22;
    r._21 = a->_21 * b->_11 + a->_22 * b->_21;
    r._22 = a->_21 * b->_12 + a->_22 * b->_22;
    r._31 = a->_31 * b->_11 + a->_32 * b->_21 + b->_31;
    r._32 = a->_31 * b->_12 + a->_32 * b->_22 + b->_32;
    return r;
}

/* UTF-8 -> UTF-16 into a stack buffer, spilling to the heap for long text. */
typedef struct {
    WCHAR *p;
    int n;
    WCHAR stack[256];
} WBuf;

static int wbuf_from(WBuf *b, const char *s, int len) {
    b->p = b->stack;
    b->n = 0;
    if (len <= 0) return 1;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, len, b->stack, 256);
    if (n > 0) { b->n = n; return 1; }
    n = MultiByteToWideChar(CP_UTF8, 0, s, len, NULL, 0);
    if (n <= 0) return 0;
    b->p = (WCHAR *)malloc(sizeof(WCHAR) * (size_t)n);
    if (!b->p) { b->p = b->stack; return 0; }
    b->n = MultiByteToWideChar(CP_UTF8, 0, s, len, b->p, n);
    return b->n > 0;
}

static void wbuf_free(WBuf *b) {
    if (b->p != b->stack) free(b->p);
}

static void reset_state(struct D2dCtx *c);
static double rrect_sd(double px, double py, double x, double y, double w, double h,
                       double r);
static uint8_t *lock_rw(struct D2dCtx *c, int x, int y, int w, int h);
static void unlock(struct D2dCtx *c);
static void dev_box(const D2D1_MATRIX_3X2_F *m, double x, double y, double w, double h,
                    double *x0, double *y0, double *x1, double *y1);

/* ---- render target ------------------------------------------------------ */

static void release_target(D2dCtx *c) {
    /* Layers belong to the render target that made them. */
    for (int i = 0; i < D2D_MAX_CLIP; i++)
        if (c->layers[i]) { ID2D1Layer_Release(c->layers[i]); c->layers[i] = NULL; }
    for (int i = 0; i < D2D_GRADS; i++) {
        D2dGrad *g = &c->grads[i];
        if (g->lin) ID2D1LinearGradientBrush_Release(g->lin);
        if (g->rad) ID2D1RadialGradientBrush_Release(g->rad);
        if (g->coll) ID2D1GradientStopCollection_Release(g->coll);
        memset(g, 0, sizeof *g);
    }
    for (int i = 0; i < D2D_SHADOWS; i++)
        if (c->shadows[i].bmp) {
            ID2D1Bitmap_Release(c->shadows[i].bmp);
            c->shadows[i].bmp = NULL;
        }
    c->shadow_bytes = 0;
    if (c->brush) { ID2D1SolidColorBrush_Release(c->brush); c->brush = NULL; }
    if (c->rt) { ID2D1RenderTarget_Release(c->rt); c->rt = NULL; }
    if (c->bmp) { IWICBitmap_Release(c->bmp); c->bmp = NULL; }
}

static int make_brush(D2dCtx *c) {
    D2D1_COLOR_F black = colf((int32_t)0xFF000000u);
    return SUCCEEDED(ID2D1RenderTarget_CreateSolidColorBrush(c->rt, &black, NULL,
                                                             &c->brush));
}

/* Text uses the user's own ClearType settings: the native Windows look,
   chosen over matching blend2d's heavier grayscale text. A PBGRA target
   cannot do ClearType (it needs an opaque target), so it falls back to
   grayscale. */
static void apply_text_mode(D2dCtx *c) {
    if (c->mode == D2D_MODE_WIC_PBGRA)
        ID2D1RenderTarget_SetTextAntialiasMode(c->rt, D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
}

static D2D1_RENDER_TARGET_PROPERTIES rt_props(D2D1_RENDER_TARGET_TYPE type,
                                              D2D1_ALPHA_MODE alpha) {
    D2D1_RENDER_TARGET_PROPERTIES p;
    p.type = type;
    p.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    p.pixelFormat.alphaMode = alpha;
    /* Explicit 96: coordinates are device pixels. Leaving 0 lets a target
       pick up the system DPI and silently scale every coordinate. */
    p.dpiX = 96.0f;
    p.dpiY = 96.0f;
    p.usage = D2D1_RENDER_TARGET_USAGE_NONE;
    p.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;
    return p;
}

static int make_wic(D2dCtx *c, int cw, int ch) {
    release_target(c);
    D2D1_RENDER_TARGET_PROPERTIES p =
        rt_props(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1_ALPHA_MODE_IGNORE);
    if (SUCCEEDED(IWICImagingFactory_CreateBitmap(g_wic, (UINT)cw, (UINT)ch,
                                                  &d2d_GUID_WICPixelFormat32bppBGR,
                                                  WICBitmapCacheOnLoad, &c->bmp)) &&
        SUCCEEDED(ID2D1Factory_CreateWicBitmapRenderTarget(g_d2d, c->bmp, &p, &c->rt))) {
        c->mode = D2D_MODE_WIC_BGR;
    } else {
        /* Not every D2D accepts an IGNORE-alpha WIC target. PBGRA is always
           supported; text then has to be grayscale, since ClearType needs an
           opaque target. The backbuffer is opaque anyway (every frame starts
           with a full fill), so the extra alpha byte is never seen. */
        release_target(c);
        p.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        if (FAILED(IWICImagingFactory_CreateBitmap(g_wic, (UINT)cw, (UINT)ch,
                                                   &d2d_GUID_WICPixelFormat32bppPBGRA,
                                                   WICBitmapCacheOnLoad, &c->bmp)))
            return 0;
        if (FAILED(ID2D1Factory_CreateWicBitmapRenderTarget(g_d2d, c->bmp, &p, &c->rt))) {
            release_target(c);
            return 0;
        }
        c->mode = D2D_MODE_WIC_PBGRA;
    }
    if (!make_brush(c)) { release_target(c); return 0; }
    apply_text_mode(c);
    c->cap_w = cw;
    c->cap_h = ch;
    c->gen = ++g_gen;
    return 1;
}

/* Apply clip i to the target, under the transform it was pushed with. The
   layer for its slot is made on first use and lives as long as the target. */
static void apply_clip(D2dCtx *c, int i) {
    D2dClip *k = &c->clip[i];
    ID2D1RenderTarget_SetTransform(c->rt, &k->m);
    if (k->kind != D2D_CLIP_ROUND) {
        ID2D1RenderTarget_PushAxisAlignedClip(c->rt, &k->rect,
                                              D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        return;
    }
    if (!c->layers[i] && FAILED(ID2D1RenderTarget_CreateLayer(c->rt, NULL, &c->layers[i])))
        c->layers[i] = NULL;
    D2D1_LAYER_PARAMETERS lp;
    lp.contentBounds = k->rect;
    lp.geometricMask = k->geom;
    lp.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
    lp.maskTransform = mat_identity();
    lp.opacity = 1.0f;
    lp.opacityBrush = NULL;
    lp.layerOptions = D2D1_LAYER_OPTIONS_NONE;
    if (c->layers[i]) {
        ID2D1RenderTarget_PushLayer(c->rt, &lp, c->layers[i]);
    } else {
        /* No layer to be had: a rectangular clip beats no clip, and keeps
           the push/pop pairing intact. */
        k->kind = D2D_CLIP_AXIS;
        ID2D1RenderTarget_PushAxisAlignedClip(c->rt, &k->rect,
                                              D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }
}

static void unapply_clip(D2dCtx *c, int i) {
    if (c->clip[i].kind == D2D_CLIP_ROUND) ID2D1RenderTarget_PopLayer(c->rt);
    else ID2D1RenderTarget_PopAxisAlignedClip(c->rt);
}

/* BeginDraw lazily, on the first op of a frame, re-applying the clip stack
   that end_draw had to pop, then the current transform. */
/* Direct2D queues every draw until EndDraw and keeps its internal buffers at
   the largest batch it has seen: on Windows 7 a single frame of 2000 rounded
   rects left 1.7 MB behind for good. Handing the queue over every
   D2D_FLUSH_OPS draws caps that high-water mark; an ordinary UI frame issues
   fewer draws than this and never flushes early. */
#define D2D_FLUSH_OPS 256

static int begin(D2dCtx *c) {
    if (c->drawing) {
        if (++c->ops >= D2D_FLUSH_OPS) {
            ID2D1RenderTarget_Flush(c->rt, NULL, NULL);
            c->ops = 0;
        }
        return 1;
    }
    if (!c->rt) return 0;
    c->ops = 0;
    ID2D1RenderTarget_BeginDraw(c->rt);
    for (int i = 0; i < c->nclip; i++) apply_clip(c, i);
    ID2D1RenderTarget_SetTransform(c->rt, &c->xf);
    c->drawing = 1;
    return 1;
}

/* EndDraw requires every pushed clip to be popped first, innermost first, so
   the stack is unwound here and replayed by the next begin. */
static void end_draw(D2dCtx *c) {
    if (!c->drawing) return;
    for (int i = c->nclip - 1; i >= 0; i--) unapply_clip(c, i);
    HRESULT hr = ID2D1RenderTarget_EndDraw(c->rt, NULL, NULL);
    c->drawing = 0;
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) {
        /* A software target should never lose its device; if it reports so
           anyway, rebuild it so the next frame draws. */
        make_wic(c, c->cap_w, c->cap_h);
    }
}

static void set_color(D2dCtx *c, int32_t color) {
    D2D1_COLOR_F f = colf(color);
    ID2D1SolidColorBrush_SetColor(c->brush, &f);
}

static int visible(int32_t color) { return ((uint32_t)color >> 24) != 0; }

/* ---- fonts -------------------------------------------------------------- */

static DWRITE_FONT_WEIGHT dw_weight(int w) {
    if (w == 1) return DWRITE_FONT_WEIGHT_MEDIUM;
    if (w == 2) return DWRITE_FONT_WEIGHT_BOLD;
    return DWRITE_FONT_WEIGHT_REGULAR;
}

static void drop_fonts(D2dCtx *c) {
    for (int i = 0; i < c->nfmt; i++) IDWriteTextFormat_Release(c->fmts[i].fmt);
    c->nfmt = 0;
    c->fmt_next = 0;
    if (c->wc) memset(c->wc, 0, sizeof(D2dWidth) * D2D_WCACHE);
    c->wc_used = 0;
}

/* Resolve `name` in the system collection and read the metrics of its three
   weight slots. Returns 0 when the family does not exist. */
static int load_family(D2dCtx *c, const WCHAR *name) {
    UINT32 idx = 0;
    BOOL exists = FALSE;
    if (FAILED(IDWriteFontCollection_FindFamilyName(g_fonts, name, &idx, &exists)) ||
        !exists)
        return 0;
    IDWriteFontFamily *fam = NULL;
    if (FAILED(IDWriteFontCollection_GetFontFamily(g_fonts, idx, &fam))) return 0;
    for (int w = 0; w < 3; w++) {
        IDWriteFont *font = NULL;
        c->have_fm[w] = 0;
        c->slot_w[w] = dw_weight(w);
        if (SUCCEEDED(IDWriteFontFamily_GetFirstMatchingFont(
                fam, dw_weight(w), DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                &font))) {
            /* Medium is a nudge above regular. A family without a 500 face
               (Segoe UI on Windows 7 has 400, 600 and 700) would otherwise
               resolve it to semibold or bold, and every medium label — a
               button's, say — reads as shouting. Fall back to regular. */
            if (w == 1 && IDWriteFont_GetWeight(font) > 550) {
                IDWriteFont_Release(font);
                font = NULL;
            } else {
                IDWriteFont_GetMetrics(font, &c->fm[w]);
                c->have_fm[w] = c->fm[w].designUnitsPerEm > 0;
                IDWriteFont_Release(font);
            }
        }
    }
    IDWriteFontFamily_Release(fam);
    if (!c->have_fm[0]) return 0;
    for (int w = 1; w < 3; w++)
        if (!c->have_fm[w]) {
            c->fm[w] = c->fm[0];
            c->slot_w[w] = c->slot_w[0];
            c->have_fm[w] = 1;
        }
    wcsncpy(c->family, name, 63);
    c->family[63] = 0;
    drop_fonts(c);
    return 1;
}

static const WCHAR *const k_fallbacks[] = {L"Segoe UI", L"Tahoma", L"Arial"};

static D2dFmt *fmt_for(D2dCtx *c) {
    for (int i = 0; i < c->nfmt; i++)
        if (c->fmts[i].size == c->size && c->fmts[i].weight == c->weight)
            return &c->fmts[i];
    IDWriteTextFormat *fmt = NULL;
    if (FAILED(IDWriteFactory_CreateTextFormat(g_dw, c->family, NULL, c->slot_w[c->weight],
                                               DWRITE_FONT_STYLE_NORMAL,
                                               DWRITE_FONT_STRETCH_NORMAL, c->size,
                                               L"en-us", &fmt)))
        return NULL;
    IDWriteTextFormat_SetWordWrapping(fmt, DWRITE_WORD_WRAPPING_NO_WRAP);
    const DWRITE_FONT_METRICS *m = &c->fm[c->weight];
    float k = c->size / (float)m->designUnitsPerEm;
    float asc = (float)m->ascent * k;
    float desc = (float)m->descent * k;
    float gap = (float)m->lineGap * k;
    if (gap < 0.0f) gap = 0.0f;
    /* Uniform spacing with baseline = ascent pins the baseline exactly
       `asc` below the layout box's top, which is what lets text() convert
       the Painter's baseline-y into DirectWrite's top-y. */
    IDWriteTextFormat_SetLineSpacing(fmt, DWRITE_LINE_SPACING_METHOD_UNIFORM,
                                     asc + desc + gap, asc);
    int slot;
    if (c->nfmt < D2D_FMT_CACHE) {
        slot = c->nfmt++;
    } else {
        slot = c->fmt_next;
        c->fmt_next = (c->fmt_next + 1) % D2D_FMT_CACHE;
        IDWriteTextFormat_Release(c->fmts[slot].fmt);
    }
    D2dFmt *f = &c->fmts[slot];
    f->size = c->size;
    f->weight = c->weight;
    f->fmt = fmt;
    f->asc = asc;
    f->desc = desc;
    f->gap = gap;
    return f;
}

static uint64_t width_key(const char *s, int len, float size, int weight) {
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    uint32_t sb;
    memcpy(&sb, &size, sizeof sb);
    h ^= ((uint64_t)sb << 8) ^ (uint64_t)(weight + 1);
    h *= 1099511628211ULL;
    return h ? h : 1;
}

/* ---- exported API ------------------------------------------------------- */

int32_t d2d_ok(void) { return ensure_factories(); }

void *d2d_new(int32_t w, int32_t h) {
    if (!ensure_factories()) return NULL;
    D2dCtx *c = (D2dCtx *)calloc(1, sizeof(D2dCtx));
    if (!c) return NULL;
    c->w = w < 1 ? 1 : w;
    c->h = h < 1 ? 1 : h;
    c->size = 15.0f;
    c->xf = mat_identity();
    int ok = make_wic(c, round_up(c->w, D2D_CAP_ROUND), round_up(c->h, D2D_CAP_ROUND));
    if (!ok) { free(c); return NULL; }
    for (int i = 0; i < 3 && !c->family[0]; i++) load_family(c, k_fallbacks[i]);
    if (!c->family[0]) { release_target(c); free(c); return NULL; }
    return c;
}

void d2d_free(void *p) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c) return;
    reset_state(c);
    drop_fonts(c);
    free(c->wc);
    release_target(c);
    free(c);
}

int32_t d2d_mode(void *p) {
    D2dCtx *c = (D2dCtx *)p;
    return c ? c->mode : 0;
}

/* Contents are undefined afterwards; the Window redraws in full on resize.
   The WIC bitmap only grows (rounded up), so a drag-resize reallocates a few
   times rather than on every step. Returns 0 if a needed grow failed, leaving
   the old target in place. */
int32_t d2d_resize(void *p, int32_t w, int32_t h) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c) return 0;
    reset_state(c);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if ((w > c->cap_w || h > c->cap_h)) {
        int cw = w > c->cap_w ? round_up(w, D2D_CAP_ROUND) : c->cap_w;
        int ch = h > c->cap_h ? round_up(h, D2D_CAP_ROUND) : c->cap_h;
        if (!make_wic(c, cw, ch)) {
            make_wic(c, c->cap_w, c->cap_h);
            return 0;
        }
    }
    c->w = w;
    c->h = h;
    return 1;
}

/* Finish the frame's drawing: everything queued is rasterised. */
void d2d_end(void *p) {
    D2dCtx *c = (D2dCtx *)p;
    if (c) end_draw(c);
}

void d2d_fill_rect(void *p, double x, double y, double w, double h, int32_t color) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c || !visible(color) || !begin(c)) return;
    set_color(c, color);
    D2D1_RECT_F r = rectf(x, y, w, h);
    ID2D1RenderTarget_FillRectangle(c->rt, &r, (ID2D1Brush *)c->brush);
}

static D2D1_ROUNDED_RECT round_rect(double x, double y, double w, double h, double rad);

void d2d_fill_round_rect(void *p, double x, double y, double w, double h, double rad,
                         int32_t color) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c || !visible(color) || !begin(c)) return;
    set_color(c, color);
    D2D1_ROUNDED_RECT rr = round_rect(x, y, w, h, rad);
    ID2D1RenderTarget_FillRoundedRectangle(c->rt, &rr, (ID2D1Brush *)c->brush);
}

/* A line with the current brush. A horizontal or vertical one is filled as
   the rectangle it covers, which is exactly the shape a flat-capped stroke
   makes: a fill measured about 0.85 us against about 6 us for DrawLine on
   Windows 7, and dividers, grids and bevels are all axis-aligned. */
static void line_with_brush(D2dCtx *c, double x0, double y0, double x1, double y1,
                            double width) {
    double hw = width / 2.0;
    if (y0 == y1 || x0 == x1) {
        D2D1_RECT_F r;
        if (y0 == y1) {
            r.left = (float)(x0 < x1 ? x0 : x1);
            r.right = (float)(x0 < x1 ? x1 : x0);
            r.top = (float)(y0 - hw);
            r.bottom = (float)(y0 + hw);
        } else {
            r.left = (float)(x0 - hw);
            r.right = (float)(x0 + hw);
            r.top = (float)(y0 < y1 ? y0 : y1);
            r.bottom = (float)(y0 < y1 ? y1 : y0);
        }
        ID2D1RenderTarget_FillRectangle(c->rt, &r, (ID2D1Brush *)c->brush);
        return;
    }
    D2D1_POINT_2F a = {(float)x0, (float)y0};
    D2D1_POINT_2F b = {(float)x1, (float)y1};
    ID2D1RenderTarget_DrawLine(c->rt, a, b, (ID2D1Brush *)c->brush, (float)width, NULL);
}

void d2d_stroke_line(void *p, double x0, double y0, double x1, double y1, double width,
                     int32_t color) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c || !visible(color) || !begin(c)) return;
    set_color(c, color);
    line_with_brush(c, x0, y0, x1, y1, width);
}

/* A radius past half the short side is a pill; clamp it as blend2d does
   rather than hand Direct2D an over-large one. */
static D2D1_ROUNDED_RECT round_rect(double x, double y, double w, double h, double rad) {
    double lim = (w < h ? w : h) / 2.0;
    if (rad > lim) rad = lim;
    if (rad < 0.0) rad = 0.0;
    D2D1_ROUNDED_RECT rr;
    rr.rect = rectf(x, y, w, h);
    rr.radiusX = (float)rad;
    rr.radiusY = (float)rad;
    return rr;
}

void d2d_stroke_round_rect(void *p, double x, double y, double w, double h, double rad,
                           double width, int32_t color) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c || !visible(color) || !begin(c)) return;
    set_color(c, color);
    D2D1_ROUNDED_RECT rr = round_rect(x, y, w, h, rad);
    ID2D1RenderTarget_DrawRoundedRectangle(c->rt, &rr, (ID2D1Brush *)c->brush, (float)width,
                                           NULL);
}

void d2d_fill_ellipse(void *p, double cx, double cy, double rx, double ry, int32_t color) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c || !visible(color) || rx <= 0.0 || ry <= 0.0 || !begin(c)) return;
    set_color(c, color);
    D2D1_ELLIPSE e = {{(float)cx, (float)cy}, (float)rx, (float)ry};
    ID2D1RenderTarget_FillEllipse(c->rt, &e, (ID2D1Brush *)c->brush);
}

void d2d_stroke_ellipse(void *p, double cx, double cy, double rx, double ry, double width,
                        int32_t color) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c || !visible(color) || rx <= 0.0 || ry <= 0.0 || !begin(c)) return;
    set_color(c, color);
    D2D1_ELLIPSE e = {{(float)cx, (float)cy}, (float)rx, (float)ry};
    ID2D1RenderTarget_DrawEllipse(c->rt, &e, (ID2D1Brush *)c->brush, (float)width, NULL);
}

/* The cached gradient for these stops, building it on a miss. NULL when
   the stop list is too long to cache or creation failed:
   the caller then builds a one-off. */
static D2dGrad *grad_for(D2dCtx *c, const D2D1_GRADIENT_STOP *st, UINT n) {
    if (n > D2D_GRAD_STOPS) return NULL;
    size_t bytes = sizeof *st * n;
    for (int i = 0; i < D2D_GRADS; i++)
        if (c->grads[i].n == n && memcmp(c->grads[i].st, st, bytes) == 0)
            return &c->grads[i];
    D2dGrad *g = &c->grads[c->grad_next];
    c->grad_next = (c->grad_next + 1) % D2D_GRADS;
    if (g->lin) ID2D1LinearGradientBrush_Release(g->lin);
    if (g->rad) ID2D1RadialGradientBrush_Release(g->rad);
    if (g->coll) ID2D1GradientStopCollection_Release(g->coll);
    memset(g, 0, sizeof *g);
    if (FAILED(c->rt->lpVtbl->CreateGradientStopCollection(c->rt, st, n, D2D1_GAMMA_2_2,
                                                           D2D1_EXTEND_MODE_CLAMP, &g->coll))) {
        g->coll = NULL;
        return NULL;
    }
    memcpy(g->st, st, bytes);
    g->n = n;
    return g;
}

/* A one-off stop collection, for when the cache cannot serve. D2D1_GAMMA_2_2
   interpolates in sRGB space, which is what blend2d does too. Called through
   the vtable: mingw's ID2D1RenderTarget_CreateGradientStopCollection macro
   takes three arguments for what is a five-argument method. */
static ID2D1GradientStopCollection *make_stops(D2dCtx *c, const D2D1_GRADIENT_STOP *st,
                                               UINT n) {
    ID2D1GradientStopCollection *coll = NULL;
    if (FAILED(c->rt->lpVtbl->CreateGradientStopCollection(c->rt, st, n, D2D1_GAMMA_2_2,
                                                           D2D1_EXTEND_MODE_CLAMP, &coll)))
        return NULL;
    return coll;
}

static void fill_shape(D2dCtx *c, double x, double y, double w, double h, double rad,
                       ID2D1Brush *br) {
    D2D1_ROUNDED_RECT rr = round_rect(x, y, w, h, rad);
    if (rr.radiusX > 0.0f) ID2D1RenderTarget_FillRoundedRectangle(c->rt, &rr, br);
    else ID2D1RenderTarget_FillRectangle(c->rt, &rr.rect, br);
}

/* Vertical linear gradient from y to y+h, filled straight into the rounded
   shape: no clip needed, unlike blend2d. */
static void fill_gradient(D2dCtx *c, double x, double y, double w, double h, double rad,
                          const D2D1_GRADIENT_STOP *stops, UINT n) {
    if (n == 0 || !begin(c)) return;
    D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lp;
    lp.startPoint.x = (float)x;
    lp.startPoint.y = (float)y;
    lp.endPoint.x = (float)x;
    lp.endPoint.y = (float)(y + h);
    D2dGrad *g = grad_for(c, stops, n);
    if (g) {
        if (!g->lin) {
            if (FAILED(ID2D1RenderTarget_CreateLinearGradientBrush(c->rt, &lp, NULL, g->coll,
                                                                   &g->lin))) {
                g->lin = NULL;
                return;
            }
        } else {
            ID2D1LinearGradientBrush_SetStartPoint(g->lin, lp.startPoint);
            ID2D1LinearGradientBrush_SetEndPoint(g->lin, lp.endPoint);
        }
        fill_shape(c, x, y, w, h, rad, (ID2D1Brush *)g->lin);
        return;
    }
    ID2D1GradientStopCollection *coll = make_stops(c, stops, n);
    if (!coll) return;
    ID2D1LinearGradientBrush *br = NULL;
    if (SUCCEEDED(ID2D1RenderTarget_CreateLinearGradientBrush(c->rt, &lp, NULL, coll, &br))) {
        fill_shape(c, x, y, w, h, rad, (ID2D1Brush *)br);
        ID2D1LinearGradientBrush_Release(br);
    }
    ID2D1GradientStopCollection_Release(coll);
}

void d2d_gradient_v(void *p, double x, double y, double w, double h, double rad, int32_t top,
                    int32_t bottom) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c) return;
    D2D1_GRADIENT_STOP st[2];
    st[0].position = 0.0f;
    st[0].color = colf(top);
    st[1].position = 1.0f;
    st[1].color = colf(bottom);
    fill_gradient(c, x, y, w, h, rad, st, 2);
}

void d2d_gradient_n(void *p, double x, double y, double w, double h, double rad,
                    const int32_t *colors, const double *positions, int32_t n) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c || n <= 0) return;
    D2D1_GRADIENT_STOP local[16];
    D2D1_GRADIENT_STOP *st = n <= 16 ? local
                                     : (D2D1_GRADIENT_STOP *)malloc(sizeof *st * (size_t)n);
    if (!st) return;
    for (int i = 0; i < n; i++) {
        st[i].position = (float)positions[i];
        st[i].color = colf(colors[i]);
    }
    fill_gradient(c, x, y, w, h, rad, st, (UINT)n);
    if (st != local) free(st);
}

/* Centred radial gradient (focal point = centre) filling the rect, extend
   mode clamp: the radial twin of blend2d's gfx_radial_gradient_rect_n. */
void d2d_radial_gradient(void *p, double x, double y, double w, double h, double cx,
                         double cy, double radius, const int32_t *colors,
                         const double *positions, int32_t n) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c || n <= 0 || radius <= 0.0 || !begin(c)) return;
    D2D1_GRADIENT_STOP local[16];
    D2D1_GRADIENT_STOP *st = n <= 16 ? local
                                     : (D2D1_GRADIENT_STOP *)malloc(sizeof *st * (size_t)n);
    if (!st) return;
    for (int i = 0; i < n; i++) {
        st[i].position = (float)positions[i];
        st[i].color = colf(colors[i]);
    }
    D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES rp;
    rp.center.x = (float)cx;
    rp.center.y = (float)cy;
    rp.gradientOriginOffset.x = 0.0f;
    rp.gradientOriginOffset.y = 0.0f;
    rp.radiusX = (float)radius;
    rp.radiusY = (float)radius;
    D2D1_RECT_F r = rectf(x, y, w, h);
    D2dGrad *g = grad_for(c, st, (UINT)n);
    if (g) {
        if (!g->rad) {
            if (FAILED(ID2D1RenderTarget_CreateRadialGradientBrush(c->rt, &rp, NULL, g->coll,
                                                                   &g->rad)))
                g->rad = NULL;
        } else {
            ID2D1RadialGradientBrush_SetCenter(g->rad, rp.center);
            ID2D1RadialGradientBrush_SetRadiusX(g->rad, rp.radiusX);
            ID2D1RadialGradientBrush_SetRadiusY(g->rad, rp.radiusY);
        }
        if (g->rad) ID2D1RenderTarget_FillRectangle(c->rt, &r, (ID2D1Brush *)g->rad);
    } else {
        ID2D1GradientStopCollection *coll = make_stops(c, st, (UINT)n);
        ID2D1RadialGradientBrush *br = NULL;
        if (coll && SUCCEEDED(ID2D1RenderTarget_CreateRadialGradientBrush(c->rt, &rp, NULL,
                                                                          coll, &br))) {
            ID2D1RenderTarget_FillRectangle(c->rt, &r, (ID2D1Brush *)br);
            ID2D1RadialGradientBrush_Release(br);
        }
        if (coll) ID2D1GradientStopCollection_Release(coll);
    }
    if (st != local) free(st);
}

/* Four straight strokes inset by the radius at the corners: light on the
   top and left when raised, dark on the bottom and right. Port of
   gfx_bevel_round. */
void d2d_bevel(void *p, double x, double y, double w, double h, double rad, double width,
               int32_t light, int32_t dark, int32_t raised) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c || !begin(c)) return;
    if (rad < 0.0) rad = 0.0;
    int32_t tl = raised ? light : dark;
    int32_t br = raised ? dark : light;
    double hw = width / 2.0;
    if (visible(tl)) {
        set_color(c, tl);
        line_with_brush(c, x + rad, y + hw, x + w - rad, y + hw, width);
        line_with_brush(c, x + hw, y + rad, x + hw, y + h - rad, width);
    }
    if (visible(br)) {
        set_color(c, br);
        line_with_brush(c, x + rad, y + h - hw, x + w - rad, y + h - hw, width);
        line_with_brush(c, x + w - hw, y + rad, x + w - hw, y + h - rad, width);
    }
}

/* ---- paths --------------------------------------------------------------- */

/* Path builder over an ID2D1GeometrySink with blend2d's figure rules: a
   line or curve with no open figure starts one at the current point (the
   start of the last closed figure, or the segment's own end before any
   move), and filling uses the non-zero rule, blend2d's default. */
typedef struct {
    ID2D1GeometrySink *sink;
    int open;
    int have_cur;
    D2D1_POINT_2F cur, start;
} PathB;

static void pb_move(PathB *b, float x, float y) {
    if (b->open) ID2D1GeometrySink_EndFigure(b->sink, D2D1_FIGURE_END_OPEN);
    D2D1_POINT_2F pt = {x, y};
    ID2D1GeometrySink_BeginFigure(b->sink, pt, D2D1_FIGURE_BEGIN_FILLED);
    b->open = 1;
    b->have_cur = 1;
    b->cur = pt;
    b->start = pt;
}

/* Make sure a figure is open before a segment ending at (x, y). */
static void pb_ensure(PathB *b, float x, float y) {
    if (b->open) return;
    if (b->have_cur) pb_move(b, b->cur.x, b->cur.y);
    else pb_move(b, x, y);
}

static void pb_close(PathB *b) {
    if (!b->open) return;
    ID2D1GeometrySink_EndFigure(b->sink, D2D1_FIGURE_END_CLOSED);
    b->open = 0;
    b->cur = b->start;
}

static ID2D1PathGeometry *pb_begin(PathB *b) {
    ID2D1PathGeometry *g = NULL;
    memset(b, 0, sizeof *b);
    if (FAILED(ID2D1Factory_CreatePathGeometry(g_d2d, &g))) return NULL;
    if (FAILED(ID2D1PathGeometry_Open(g, &b->sink))) {
        ID2D1PathGeometry_Release(g);
        return NULL;
    }
    ID2D1GeometrySink_SetFillMode(b->sink, D2D1_FILL_MODE_WINDING);
    return g;
}

static int pb_end(PathB *b, ID2D1PathGeometry *g) {
    if (b->open) ID2D1GeometrySink_EndFigure(b->sink, D2D1_FIGURE_END_OPEN);
    HRESULT hr = ID2D1GeometrySink_Close(b->sink);
    ID2D1GeometrySink_Release(b->sink);
    if (FAILED(hr)) {
        ID2D1PathGeometry_Release(g);
        return 0;
    }
    return 1;
}

/* A vyto/geom/path command stream (0 move, 1 line, 2 quad, 3 cubic,
   4 close) with its coords, as gfx_build_path reads it. NULL on a short
   coords array or a geometry failure. */
static ID2D1PathGeometry *build_path(const int32_t *cmds, int nc, const double *co, int nco) {
    PathB b;
    ID2D1PathGeometry *g = pb_begin(&b);
    if (!g) return NULL;
    int ci = 0, ok = 1;
    for (int k = 0; k < nc && ok; k++) {
        switch (cmds[k]) {
        case 0:
            if (ci + 2 > nco) { ok = 0; break; }
            pb_move(&b, (float)co[ci], (float)co[ci + 1]);
            ci += 2;
            break;
        case 1: {
            if (ci + 2 > nco) { ok = 0; break; }
            D2D1_POINT_2F pt = {(float)co[ci], (float)co[ci + 1]};
            pb_ensure(&b, pt.x, pt.y);
            ID2D1GeometrySink_AddLine(b.sink, pt);
            b.cur = pt;
            ci += 2;
            break;
        }
        case 2: {
            if (ci + 4 > nco) { ok = 0; break; }
            D2D1_QUADRATIC_BEZIER_SEGMENT q;
            q.point1.x = (float)co[ci]; q.point1.y = (float)co[ci + 1];
            q.point2.x = (float)co[ci + 2]; q.point2.y = (float)co[ci + 3];
            pb_ensure(&b, q.point2.x, q.point2.y);
            ID2D1GeometrySink_AddQuadraticBezier(b.sink, &q);
            b.cur = q.point2;
            ci += 4;
            break;
        }
        case 3: {
            if (ci + 6 > nco) { ok = 0; break; }
            D2D1_BEZIER_SEGMENT s;
            s.point1.x = (float)co[ci]; s.point1.y = (float)co[ci + 1];
            s.point2.x = (float)co[ci + 2]; s.point2.y = (float)co[ci + 3];
            s.point3.x = (float)co[ci + 4]; s.point3.y = (float)co[ci + 5];
            pb_ensure(&b, s.point3.x, s.point3.y);
            ID2D1GeometrySink_AddBezier(b.sink, &s);
            b.cur = s.point3;
            ci += 6;
            break;
        }
        case 4:
            pb_close(&b);
            break;
        default:
            break;
        }
    }
    if (!pb_end(&b, g)) return NULL;
    if (!ok) {
        ID2D1PathGeometry_Release(g);
        return NULL;
    }
    return g;
}

static void fill_geom(D2dCtx *c, ID2D1PathGeometry *g, int32_t color) {
    set_color(c, color);
    ID2D1RenderTarget_FillGeometry(c->rt, (ID2D1Geometry *)g, (ID2D1Brush *)c->brush, NULL);
}

static void stroke_geom(D2dCtx *c, ID2D1PathGeometry *g, double width, int32_t color) {
    set_color(c, color);
    ID2D1RenderTarget_DrawGeometry(c->rt, (ID2D1Geometry *)g, (ID2D1Brush *)c->brush,
                                   (float)width, NULL);
}

void d2d_fill_path(void *p, const int32_t *cmds, int32_t nc, const double *coords,
                   int32_t ncoord, int32_t color) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c || !visible(color) || !begin(c)) return;
    ID2D1PathGeometry *g = build_path(cmds, nc, coords, ncoord);
    if (!g) return;
    fill_geom(c, g, color);
    ID2D1PathGeometry_Release(g);
}

void d2d_stroke_path(void *p, const int32_t *cmds, int32_t nc, const double *coords,
                     int32_t ncoord, double width, int32_t color) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c || !visible(color) || !begin(c)) return;
    ID2D1PathGeometry *g = build_path(cmds, nc, coords, ncoord);
    if (!g) return;
    stroke_geom(c, g, width, color);
    ID2D1PathGeometry_Release(g);
}

/* Filled polygon from parallel x/y arrays; fewer than 3 points draws
   nothing, as in blend2d. */
void d2d_fill_polygon(void *p, const double *xs, const double *ys, int32_t n,
                      int32_t color) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c || n < 3 || !visible(color) || !begin(c)) return;
    PathB b;
    ID2D1PathGeometry *g = pb_begin(&b);
    if (!g) return;
    pb_move(&b, (float)xs[0], (float)ys[0]);
    for (int i = 1; i < n; i++) {
        D2D1_POINT_2F pt = {(float)xs[i], (float)ys[i]};
        ID2D1GeometrySink_AddLine(b.sink, pt);
    }
    pb_close(&b);
    if (!pb_end(&b, g)) return;
    fill_geom(c, g, color);
    ID2D1PathGeometry_Release(g);
}

/* Elliptical arc stroke. Degrees, 0 = +x axis, positive sweep clockwise on
   screen, as gfx_stroke_arc. A full turn or more is a whole ellipse. */
void d2d_stroke_arc(void *p, double cx, double cy, double rx, double ry, double start_deg,
                    double sweep_deg, double width, int32_t color) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c || !visible(color) || rx <= 0.0 || ry <= 0.0 || sweep_deg == 0.0 || !begin(c))
        return;
    set_color(c, color);
    if (fabs(sweep_deg) >= 360.0) {
        D2D1_ELLIPSE e = {{(float)cx, (float)cy}, (float)rx, (float)ry};
        ID2D1RenderTarget_DrawEllipse(c->rt, &e, (ID2D1Brush *)c->brush, (float)width, NULL);
        return;
    }
    const double d2r = 3.14159265358979323846 / 180.0;
    double a0 = start_deg * d2r, a1 = (start_deg + sweep_deg) * d2r;
    PathB b;
    ID2D1PathGeometry *g = pb_begin(&b);
    if (!g) return;
    pb_move(&b, (float)(cx + rx * cos(a0)), (float)(cy + ry * sin(a0)));
    D2D1_ARC_SEGMENT arc;
    arc.point.x = (float)(cx + rx * cos(a1));
    arc.point.y = (float)(cy + ry * sin(a1));
    arc.size.width = (float)rx;
    arc.size.height = (float)ry;
    arc.rotationAngle = 0.0f;
    arc.sweepDirection = sweep_deg > 0.0 ? D2D1_SWEEP_DIRECTION_CLOCKWISE
                                         : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE;
    arc.arcSize = fabs(sweep_deg) > 180.0 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;
    ID2D1GeometrySink_AddArc(b.sink, &arc);
    if (!pb_end(&b, g)) return;
    ID2D1RenderTarget_DrawGeometry(c->rt, (ID2D1Geometry *)g, (ID2D1Brush *)c->brush,
                                   (float)width, NULL);
    ID2D1PathGeometry_Release(g);
}

/* ---- pixel effects -------------------------------------------------------
 * Ported from vyto/gfx's gfx_shim.c so the two rich tiers agree: a separable
 * box blur run three times converges on a gaussian. The blur and backdrop
 * read what is already drawn, so they end the frame and work on the locked
 * WIC bitmap. The shadow reads nothing; it is built off to the side and then
 * drawn as a bitmap, so it stays cached and obeys transforms and clips. */

/* Horizontal pass, all four channels. Multiplies by a 24-bit fixed-point
   reciprocal instead of dividing per sample; with round-to-nearest that is
   an exact divide over the whole input range. */
static void box_blur_h(uint8_t *p, intptr_t stride, int x0, int y0, int w, int h, int rad) {
    if (rad < 1 || w <= 1) return;
    int win = rad * 2 + 1;
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

/* Vertical pass over a strip of columns at a time, so each row's span stays
   contiguous in cache. */
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
        int nb = nc * 4;
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

/* Single-channel variants for the shadow, which only needs alpha blurred;
   the colour is rebuilt from it afterwards. */
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

/* Signed distance to a rounded-rect boundary; negative inside. */
static double rrect_sd(double px, double py, double x, double y, double w, double h,
                       double r) {
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

/* Lock a device-space region for reading and writing. Ends the frame first:
   the pixels must be final before they are read. Released by d2d_unlock. */
static uint8_t *lock_rw(D2dCtx *c, int x, int y, int w, int h) {
    end_draw(c);
    if (!c->bmp || c->lock) return NULL;
    WICRect r = {x, y, w, h};
    if (FAILED(IWICBitmap_Lock(c->bmp, &r, WICBitmapLockRead | WICBitmapLockWrite, &c->lock))) {
        c->lock = NULL;
        return NULL;
    }
    UINT size = 0;
    BYTE *data = NULL;
    if (FAILED(IWICBitmapLock_GetStride(c->lock, &c->stride)) ||
        FAILED(IWICBitmapLock_GetDataPointer(c->lock, &size, &data))) {
        IWICBitmapLock_Release(c->lock);
        c->lock = NULL;
        return NULL;
    }
    return data;
}

static void unlock(D2dCtx *c) {
    if (c->lock) {
        IWICBitmapLock_Release(c->lock);
        c->lock = NULL;
    }
}

/* The user-space rect's device box, clamped to the canvas and the current
   clip's box; 0 when nothing is left. */
static int clamp_region(D2dCtx *c, double x, double y, double w, double h, int *ox, int *oy,
                        int *ow, int *oh) {
    double fx0, fy0, fx1, fy1;
    dev_box(&c->xf, x, y, w, h, &fx0, &fy0, &fx1, &fy1);
    int x0 = (int)floor(fx0), y0 = (int)floor(fy0);
    int x1 = (int)ceil(fx1), y1 = (int)ceil(fy1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    if (c->nclip > 0) {
        const D2dClip *k = &c->clip[c->nclip - 1];
        if (x0 < k->bx) x0 = k->bx;
        if (y0 < k->by) y0 = k->by;
        if (x1 > k->bx + k->bw) x1 = k->bx + k->bw;
        if (y1 > k->by + k->bh) y1 = k->by + k->bh;
    }
    if (x1 <= x0 || y1 <= y0) return 0;
    *ox = x0;
    *oy = y0;
    *ow = x1 - x0;
    *oh = y1 - y0;
    return 1;
}

/* Soften what is already drawn inside the rect, in place. */
void d2d_blur(void *p, double x, double y, double w, double h, double radius) {
    D2dCtx *c = (D2dCtx *)p;
    int rad = (int)(radius + 0.5);
    int bx, by, bw, bh;
    if (!c || rad < 1 || !clamp_region(c, x, y, w, h, &bx, &by, &bw, &bh)) return;
    uint8_t *px = lock_rw(c, bx, by, bw, bh);
    if (!px) return;
    for (int pass = 0; pass < 3; pass++) {
        box_blur_h(px, (intptr_t)c->stride, 0, 0, bw, bh, rad);
        box_blur_v(px, (intptr_t)c->stride, 0, 0, bw, bh, rad);
    }
    unlock(c);
}

/* Frosted material: blur what is behind the rounded rect, put the corners
   outside the shape back (with a one-pixel anti-aliased edge), then lay the
   tint over it. Tint alpha 0 means blur only. Under a rotating transform the
   shape test uses the device bounding box, the same limit gfx has. */
void d2d_backdrop(void *p, double x, double y, double w, double h, double r, double radius,
                  int32_t tint) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c) return;
    int rad = (int)(radius + 0.5);
    if (r < 0.0) r = 0.0;
    int bx, by, bw, bh;
    if (rad >= 1 && clamp_region(c, x, y, w, h, &bx, &by, &bw, &bh)) {
        double sx0, sy0, sx1, sy1;
        dev_box(&c->xf, x, y, w, h, &sx0, &sy0, &sx1, &sy1);
        double det = fabs(c->xf._11 * c->xf._22 - c->xf._12 * c->xf._21);
        double dr = r * sqrt(det);
        size_t rowbytes = (size_t)bw * 4;
        uint8_t *saved = (uint8_t *)malloc(rowbytes * (size_t)bh);
        uint8_t *px = saved ? lock_rw(c, bx, by, bw, bh) : NULL;
        if (px) {
            intptr_t stride = (intptr_t)c->stride;
            for (int yy = 0; yy < bh; yy++)
                memcpy(saved + (size_t)yy * rowbytes, px + (intptr_t)yy * stride, rowbytes);
            for (int pass = 0; pass < 3; pass++) {
                box_blur_h(px, stride, 0, 0, bw, bh, rad);
                box_blur_v(px, stride, 0, 0, bw, bh, rad);
            }
            for (int yy = 0; yy < bh; yy++) {
                uint8_t *row = px + (intptr_t)yy * stride;
                const uint8_t *src = saved + (size_t)yy * rowbytes;
                for (int xx = 0; xx < bw; xx++) {
                    double sd = rrect_sd((double)(bx + xx) + 0.5, (double)(by + yy) + 0.5, sx0,
                                         sy0, sx1 - sx0, sy1 - sy0, dr);
                    double cov = 0.5 - sd;
                    if (cov >= 1.0) continue;
                    if (cov <= 0.0) {
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
            unlock(c);
        }
        free(saved);
    }
    if (visible(tint)) d2d_fill_round_rect(c, x, y, w, h, r, tint);
}

/* Rasterise, blur, recolour and knock out one shadow into a premultiplied
   bitmap of lw x lh with the caster at (pad, pad - dy). Matches gfx: the
   colour tints the blurred coverage and its alpha scales it, and the
   knock-out removes the caster's own shape so a translucent panel does not
   show its shadow. */
static ID2D1Bitmap *build_shadow(D2dCtx *c, double w, double h, double r, int rad, double dy,
                                 uint32_t col, int *olw, int *olh, int *opad) {
    int pad = rad * 3 + 2;
    int lw = (int)ceil(w) + pad * 2;
    int lh = (int)ceil(h) + pad * 2;
    size_t stride = (size_t)lw * 4;
    uint8_t *buf = (uint8_t *)calloc((size_t)lh, stride);
    if (!buf) return NULL;
    for (int yy = 0; yy < lh; yy++) {
        uint8_t *row = buf + (size_t)yy * stride;
        for (int xx = 0; xx < lw; xx++) {
            double cov = 0.5 - rrect_sd((double)xx + 0.5, (double)yy + 0.5, (double)pad,
                                        (double)pad, w, h, r);
            if (cov <= 0.0) continue;
            row[xx * 4 + 3] = cov >= 1.0 ? 255 : (uint8_t)(cov * 255.0 + 0.5);
        }
    }
    for (int pass = 0; pass < 3; pass++) {
        box_blur_h1(buf, (intptr_t)stride, lw, lh, rad, 3);
        box_blur_v1(buf, (intptr_t)stride, lw, lh, rad, 3);
    }
    unsigned cr = (col >> 16) & 0xFFu, cg = (col >> 8) & 0xFFu, cb = col & 0xFFu;
    unsigned ca = (col >> 24) & 0xFFu;
    for (int yy = 0; yy < lh; yy++) {
        uint8_t *row = buf + (size_t)yy * stride;
        for (int xx = 0; xx < lw; xx++) {
            double cov = 0.5 - rrect_sd((double)xx + 0.5, (double)yy + 0.5, (double)pad,
                                        (double)pad - dy, w, h, r);
            unsigned a = (row[xx * 4 + 3] * ca + 127u) / 255u;
            if (cov >= 1.0) {
                a = 0;
            } else if (cov > 0.0) {
                unsigned keep = (unsigned)((1.0 - cov) * 255.0 + 0.5);
                a = (a * keep + 127u) / 255u;
            }
            row[xx * 4 + 0] = (uint8_t)((cb * a + 127u) / 255u);
            row[xx * 4 + 1] = (uint8_t)((cg * a + 127u) / 255u);
            row[xx * 4 + 2] = (uint8_t)((cr * a + 127u) / 255u);
            row[xx * 4 + 3] = (uint8_t)a;
        }
    }
    D2D1_SIZE_U size = {(UINT32)lw, (UINT32)lh};
    D2D1_BITMAP_PROPERTIES bp;
    bp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    bp.dpiX = 96.0f;
    bp.dpiY = 96.0f;
    ID2D1Bitmap *bmp = NULL;
    if (FAILED(ID2D1RenderTarget_CreateBitmap(c->rt, size, buf, (UINT32)stride, &bp, &bmp)))
        bmp = NULL;
    free(buf);
    *olw = lw;
    *olh = lh;
    *opad = pad;
    return bmp;
}

static int quarter(double v) { return (int)floor(v * 4.0 + 0.5); }

/* Soft drop shadow under the rounded rect, offset down by dy. Colour alpha
   0 means a default darkness; blur under 1 is a plain offset fill. */
void d2d_shadow(void *p, double x, double y, double w, double h, double r, double blur,
                double dy, int32_t color) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c || w <= 0.0 || h <= 0.0) return;
    if (blur < 0.0) blur = 0.0;
    if (r < 0.0) r = 0.0;
    uint32_t col = (uint32_t)color;
    if ((col >> 24) == 0u) col = 0x30000000u | (col & 0xFFFFFFu);
    if (blur < 1.0) {
        d2d_fill_round_rect(c, x, y + dy, w, h, r, (int32_t)col);
        return;
    }
    if (!begin(c)) return;
    int rad = (int)(blur + 0.5);
    /* Build from the quantised key, so a hit and a miss draw identically. */
    int qw = quarter(w), qh = quarter(h), qr = quarter(r), qdy = quarter(dy);
    double kw = qw / 4.0, kh = qh / 4.0, kr = qr / 4.0, kdy = qdy / 4.0;
    D2dShadow *e = NULL;
    for (int i = 0; i < D2D_SHADOWS && !e; i++) {
        D2dShadow *s = &c->shadows[i];
        if (s->bmp && s->qw == qw && s->qh == qh && s->qr == qr && s->qdy == qdy &&
            s->rad == rad && s->col == col)
            e = s;
    }
    D2dShadow tmp;
    if (!e) {
        int pad = rad * 3 + 2;
        size_t bytes = (size_t)((int)ceil(kw) + pad * 2) * (size_t)((int)ceil(kh) + pad * 2) * 4;
        if (bytes > D2D_SHADOW_BUDGET / 2) {
            e = &tmp;                      /* too big to keep: draw it and let it go */
        } else {
            /* Evict least-recently-used until there is a free slot and the
               budget has room. */
            for (;;) {
                D2dShadow *lru = NULL, *free_slot = NULL;
                for (int i = 0; i < D2D_SHADOWS; i++) {
                    D2dShadow *s = &c->shadows[i];
                    if (!s->bmp) { if (!free_slot) free_slot = s; continue; }
                    if (!lru || s->used < lru->used) lru = s;
                }
                if (free_slot && c->shadow_bytes + bytes <= D2D_SHADOW_BUDGET) {
                    e = free_slot;
                    break;
                }
                if (!lru) { e = &tmp; break; }
                c->shadow_bytes -= (size_t)lru->lw * (size_t)lru->lh * 4;
                ID2D1Bitmap_Release(lru->bmp);
                lru->bmp = NULL;
            }
        }
        memset(e, 0, sizeof *e);
        e->bmp = build_shadow(c, kw, kh, kr, rad, kdy, col, &e->lw, &e->lh, &e->pad);
        if (!e->bmp) return;
        e->qw = qw; e->qh = qh; e->qr = qr; e->qdy = qdy; e->rad = rad; e->col = col;
        if (e != &tmp) c->shadow_bytes += (size_t)e->lw * (size_t)e->lh * 4;
    }
    e->used = ++c->tick;
    D2D1_RECT_F dst = rectf(x - e->pad, y + kdy - e->pad, e->lw, e->lh);
    ID2D1RenderTarget_DrawBitmap(c->rt, e->bmp, &dst, 1.0f,
                                 D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, NULL);
    if (e == &tmp) ID2D1Bitmap_Release(tmp.bmp);
}

/* ---- images -------------------------------------------------------------- */

/* A decoded image. The WIC bitmap owns the pixels and outlives any render
   target; the Direct2D bitmap is made from it per target, on first draw,
   and remade when `gen` shows the target has changed. */
typedef struct {
    IWICBitmap *src;
    ID2D1Bitmap *bmp;
    unsigned gen;
} D2dImage;

static D2dImage *image_from_decoder(IWICBitmapDecoder *dec) {
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    D2dImage *img = NULL;
    if (FAILED(IWICBitmapDecoder_GetFrame(dec, 0, &frame))) goto out;
    if (FAILED(IWICImagingFactory_CreateFormatConverter(g_wic, &conv))) goto out;
    if (FAILED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frame,
                                              &d2d_GUID_WICPixelFormat32bppPBGRA,
                                              WICBitmapDitherTypeNone, NULL, 0.0,
                                              WICBitmapPaletteTypeCustom)))
        goto out;
    img = (D2dImage *)calloc(1, sizeof *img);
    if (!img) goto out;
    /* CacheOnLoad decodes now and owns the pixels, so the file or the
       memory the decoder read from is no longer needed afterwards. */
    if (FAILED(IWICImagingFactory_CreateBitmapFromSource(g_wic, (IWICBitmapSource *)conv,
                                                         WICBitmapCacheOnLoad, &img->src))) {
        free(img);
        img = NULL;
    }
out:
    if (conv) IWICFormatConverter_Release(conv);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    return img;
}

void *d2d_image_load_file(const char *utf8) {
    if (!ensure_factories()) return NULL;
    int len = (int)strlen(utf8);
    WBuf b;
    if (len == 0 || !wbuf_from(&b, utf8, len)) return NULL;
    /* The converter wrote no terminator; the decoder needs one. */
    WCHAR *path = (WCHAR *)malloc(sizeof(WCHAR) * (size_t)(b.n + 1));
    D2dImage *img = NULL;
    if (path) {
        memcpy(path, b.p, sizeof(WCHAR) * (size_t)b.n);
        path[b.n] = 0;
        IWICBitmapDecoder *dec = NULL;
        if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(
                g_wic, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) {
            img = image_from_decoder(dec);
            IWICBitmapDecoder_Release(dec);
        }
        free(path);
    }
    wbuf_free(&b);
    return img;
}

void *d2d_image_load_bytes(const void *data, int32_t len) {
    if (!data || len <= 0 || !ensure_factories()) return NULL;
    IWICStream *stream = NULL;
    IWICBitmapDecoder *dec = NULL;
    D2dImage *img = NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateStream(g_wic, &stream)) &&
        SUCCEEDED(IWICStream_InitializeFromMemory(stream, (BYTE *)data, (DWORD)len)) &&
        SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream(
            g_wic, (IStream *)stream, NULL, WICDecodeMetadataCacheOnDemand, &dec))) {
        img = image_from_decoder(dec);
    }
    if (dec) IWICBitmapDecoder_Release(dec);
    if (stream) IWICStream_Release(stream);
    return img;
}

void d2d_image_free(void *p) {
    D2dImage *img = (D2dImage *)p;
    if (!img) return;
    if (img->bmp) ID2D1Bitmap_Release(img->bmp);
    if (img->src) IWICBitmap_Release(img->src);
    free(img);
}

/* Scale the image into the rect, bilinear. */
void d2d_draw_image(void *p, void *ip, double x, double y, double w, double h) {
    D2dCtx *c = (D2dCtx *)p;
    D2dImage *img = (D2dImage *)ip;
    if (!c || !img || !begin(c)) return;
    if (!img->bmp || img->gen != c->gen) {
        if (img->bmp) { ID2D1Bitmap_Release(img->bmp); img->bmp = NULL; }
        if (FAILED(ID2D1RenderTarget_CreateBitmapFromWicBitmap(
                c->rt, (IWICBitmapSource *)img->src, NULL, &img->bmp))) {
            img->bmp = NULL;
            return;
        }
        img->gen = c->gen;
    }
    D2D1_RECT_F r = rectf(x, y, w, h);
    ID2D1RenderTarget_DrawBitmap(c->rt, img->bmp, &r, 1.0f,
                                 D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, NULL);
}

/* Device-space bounding box of a user-space rect under transform m. */
static void dev_box(const D2D1_MATRIX_3X2_F *m, double x, double y, double w, double h,
                    double *x0, double *y0, double *x1, double *y1) {
    double xs[4] = {x, x + w, x, x + w}, ys[4] = {y, y, y + h, y + h};
    for (int i = 0; i < 4; i++) {
        double dx = xs[i] * m->_11 + ys[i] * m->_21 + m->_31;
        double dy = xs[i] * m->_12 + ys[i] * m->_22 + m->_32;
        if (i == 0 || dx < *x0) *x0 = dx;
        if (i == 0 || dy < *y0) *y0 = dy;
        if (i == 0 || dx > *x1) *x1 = dx;
        if (i == 0 || dy > *y1) *y1 = dy;
    }
}

/* The clip's whole-pixel device box, intersected with the enclosing clip's
   (or the canvas). Direct2D clips its own drawing; this box exists for the
   pixel ops, which write the bitmap directly and would otherwise ignore the
   clip, smearing outside a partial repaint's dirty rect. */
static void clip_box(D2dCtx *c, D2dClip *k) {
    double x0, y0, x1, y1;
    dev_box(&k->m, k->rect.left, k->rect.top, k->rect.right - k->rect.left,
            k->rect.bottom - k->rect.top, &x0, &y0, &x1, &y1);
    int px0 = 0, py0 = 0, px1 = c->w, py1 = c->h;
    if (k != c->clip) {
        const D2dClip *pk = k - 1;
        px0 = pk->bx; py0 = pk->by; px1 = pk->bx + pk->bw; py1 = pk->by + pk->bh;
    }
    int ix0 = (int)floor(x0), iy0 = (int)floor(y0);
    int ix1 = (int)ceil(x1), iy1 = (int)ceil(y1);
    if (ix0 < px0) ix0 = px0;
    if (iy0 < py0) iy0 = py0;
    if (ix1 > px1) ix1 = px1;
    if (iy1 > py1) iy1 = py1;
    k->bx = ix0;
    k->by = iy0;
    k->bw = ix1 > ix0 ? ix1 - ix0 : 0;
    k->bh = iy1 > iy0 ? iy1 - iy0 : 0;
}

/* True for transforms that keep rectangles axis-aligned (translate and
   scale only), the ones the corner emulation can follow. */
static int axis_preserving(const D2D1_MATRIX_3X2_F *m) {
    return m->_12 == 0.0f && m->_21 == 0.0f;
}

/* Rounded clips, emulated the way vyto/gfx does it. A rounded clip differs
   from an axis-aligned one only in its four corners, so push the rectangle
   and save the pixels the corners will spill into; on pop, blend the saved
   pixels back wherever the rounded shape does not cover, with a one-pixel
   anti-aliased edge. Direct2D's own answer, a layer with a geometric mask,
   measured about 190 us per push and pop on Windows 7 against about 27 us
   for blend2d's emulation, and rounded clips sit on every image corner and
   gallery tile. Nesting composes: an inner clip restores its corners before
   an outer one does, from a snapshot taken before the inner drew. */
static int save_corners(D2dCtx *c, D2dClip *k, double x, double y, double w, double h,
                        double rad) {
    double x0, y0, x1, y1;
    dev_box(&c->xf, x, y, w, h, &x0, &y0, &x1, &y1);
    double sc = fabs(c->xf._11) < fabs(c->xf._22) ? fabs(c->xf._11) : fabs(c->xf._22);
    double dw = x1 - x0, dh = y1 - y0, dr = rad * sc;
    if (dr > dw / 2.0) dr = dw / 2.0;
    if (dr > dh / 2.0) dr = dh / 2.0;
    k->sx = x0; k->sy = y0; k->sw = dw; k->sh = dh; k->sr = dr;
    int bx0 = (int)floor(x0), by0 = (int)floor(y0);
    int bx1 = (int)ceil(x1), by1 = (int)ceil(y1);
    int cs = (int)ceil(dr) + 1;
    int raw[4][4];
    int n;
    if (2 * cs >= bx1 - bx0 || 2 * cs >= by1 - by0) {
        n = 1;
        raw[0][0] = bx0; raw[0][1] = by0; raw[0][2] = bx1 - bx0; raw[0][3] = by1 - by0;
    } else {
        n = 4;
        int xs[4] = {bx0, bx1 - cs, bx0, bx1 - cs}, ys[4] = {by0, by0, by1 - cs, by1 - cs};
        for (int i = 0; i < 4; i++) {
            raw[i][0] = xs[i]; raw[i][1] = ys[i]; raw[i][2] = cs; raw[i][3] = cs;
        }
    }
    /* Clamp each block to the canvas; one off-canvas has nothing to restore. */
    size_t total = 0;
    k->nblk = 0;
    for (int i = 0; i < n; i++) {
        int ax0 = raw[i][0] < 0 ? 0 : raw[i][0], ay0 = raw[i][1] < 0 ? 0 : raw[i][1];
        int ax1 = raw[i][0] + raw[i][2], ay1 = raw[i][1] + raw[i][3];
        if (ax1 > c->w) ax1 = c->w;
        if (ay1 > c->h) ay1 = c->h;
        if (ax1 <= ax0 || ay1 <= ay0) continue;
        int *b = k->blk[k->nblk++];
        b[0] = ax0; b[1] = ay0; b[2] = ax1 - ax0; b[3] = ay1 - ay0;
        total += (size_t)b[2] * (size_t)b[3] * 4;
    }
    if (k->nblk == 0) return 1;            /* nothing on canvas to protect */
    k->saved = (uint8_t *)malloc(total);
    if (!k->saved) return 0;
    for (int i = 0; i < k->nblk; i++) {
        int *b = k->blk[i];
        uint8_t *px = lock_rw(c, b[0], b[1], b[2], b[3]);
        if (!px) {
            free(k->saved);
            k->saved = NULL;
            return 0;
        }
        uint8_t *dst = k->saved;
        for (int j = 0; j < i; j++) dst += (size_t)k->blk[j][2] * (size_t)k->blk[j][3] * 4;
        for (int yy = 0; yy < b[3]; yy++)
            memcpy(dst + (size_t)yy * (size_t)b[2] * 4, px + (size_t)yy * c->stride,
                   (size_t)b[2] * 4);
        unlock(c);
    }
    return 1;
}

static void restore_corners(D2dCtx *c, D2dClip *k) {
    if (!k->saved) return;
    const uint8_t *src = k->saved;
    for (int i = 0; i < k->nblk; i++) {
        int *b = k->blk[i];
        uint8_t *px = lock_rw(c, b[0], b[1], b[2], b[3]);
        if (px) {
            for (int yy = 0; yy < b[3]; yy++) {
                uint8_t *row = px + (size_t)yy * c->stride;
                const uint8_t *old = src + (size_t)yy * (size_t)b[2] * 4;
                for (int xx = 0; xx < b[2]; xx++) {
                    double cov = 0.5 - rrect_sd((double)(b[0] + xx) + 0.5,
                                                (double)(b[1] + yy) + 0.5, k->sx, k->sy,
                                                k->sw, k->sh, k->sr);
                    if (cov >= 1.0) continue;
                    if (cov <= 0.0) {
                        memcpy(row + xx * 4, old + xx * 4, 4);
                        continue;
                    }
                    unsigned a = (unsigned)(cov * 255.0 + 0.5);
                    for (int ch = 0; ch < 4; ch++) {
                        unsigned nb = row[xx * 4 + ch], ob = old[xx * 4 + ch];
                        row[xx * 4 + ch] = (uint8_t)((nb * a + ob * (255u - a) + 127u) / 255u);
                    }
                }
            }
            unlock(c);
        }
        src += (size_t)b[2] * (size_t)b[3] * 4;
    }
    free(k->saved);
    k->saved = NULL;
}

/* Clip to a rectangle, or to a rounded rectangle when radius > 0. Nested
   clips intersect. A rounded clip is a layer with a geometric mask: real
   anti-aliased corners, where blend2d has to save and blend corner pixels.
   The clip is pushed under the current transform and stays put if the
   transform changes afterwards, as in blend2d. */
void d2d_clip_push(void *p, double x, double y, double w, double h, double rad) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c) return;
    c->clip_depth++;
    if (c->nclip >= D2D_MAX_CLIP) return;  /* too deep: drawn unclipped, still balanced */
    D2dClip *k = &c->clip[c->nclip];
    k->kind = D2D_CLIP_AXIS;
    k->rect = rectf(x, y, w, h);
    k->geom = NULL;
    k->m = c->xf;
    clip_box(c, k);
    k->saved = NULL;
    k->nblk = 0;
    if (rad > 0.0 && axis_preserving(&c->xf) &&
        save_corners(c, k, x, y, w, h, rad)) {
        k->kind = D2D_CLIP_CORNERS;
    } else if (rad > 0.0) {
        D2D1_ROUNDED_RECT rr = round_rect(x, y, w, h, rad);
        ID2D1RoundedRectangleGeometry *g = NULL;
        if (SUCCEEDED(ID2D1Factory_CreateRoundedRectangleGeometry(g_d2d, &rr, &g))) {
            k->kind = D2D_CLIP_ROUND;
            k->geom = (ID2D1Geometry *)g;
        }
    }
    if (c->drawing) {
        apply_clip(c, c->nclip);
        ID2D1RenderTarget_SetTransform(c->rt, &c->xf);
    }
    c->nclip++;
}

static void drop_clip(D2dCtx *c) {
    c->nclip--;
    D2dClip *k = &c->clip[c->nclip];
    if (c->drawing) unapply_clip(c, c->nclip);
    if (k->geom) { ID2D1Geometry_Release(k->geom); k->geom = NULL; }
    if (k->kind == D2D_CLIP_CORNERS) restore_corners(c, k);
}

void d2d_clip_pop(void *p) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c || c->clip_depth == 0) return;
    c->clip_depth--;
    if (c->clip_depth >= c->nclip) return;  /* popping an unapplied overflow push */
    drop_clip(c);
}

/* Drop every clip and save, and reset the transform: a resized canvas
   starts from a clean state, as blend2d's does. */
static void reset_state(D2dCtx *c) {
    int was = c->drawing;
    if (was) end_draw(c);
    while (c->nclip > 0) drop_clip(c);
    c->clip_depth = 0;
    c->nsave = 0;
    c->save_depth = 0;
    c->xf = mat_identity();
}

/* ---- transforms ---------------------------------------------------------- */

static void set_xf(D2dCtx *c, D2D1_MATRIX_3X2_F m) {
    c->xf = m;
    if (c->drawing) ID2D1RenderTarget_SetTransform(c->rt, &c->xf);
}

/* Compose an operation onto the current transform in user space: it applies
   to what is drawn next, before the transforms already in force. */
static void apply_op(D2dCtx *c, D2D1_MATRIX_3X2_F op) {
    set_xf(c, mat_mul(&op, &c->xf));
}

void d2d_save(void *p) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c) return;
    c->save_depth++;
    if (c->nsave >= D2D_MAX_SAVE) return;
    c->saves[c->nsave].m = c->xf;
    c->saves[c->nsave].clip_depth = c->clip_depth;
    c->nsave++;
}

void d2d_restore(void *p) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c || c->save_depth == 0) return;
    c->save_depth--;
    if (c->save_depth >= c->nsave) return;
    c->nsave--;
    D2dSave *s = &c->saves[c->nsave];
    while (c->clip_depth > s->clip_depth) d2d_clip_pop(c);
    set_xf(c, s->m);
}

void d2d_translate(void *p, double dx, double dy) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c) return;
    D2D1_MATRIX_3X2_F t = mat_identity();
    t._31 = (float)dx;
    t._32 = (float)dy;
    apply_op(c, t);
}

void d2d_scale(void *p, double sx, double sy) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c) return;
    D2D1_MATRIX_3X2_F t = mat_identity();
    t._11 = (float)sx;
    t._22 = (float)sy;
    apply_op(c, t);
}

/* Degrees, clockwise on screen (y points down), as in blend2d. */
void d2d_rotate(void *p, double deg) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c) return;
    double rad = deg * (3.14159265358979323846 / 180.0);
    float cs = (float)cos(rad), sn = (float)sin(rad);
    D2D1_MATRIX_3X2_F t = mat_identity();
    t._11 = cs;
    t._12 = sn;
    t._21 = -sn;
    t._22 = cs;
    apply_op(c, t);
}

/* Select a system font family by name; falls back through Segoe UI, Tahoma,
   Arial. Returns 1 when the requested family itself was found. */
int32_t d2d_set_family(void *p, const char *utf8) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c) return 0;
    WBuf b;
    int found = 0;
    int len = (int)strlen(utf8);
    if (len > 0 && len < 64 && wbuf_from(&b, utf8, len)) {
        WCHAR name[64];
        int n = b.n < 63 ? b.n : 63;
        memcpy(name, b.p, sizeof(WCHAR) * (size_t)n);
        name[n] = 0;
        wbuf_free(&b);
        found = load_family(c, name);
    }
    for (int i = 0; i < 3 && !c->family[0]; i++) load_family(c, k_fallbacks[i]);
    return found;
}

void d2d_set_font(void *p, double size, int32_t weight) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c) return;
    if (size > 0.0) c->size = (float)size;
    c->weight = weight < 0 || weight > 2 ? 0 : weight;
}

double d2d_font_size(void *p) {
    D2dCtx *c = (D2dCtx *)p;
    return c ? (double)c->size : 0.0;
}

double d2d_font_ascent(void *p) {
    D2dCtx *c = (D2dCtx *)p;
    D2dFmt *f = c ? fmt_for(c) : NULL;
    return f ? (double)f->asc : 0.0;
}

double d2d_font_height(void *p) {
    D2dCtx *c = (D2dCtx *)p;
    D2dFmt *f = c ? fmt_for(c) : NULL;
    return f ? (double)(f->asc + f->desc + f->gap) : 0.0;
}

double d2d_text_width(void *p, const char *utf8) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c) return 0.0;
    int len = (int)strlen(utf8);
    if (len == 0) return 0.0;
    if (!c->wc) c->wc = (D2dWidth *)calloc(D2D_WCACHE, sizeof(D2dWidth));
    uint64_t key = width_key(utf8, len, c->size, c->weight);
    uint32_t mask = D2D_WCACHE - 1;
    uint32_t i = (uint32_t)key & mask;
    if (c->wc) {
        for (;;) {
            D2dWidth *e = &c->wc[i];
            if (e->h == 0) break;
            if (e->h == key && e->len == len && e->size == c->size && e->weight == c->weight)
                return (double)e->width;
            i = (i + 1) & mask;
        }
    }
    D2dFmt *f = fmt_for(c);
    if (!f) return 0.0;
    WBuf b;
    if (!wbuf_from(&b, utf8, len)) return 0.0;
    float width = 0.0f;
    IDWriteTextLayout *layout = NULL;
    if (SUCCEEDED(IDWriteFactory_CreateTextLayout(g_dw, b.p, (UINT32)b.n, f->fmt, 1.0e6f,
                                                  1.0e6f, &layout))) {
        DWRITE_TEXT_METRICS tm;
        if (SUCCEEDED(IDWriteTextLayout_GetMetrics(layout, &tm)))
            width = tm.widthIncludingTrailingWhitespace;
        IDWriteTextLayout_Release(layout);
    }
    wbuf_free(&b);
    if (c->wc) {
        /* Clear at 3/4 load rather than evict: probing stays short, and a UI's
           working set refills in one frame. */
        if (c->wc_used >= (D2D_WCACHE / 4) * 3) {
            memset(c->wc, 0, sizeof(D2dWidth) * D2D_WCACHE);
            c->wc_used = 0;
            i = (uint32_t)key & mask;
        }
        while (c->wc[i].h != 0) i = (i + 1) & mask;
        D2dWidth *e = &c->wc[i];
        e->h = key;
        e->len = len;
        e->size = c->size;
        e->weight = c->weight;
        e->width = width;
        c->wc_used++;
    }
    return (double)width;
}

/* y is the baseline, as everywhere in the Painter API. */
void d2d_text(void *p, double x, double y, const char *utf8, int32_t color) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c || !visible(color)) return;
    int len = (int)strlen(utf8);
    if (len == 0) return;
    D2dFmt *f = fmt_for(c);
    if (!f || !begin(c)) return;
    WBuf b;
    if (!wbuf_from(&b, utf8, len)) return;
    set_color(c, color);
    float top = (float)y - f->asc;
    D2D1_RECT_F r;
    r.left = (float)x;
    r.top = top;
    r.right = (float)x + 1.0e6f;
    r.bottom = top + f->asc + f->desc + f->gap;
    ID2D1RenderTarget_DrawText(c->rt, b.p, (UINT32)b.n, f->fmt, &r, (ID2D1Brush *)c->brush,
                               D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
    wbuf_free(&b);
}

/* Pixel access for presenting. Ends the frame, then locks the logical area
   for reading; the pointer is valid until d2d_unlock. NULL if the lock
   fails or one is already held. */
void *d2d_lock(void *p) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c) return NULL;
    end_draw(c);
    if (!c->bmp || c->lock) return NULL;
    WICRect r = {0, 0, c->w, c->h};
    if (FAILED(IWICBitmap_Lock(c->bmp, &r, WICBitmapLockRead, &c->lock))) {
        c->lock = NULL;
        return NULL;
    }
    UINT size = 0;
    BYTE *data = NULL;
    if (FAILED(IWICBitmapLock_GetStride(c->lock, &c->stride)) ||
        FAILED(IWICBitmapLock_GetDataPointer(c->lock, &size, &data))) {
        IWICBitmapLock_Release(c->lock);
        c->lock = NULL;
        return NULL;
    }
    return data;
}

int32_t d2d_stride(void *p) {
    D2dCtx *c = (D2dCtx *)p;
    return c ? (int32_t)c->stride : 0;
}

void d2d_unlock(void *p) {
    D2dCtx *c = (D2dCtx *)p;
    if (c && c->lock) {
        IWICBitmapLock_Release(c->lock);
        c->lock = NULL;
    }
}

/* 0xFFRRGGBB at (x, y), or 0 outside the canvas. A test hook. */
int32_t d2d_pixel_at(void *p, int32_t x, int32_t y) {
    D2dCtx *c = (D2dCtx *)p;
    if (!c || x < 0 || y < 0 || x >= c->w || y >= c->h) return 0;
    uint8_t *data = (uint8_t *)d2d_lock(c);
    if (!data) return 0;
    uint32_t px;
    memcpy(&px, data + (size_t)y * c->stride + (size_t)x * 4, 4);
    d2d_unlock(c);
    return (int32_t)(0xFF000000u | (px & 0x00FFFFFFu));
}

#else /* not Windows: every entry point reports failure */

int32_t d2d_ok(void) { return 0; }
void *d2d_new(int32_t w, int32_t h) { (void)w; (void)h; return 0; }
void d2d_free(void *p) { (void)p; }
int32_t d2d_mode(void *p) { (void)p; return 0; }
int32_t d2d_resize(void *p, int32_t w, int32_t h) {
    (void)p; (void)w; (void)h; return 0;
}
void d2d_end(void *p) { (void)p; }
void d2d_fill_rect(void *p, double x, double y, double w, double h, int32_t color) {
    (void)p; (void)x; (void)y; (void)w; (void)h; (void)color;
}
void d2d_fill_round_rect(void *p, double x, double y, double w, double h, double rad,
                         int32_t color) {
    (void)p; (void)x; (void)y; (void)w; (void)h; (void)rad; (void)color;
}
void d2d_stroke_line(void *p, double x0, double y0, double x1, double y1, double width,
                     int32_t color) {
    (void)p; (void)x0; (void)y0; (void)x1; (void)y1; (void)width; (void)color;
}
void d2d_stroke_round_rect(void *p, double x, double y, double w, double h, double rad,
                           double width, int32_t color) {
    (void)p; (void)x; (void)y; (void)w; (void)h; (void)rad; (void)width; (void)color;
}
void d2d_fill_ellipse(void *p, double cx, double cy, double rx, double ry, int32_t color) {
    (void)p; (void)cx; (void)cy; (void)rx; (void)ry; (void)color;
}
void d2d_stroke_ellipse(void *p, double cx, double cy, double rx, double ry, double width,
                        int32_t color) {
    (void)p; (void)cx; (void)cy; (void)rx; (void)ry; (void)width; (void)color;
}
void d2d_gradient_v(void *p, double x, double y, double w, double h, double rad, int32_t top,
                    int32_t bottom) {
    (void)p; (void)x; (void)y; (void)w; (void)h; (void)rad; (void)top; (void)bottom;
}
void d2d_gradient_n(void *p, double x, double y, double w, double h, double rad,
                    const int32_t *colors, const double *positions, int32_t n) {
    (void)p; (void)x; (void)y; (void)w; (void)h; (void)rad; (void)colors; (void)positions;
    (void)n;
}
void d2d_clip_push(void *p, double x, double y, double w, double h, double rad) {
    (void)p; (void)x; (void)y; (void)w; (void)h; (void)rad;
}
void d2d_clip_pop(void *p) { (void)p; }
void d2d_save(void *p) { (void)p; }
void d2d_restore(void *p) { (void)p; }
void d2d_translate(void *p, double dx, double dy) { (void)p; (void)dx; (void)dy; }
void d2d_scale(void *p, double sx, double sy) { (void)p; (void)sx; (void)sy; }
void d2d_rotate(void *p, double deg) { (void)p; (void)deg; }
void d2d_radial_gradient(void *p, double x, double y, double w, double h, double cx,
                         double cy, double radius, const int32_t *colors,
                         const double *positions, int32_t n) {
    (void)p; (void)x; (void)y; (void)w; (void)h; (void)cx; (void)cy; (void)radius;
    (void)colors; (void)positions; (void)n;
}
void d2d_bevel(void *p, double x, double y, double w, double h, double rad, double width,
               int32_t light, int32_t dark, int32_t raised) {
    (void)p; (void)x; (void)y; (void)w; (void)h; (void)rad; (void)width; (void)light;
    (void)dark; (void)raised;
}
void d2d_fill_path(void *p, const int32_t *cmds, int32_t nc, const double *coords,
                   int32_t ncoord, int32_t color) {
    (void)p; (void)cmds; (void)nc; (void)coords; (void)ncoord; (void)color;
}
void d2d_stroke_path(void *p, const int32_t *cmds, int32_t nc, const double *coords,
                     int32_t ncoord, double width, int32_t color) {
    (void)p; (void)cmds; (void)nc; (void)coords; (void)ncoord; (void)width; (void)color;
}
void d2d_fill_polygon(void *p, const double *xs, const double *ys, int32_t n,
                      int32_t color) {
    (void)p; (void)xs; (void)ys; (void)n; (void)color;
}
void d2d_stroke_arc(void *p, double cx, double cy, double rx, double ry, double start_deg,
                    double sweep_deg, double width, int32_t color) {
    (void)p; (void)cx; (void)cy; (void)rx; (void)ry; (void)start_deg; (void)sweep_deg;
    (void)width; (void)color;
}
void d2d_blur(void *p, double x, double y, double w, double h, double radius) {
    (void)p; (void)x; (void)y; (void)w; (void)h; (void)radius;
}
void d2d_backdrop(void *p, double x, double y, double w, double h, double r, double radius,
                  int32_t tint) {
    (void)p; (void)x; (void)y; (void)w; (void)h; (void)r; (void)radius; (void)tint;
}
void d2d_shadow(void *p, double x, double y, double w, double h, double r, double blur,
                double dy, int32_t color) {
    (void)p; (void)x; (void)y; (void)w; (void)h; (void)r; (void)blur; (void)dy; (void)color;
}
void *d2d_image_load_file(const char *utf8) { (void)utf8; return 0; }
void *d2d_image_load_bytes(const void *data, int32_t len) { (void)data; (void)len; return 0; }
void d2d_image_free(void *p) { (void)p; }
void d2d_draw_image(void *p, void *ip, double x, double y, double w, double h) {
    (void)p; (void)ip; (void)x; (void)y; (void)w; (void)h;
}
int32_t d2d_set_family(void *p, const char *utf8) { (void)p; (void)utf8; return 0; }
void d2d_set_font(void *p, double size, int32_t weight) { (void)p; (void)size; (void)weight; }
double d2d_font_size(void *p) { (void)p; return 0.0; }
double d2d_font_ascent(void *p) { (void)p; return 0.0; }
double d2d_font_height(void *p) { (void)p; return 0.0; }
double d2d_text_width(void *p, const char *utf8) { (void)p; (void)utf8; return 0.0; }
void d2d_text(void *p, double x, double y, const char *utf8, int32_t color) {
    (void)p; (void)x; (void)y; (void)utf8; (void)color;
}
void *d2d_lock(void *p) { (void)p; return 0; }
int32_t d2d_stride(void *p) { (void)p; return 0; }
void d2d_unlock(void *p) { (void)p; }
int32_t d2d_pixel_at(void *p, int32_t x, int32_t y) { (void)p; (void)x; (void)y; return 0; }

#endif
