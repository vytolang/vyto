/* apainter_shim.c — the vta_* draw API and the command-buffer encoder.
 *
 * Java half: dev.vyto.android.CommandBuffer. Vyto half: vyto/mobile/android/painter.
 *
 * STATUS: written, never compiled — no NDK toolchain in this clone. The body
 * is wrapped in #ifdef __ANDROID__, so it is empty on every other target.
 * Design of record: local/docs/ANDROID.md.
 *
 * -- the one rule --------------------------------------------------------------
 *
 * Draw calls encode into a native buffer and cross JNI once, at present().
 * Per-call JNI with a NewStringUTF per drawText will not hold a frame budget,
 * so nothing in the draw path may call into Java. The two exceptions are
 * deliberate and both are outside the frame:
 *
 *   - vta_intern, on a genuinely new string (once per string, ever)
 *   - vta_text_width / font metrics, on a cache miss
 *
 * -- wire format ---------------------------------------------------------------
 *
 * MUST match the opcode table in CommandBuffer.java. A mismatch decodes as
 * garbage rather than failing, so treat any change here as a wire break and
 * change both halves together.
 *
 * Coordinates are float32 — narrowed here from Vyto's f64, because Canvas
 * takes float anyway and it halves the buffer. Colors are int32 and pass
 * through unconverted: Vyto packs 0xAARRGGBB (surface.vt:178-190), which is
 * exactly android.graphics.Color.
 */
/* vytoc globs native/src/*.c flat and compiles every file for every
 * target, with no per-file platform filter, so an Android-only source
 * must exclude its own body. Same pattern as vsurf_android.c. */
#ifdef __ANDROID__

#include "vyto_android.h"

#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------- opcodes */
/* Keep in lockstep with CommandBuffer.java. */
enum {
    OP_END = 0,
    OP_FILL_RECT = 1, OP_ROUND_RECT = 2, OP_STROKE_ROUND = 3,
    OP_CIRCLE = 4, OP_STROKE_CIRCLE = 5, OP_LINE = 6, OP_ARC = 7,
    OP_POLYGON = 8, OP_GRAD_V = 9, OP_GRAD_N = 10, OP_RADIAL = 11,
    OP_SHADOW = 12, OP_BEVEL = 13, OP_CLIP_PUSH = 14, OP_CLIP_POP = 15,
    OP_SAVE = 16, OP_RESTORE = 17, OP_TRANSLATE = 18, OP_SCALE = 19,
    OP_ROTATE = 20, OP_SET_FONT = 21, OP_TEXT = 22, OP_IMAGE = 23,
    OP_DEFINE_STR = 24
};

/* OP_DEFINE_STR is defined on both sides but unused by this encoder: strings
 * are defined via a direct JNI call at intern time instead, so a string is
 * already resident before it can be measured (measurement happens during
 * layout, before any frame flushes). The opcode stays reserved rather than
 * being reused, so a future buffer-only path does not have to renumber. */

/* --------------------------------------------------------------- painter */

typedef struct {
    void   *surf;          /* VSurf*, for correlation with the surface arm */
    int     w, h;

    uint8_t *buf;
    size_t   len, cap;
    jobject  bytebuf;      /* global ref, DirectByteBuffer over buf */

    double   font_size;
    int      weight;

    VtaText *text;

    int      next_image;   /* handle allocator; handles are small ints cast to ptr */
} Painter;

#define BUF_CAP0 (64 * 1024)

/* Cached method IDs. Resolved once on first use; the classes are held alive by
 * the global refs in jni_boot.c, so the IDs stay valid. */
static jmethodID m_replayAndPost = NULL;
static jmethodID m_measureText   = NULL;
static jmethodID m_fontAscent    = NULL;
static jmethodID m_fontHeight    = NULL;
static jmethodID m_defineString  = NULL;
static jmethodID m_loadImage     = NULL;
static jmethodID m_dropImage     = NULL;
static int       ids_ready = 0;

static void resolve_ids(JNIEnv *env) {
    if (ids_ready) return;
    jobject cb = vta_commands();
    jobject view = vta_view();
    if (!cb || !view) return;

    jclass cbc = (*env)->GetObjectClass(env, cb);
    m_measureText  = (*env)->GetMethodID(env, cbc, "measureText", "(IFI)F");
    m_fontAscent   = (*env)->GetMethodID(env, cbc, "fontAscent", "(FI)F");
    m_fontHeight   = (*env)->GetMethodID(env, cbc, "fontHeight", "(FI)F");
    m_defineString = (*env)->GetMethodID(env, cbc, "defineString",
                                         "(ILjava/lang/String;)V");
    m_loadImage    = (*env)->GetMethodID(env, cbc, "loadImage",
                                         "(Ljava/lang/String;)I");
    m_dropImage    = (*env)->GetMethodID(env, cbc, "dropImage", "(I)V");
    (*env)->DeleteLocalRef(env, cbc);

    jclass vc = (*env)->GetObjectClass(env, view);
    m_replayAndPost = (*env)->GetMethodID(env, vc, "replayAndPost",
                                          "(Ljava/nio/ByteBuffer;IIIII)V");
    (*env)->DeleteLocalRef(env, vc);

    ids_ready = (m_replayAndPost && m_measureText && m_defineString) ? 1 : 0;
    if (!ids_ready) vta_logf("apainter: method id resolution failed");
}

/* ---------------------------------------------------------------- encoder */

static int ensure(Painter *p, size_t extra) {
    if (p->len + extra <= p->cap) return 1;
    size_t ncap = p->cap ? p->cap * 2 : BUF_CAP0;
    while (ncap < p->len + extra) ncap *= 2;
    uint8_t *n = (uint8_t *)realloc(p->buf, ncap);
    if (!n) return 0;
    p->buf = n;
    p->cap = ncap;
    /* The DirectByteBuffer wraps the old pointer; it must be rebuilt. Done
     * lazily in flush() rather than here, so a growth mid-frame costs nothing
     * extra. */
    if (p->bytebuf) {
        JNIEnv *env = vta_env();
        if (env) (*env)->DeleteGlobalRef(env, p->bytebuf);
        p->bytebuf = NULL;
    }
    return 1;
}

static void put_i32(Painter *p, int32_t v) {
    memcpy(p->buf + p->len, &v, 4);
    p->len += 4;
}

static void put_f32(Painter *p, double v) {
    float f = (float)v;
    memcpy(p->buf + p->len, &f, 4);
    p->len += 4;
}

/* op + n additional 4-byte words */
static int op(Painter *p, int32_t code, size_t words) {
    if (!ensure(p, 4 + words * 4)) return 0;
    put_i32(p, code);
    return 1;
}

static void put_rect(Painter *p, double x, double y, double w, double h) {
    put_f32(p, x); put_f32(p, y); put_f32(p, w); put_f32(p, h);
}

/* ------------------------------------------------------------- lifecycle */

void *vta_painter_new(void *surf, int32_t w, int32_t h, double font_size) {
    Painter *p = (Painter *)calloc(1, sizeof(Painter));
    if (!p) return NULL;
    p->surf = surf;
    p->w = w; p->h = h;
    p->font_size = font_size;
    p->weight = 0;
    p->next_image = 1;
    p->text = vta_text_new();
    p->buf = (uint8_t *)malloc(BUF_CAP0);
    if (!p->buf || !p->text) {
        vta_text_free(p->text);
        free(p->buf);
        free(p);
        return NULL;
    }
    p->cap = BUF_CAP0;
    return p;
}

void vta_painter_free(void *vp) {
    Painter *p = (Painter *)vp;
    if (!p) return;
    if (p->bytebuf) {
        JNIEnv *env = vta_env();
        if (env) (*env)->DeleteGlobalRef(env, p->bytebuf);
    }
    vta_text_free(p->text);
    free(p->buf);
    free(p);
}

void vta_resize(void *vp, int32_t w, int32_t h) {
    Painter *p = (Painter *)vp;
    if (!p) return;
    p->w = w; p->h = h;
    /* The Bitmap itself is reallocated by VytoView.onSizeChanged; nothing to
     * do here but track the size text/layout will ask about. */
}

int32_t vta_width(void *vp)  { Painter *p = (Painter *)vp; return p ? p->w : 0; }
int32_t vta_height(void *vp) { Painter *p = (Painter *)vp; return p ? p->h : 0; }

/* --------------------------------------------------------------- interning */

int32_t vta_intern(void *vp, const char *s) {
    Painter *p = (Painter *)vp;
    if (!p || !s) return 0;

    int is_new = 0;
    int32_t id = vta_text_intern(p->text, s, &is_new);
    if (!is_new || id <= 0) return id;

    /* First sighting: hand the bytes to Java once, ever. This is the only
     * string marshal in the whole design. */
    JNIEnv *env = vta_env();
    if (!env) return id;
    resolve_ids(env);
    if (!m_defineString) return id;

    jstring js = (*env)->NewStringUTF(env, s);
    if (!js) return id;
    (*env)->CallVoidMethod(env, vta_commands(), m_defineString, (jint)id, js);
    (*env)->DeleteLocalRef(env, js);
    return id;
}

/* ------------------------------------------------------------- draw ops */

void vta_fill_rect(void *vp, double x, double y, double w, double h, int32_t rgb) {
    Painter *p = (Painter *)vp;
    if (!p || !op(p, OP_FILL_RECT, 5)) return;
    put_rect(p, x, y, w, h); put_i32(p, rgb);
}

void vta_fill_round_rect(void *vp, double x, double y, double w, double h,
                         double r, int32_t rgb) {
    Painter *p = (Painter *)vp;
    if (!p || !op(p, OP_ROUND_RECT, 6)) return;
    put_rect(p, x, y, w, h); put_f32(p, r); put_i32(p, rgb);
}

void vta_stroke_round_rect(void *vp, double x, double y, double w, double h,
                           double r, double width, int32_t rgb) {
    Painter *p = (Painter *)vp;
    if (!p || !op(p, OP_STROKE_ROUND, 7)) return;
    put_rect(p, x, y, w, h); put_f32(p, r); put_f32(p, width); put_i32(p, rgb);
}

void vta_fill_circle(void *vp, double cx, double cy, double radius, int32_t rgb) {
    Painter *p = (Painter *)vp;
    if (!p || !op(p, OP_CIRCLE, 4)) return;
    put_f32(p, cx); put_f32(p, cy); put_f32(p, radius); put_i32(p, rgb);
}

void vta_stroke_circle(void *vp, double cx, double cy, double radius,
                       double width, int32_t rgb) {
    Painter *p = (Painter *)vp;
    if (!p || !op(p, OP_STROKE_CIRCLE, 5)) return;
    put_f32(p, cx); put_f32(p, cy); put_f32(p, radius);
    put_f32(p, width); put_i32(p, rgb);
}

void vta_stroke_line(void *vp, double x0, double y0, double x1, double y1,
                     double width, int32_t rgb) {
    Painter *p = (Painter *)vp;
    if (!p || !op(p, OP_LINE, 6)) return;
    put_f32(p, x0); put_f32(p, y0); put_f32(p, x1); put_f32(p, y1);
    put_f32(p, width); put_i32(p, rgb);
}

void vta_stroke_arc(void *vp, double cx, double cy, double rx, double ry,
                    double a0, double a1, double width, int32_t rgb) {
    Painter *p = (Painter *)vp;
    if (!p || !op(p, OP_ARC, 8)) return;
    put_f32(p, cx); put_f32(p, cy); put_f32(p, rx); put_f32(p, ry);
    put_f32(p, a0); put_f32(p, a1); put_f32(p, width); put_i32(p, rgb);
}

/* xs/ys arrive as Vyto float[] .ptr(), i.e. double*, and are narrowed here. */
void vta_fill_polygon(void *vp, void *xs, void *ys, int32_t n, int32_t rgb) {
    Painter *p = (Painter *)vp;
    if (!p || n <= 0 || !xs || !ys) return;
    if (!op(p, OP_POLYGON, (size_t)(1 + 2 * n + 1))) return;
    const double *dx = (const double *)xs;
    const double *dy = (const double *)ys;
    put_i32(p, n);
    for (int32_t i = 0; i < n; i++) put_f32(p, dx[i]);
    for (int32_t i = 0; i < n; i++) put_f32(p, dy[i]);
    put_i32(p, rgb);
}

void vta_gradient_v(void *vp, double x, double y, double w, double h, double r,
                    int32_t top, int32_t bottom) {
    Painter *p = (Painter *)vp;
    if (!p || !op(p, OP_GRAD_V, 7)) return;
    put_rect(p, x, y, w, h); put_f32(p, r);
    put_i32(p, top); put_i32(p, bottom);
}

void vta_gradient_n(void *vp, double x, double y, double w, double h, double r,
                    void *colors, void *positions, int32_t n) {
    Painter *p = (Painter *)vp;
    if (!p || n <= 0 || !colors || !positions) return;
    if (!op(p, OP_GRAD_N, (size_t)(6 + 2 * n))) return;
    const int32_t *cs = (const int32_t *)colors;   /* Vyto i32[] */
    const double  *ps = (const double *)positions; /* Vyto float[] */
    put_rect(p, x, y, w, h); put_f32(p, r); put_i32(p, n);
    for (int32_t i = 0; i < n; i++) put_i32(p, cs[i]);
    for (int32_t i = 0; i < n; i++) put_f32(p, ps[i]);
}

void vta_radial_gradient(void *vp, double x, double y, double w, double h, double r,
                         double cx, double cy, double radius,
                         void *colors, void *positions, int32_t n) {
    Painter *p = (Painter *)vp;
    if (!p || n <= 0 || !colors || !positions) return;
    if (!op(p, OP_RADIAL, (size_t)(9 + 2 * n))) return;
    const int32_t *cs = (const int32_t *)colors;
    const double  *ps = (const double *)positions;
    put_rect(p, x, y, w, h); put_f32(p, r);
    put_f32(p, cx); put_f32(p, cy); put_f32(p, radius);
    put_i32(p, n);
    for (int32_t i = 0; i < n; i++) put_i32(p, cs[i]);
    for (int32_t i = 0; i < n; i++) put_f32(p, ps[i]);
}

void vta_shadow(void *vp, double x, double y, double w, double h,
                double r, double blur, double dy, int32_t rgb) {
    Painter *p = (Painter *)vp;
    if (!p || !op(p, OP_SHADOW, 8)) return;
    put_rect(p, x, y, w, h);
    put_f32(p, r); put_f32(p, blur); put_f32(p, dy); put_i32(p, rgb);
}

void vta_bevel(void *vp, double x, double y, double w, double h,
               double radius, double width, int32_t light, int32_t dark,
               int32_t raised) {
    Painter *p = (Painter *)vp;
    if (!p || !op(p, OP_BEVEL, 9)) return;
    put_rect(p, x, y, w, h); put_f32(p, radius); put_f32(p, width);
    put_i32(p, light); put_i32(p, dark); put_i32(p, raised);
}

void vta_clip_push(void *vp, double x, double y, double w, double h, double r) {
    Painter *p = (Painter *)vp;
    if (!p || !op(p, OP_CLIP_PUSH, 5)) return;
    put_rect(p, x, y, w, h); put_f32(p, r);
}

void vta_clip_pop(void *vp) { Painter *p = (Painter *)vp; if (p) op(p, OP_CLIP_POP, 0); }
void vta_save(void *vp)     { Painter *p = (Painter *)vp; if (p) op(p, OP_SAVE, 0); }
void vta_restore(void *vp)  { Painter *p = (Painter *)vp; if (p) op(p, OP_RESTORE, 0); }

void vta_translate(void *vp, double dx, double dy) {
    Painter *p = (Painter *)vp;
    if (!p || !op(p, OP_TRANSLATE, 2)) return;
    put_f32(p, dx); put_f32(p, dy);
}

void vta_scale(void *vp, double sx, double sy) {
    Painter *p = (Painter *)vp;
    if (!p || !op(p, OP_SCALE, 2)) return;
    put_f32(p, sx); put_f32(p, sy);
}

void vta_rotate(void *vp, double deg) {
    Painter *p = (Painter *)vp;
    if (!p || !op(p, OP_ROTATE, 1)) return;
    put_f32(p, deg);
}

/* ------------------------------------------------------------------- text */

void vta_set_font(void *vp, double size, int32_t weight) {
    Painter *p = (Painter *)vp;
    if (!p) return;
    p->font_size = size;
    p->weight = weight;
    if (!op(p, OP_SET_FONT, 2)) return;
    put_f32(p, size); put_i32(p, weight);
}

double vta_font_size(void *vp) { Painter *p = (Painter *)vp; return p ? p->font_size : 0.0; }

void vta_text(void *vp, double x, double y, int32_t str_id, int32_t rgb) {
    Painter *p = (Painter *)vp;
    if (!p || str_id <= 0 || !op(p, OP_TEXT, 4)) return;
    put_f32(p, x); put_f32(p, y); put_i32(p, str_id); put_i32(p, rgb);
}

/* The one synchronous JNI round trip in the design, and the reason the memo
 * table exists. Called from the layout path thousands of times per layout. */
double vta_text_width(void *vp, int32_t str_id) {
    Painter *p = (Painter *)vp;
    if (!p || str_id <= 0) return 0.0;

    double hit = vta_text_get_width(p->text, str_id, p->font_size, p->weight);
    if (hit >= 0.0) return hit;

    JNIEnv *env = vta_env();
    if (!env) return 0.0;
    resolve_ids(env);
    if (!m_measureText) return 0.0;

    jfloat w = (*env)->CallFloatMethod(env, vta_commands(), m_measureText,
                                       (jint)str_id, (jfloat)p->font_size,
                                       (jint)p->weight);
    vta_text_put_width(p->text, str_id, p->font_size, p->weight, (double)w);
    return (double)w;
}

static void fetch_metrics(Painter *p, double *ascent, double *height) {
    if (vta_text_get_metrics(p->text, p->font_size, p->weight, ascent, height)) return;

    *ascent = 0.0; *height = 0.0;
    JNIEnv *env = vta_env();
    if (!env) return;
    resolve_ids(env);
    if (!m_fontAscent || !m_fontHeight) return;

    jfloat a = (*env)->CallFloatMethod(env, vta_commands(), m_fontAscent,
                                       (jfloat)p->font_size, (jint)p->weight);
    jfloat h = (*env)->CallFloatMethod(env, vta_commands(), m_fontHeight,
                                       (jfloat)p->font_size, (jint)p->weight);
    *ascent = (double)a;
    *height = (double)h;
    vta_text_put_metrics(p->text, p->font_size, p->weight, *ascent, *height);
}

double vta_font_ascent(void *vp) {
    Painter *p = (Painter *)vp;
    if (!p) return 0.0;
    double a, h;
    fetch_metrics(p, &a, &h);
    return a;
}

double vta_font_height(void *vp) {
    Painter *p = (Painter *)vp;
    if (!p) return 0.0;
    double a, h;
    fetch_metrics(p, &a, &h);
    return h;
}

/* ----------------------------------------------------------------- images */

void *vta_load_image(void *vp, const char *path) {
    Painter *p = (Painter *)vp;
    if (!p || !path) return NULL;
    JNIEnv *env = vta_env();
    if (!env) return NULL;
    resolve_ids(env);
    if (!m_loadImage) return NULL;

    jstring js = (*env)->NewStringUTF(env, path);
    if (!js) return NULL;
    jint handle = (*env)->CallIntMethod(env, vta_commands(), m_loadImage, js);
    (*env)->DeleteLocalRef(env, js);
    if (handle <= 0) return NULL;

    /* Handles are small ints carried through Vyto's opaque rawptr. Offsetting
     * by nothing is fine because 0 already means failure on both sides. */
    return (void *)(intptr_t)handle;
}

void vta_draw_image(void *vp, void *handle, double x, double y, double w, double h) {
    Painter *p = (Painter *)vp;
    int32_t id = (int32_t)(intptr_t)handle;
    if (!p || id <= 0 || !op(p, OP_IMAGE, 5)) return;
    put_i32(p, id);
    put_rect(p, x, y, w, h);
}

void vta_free_image(void *vp, void *handle) {
    Painter *p = (Painter *)vp;
    int32_t id = (int32_t)(intptr_t)handle;
    if (!p || id <= 0) return;
    JNIEnv *env = vta_env();
    if (!env) return;
    resolve_ids(env);
    if (m_dropImage) (*env)->CallVoidMethod(env, vta_commands(), m_dropImage, (jint)id);
}

/* ---------------------------------------------------------------- present */

static void flush(Painter *p, int x, int y, int w, int h) {
    if (p->len == 0) return;

    JNIEnv *env = vta_env();
    if (!env) { p->len = 0; return; }
    resolve_ids(env);
    if (!m_replayAndPost) { p->len = 0; return; }

    /* Terminator first: it can grow the buffer, and growing invalidates any
     * DirectByteBuffer wrapping the old pointer. Appending after the wrapper
     * exists would hand Java a stale address. */
    if (ensure(p, 4)) put_i32(p, OP_END);

    if (!p->bytebuf) {
        jobject local = (*env)->NewDirectByteBuffer(env, p->buf, (jlong)p->cap);
        if (!local) { p->len = 0; return; }
        p->bytebuf = (*env)->NewGlobalRef(env, local);
        (*env)->DeleteLocalRef(env, local);
        if (!p->bytebuf) { p->len = 0; return; }
    }

    (*env)->CallVoidMethod(env, vta_view(), m_replayAndPost,
                           p->bytebuf, (jint)p->len,
                           (jint)x, (jint)y, (jint)w, (jint)h);
    /* An exception thrown inside replay (a Canvas that refused a restore, an
     * OOM building a Path) stays pending on this thread, and the *next* JNI
     * call on it is undefined behaviour rather than a failure — so a single bad
     * frame would corrupt everything the shim does afterwards. Clear it here,
     * which costs one check per frame and confines the damage to that frame. */
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        vta_logf("frame replay threw — frame dropped");
    }
    p->len = 0;
}

void vta_present(void *vp) {
    Painter *p = (Painter *)vp;
    if (!p) return;
    flush(p, 0, 0, p->w, p->h);
}

void vta_present_rect(void *vp, int32_t x, int32_t y, int32_t w, int32_t h) {
    Painter *p = (Painter *)vp;
    if (!p) return;
    flush(p, x, y, w, h);
}

#endif /* __ANDROID__ */
