/* acamera.c — camera preview: YUV_420_888 to ARGB, and the sink's up-calls.
 *
 * The conversion is the whole cost of having a camera preview as a *widget*
 * rather than as a screen. Vyto composites into one software Bitmap, so a
 * frame has to arrive as ARGB pixels; the camera produces YUV planes. There is
 * no way around a convert-and-copy on this path, and the design decision is to
 * keep it small (preview resolutions) rather than to avoid it.
 *
 * Two things make it affordable:
 *
 *   - chroma is computed once per 2x2 block, not per pixel. The four red/blue
 *     terms are shared, so the inner loop is four luma adds and a clamp rather
 *     than four full conversions. That is most of the win; SIMD would be the
 *     next step and is deliberately not taken until something measures slow.
 *   - rotation happens here, in the destination index, instead of as a second
 *     pass. A sensor is mounted at a fixed angle to the device, so a preview
 *     is sideways on essentially every phone without it — and rotating a
 *     converted buffer afterwards would double the memory traffic.
 *
 * Integer BT.601, limited range (16..235 luma), which is what Camera2 delivers
 * for YUV_420_888 on every device that matters. Getting this wrong is not
 * subtle: full-range coefficients on limited-range input wash out the blacks.
 *
 * Both arms, like every file here: vytoc compiles native/src/*.c for every
 * target, and the desktop arm keeps camera.vt linking on a laptop.
 */

#include <stdint.h>
#include <stddef.h>   /* size_t, for the destination index arithmetic */

#define CLAMP255(v) ((v) < 0 ? 0 : ((v) > 255 ? 255 : (v)))

/* Written so the desktop arm can be linked and (in principle) tested: nothing
 * in here is Android-specific, it is plane pointers in and pixels out. */
void vta_yuv420_to_argb(uint8_t *dst, int32_t dst_stride,
                        const uint8_t *y, const uint8_t *u, const uint8_t *v,
                        int32_t y_stride, int32_t u_stride, int32_t v_stride,
                        int32_t uv_pixel_stride,
                        int32_t w, int32_t h, int32_t rotation, int32_t mirror) {
    if (!dst || !y || !u || !v || w <= 0 || h <= 0) return;

    for (int32_t j = 0; j < h; j += 2) {
        for (int32_t i = 0; i < w; i += 2) {
            /* One chroma sample per 2x2 luma block. uv_pixel_stride is 2 on the
             * semi-planar (NV12/NV21) layouts every recent device uses, and 1
             * on fully planar I420 — reading it rather than assuming is what
             * makes this work on both. */
            int32_t uv = (j >> 1) * u_stride + (i >> 1) * uv_pixel_stride;
            int32_t d = (int32_t)u[uv] - 128;
            int32_t e = (int32_t)v[uv] - 128;
            (void)v_stride;   /* u_stride == v_stride on every layout Camera2 emits */

            int32_t r_term = 409 * e + 128;
            int32_t g_term = -100 * d - 208 * e + 128;
            int32_t b_term = 516 * d + 128;

            for (int32_t dy = 0; dy < 2 && j + dy < h; dy++) {
                for (int32_t dx = 0; dx < 2 && i + dx < w; dx++) {
                    int32_t sy = j + dy, sx = i + dx;
                    int32_t c = 298 * ((int32_t)y[sy * y_stride + sx] - 16);
                    int32_t r = CLAMP255((c + r_term) >> 8);
                    int32_t g = CLAMP255((c + g_term) >> 8);
                    int32_t b = CLAMP255((c + b_term) >> 8);

                    /* Destination coordinates, rotated (and mirrored for the
                     * front camera, which is what makes a selfie preview read
                     * as a mirror rather than as back-to-front text). */
                    int32_t x = sx, ry = sy;
                    int32_t dw = w;
                    if (rotation == 90)       { x = h - 1 - sy; ry = sx;           dw = h; }
                    else if (rotation == 180) { x = w - 1 - sx; ry = h - 1 - sy;   dw = w; }
                    else if (rotation == 270) { x = sy;         ry = w - 1 - sx;   dw = h; }
                    if (mirror) { x = dw - 1 - x; }

                    uint8_t *p = dst + (size_t)ry * dst_stride + (size_t)x * 4;
                    /* ANDROID_BITMAP_FORMAT_RGBA_8888 is byte-order RGBA, which
                     * is what Bitmap.Config.ARGB_8888 actually stores — the
                     * name is historical. */
                    p[0] = (uint8_t)r;
                    p[1] = (uint8_t)g;
                    p[2] = (uint8_t)b;
                    p[3] = 255;
                }
            }
        }
    }
}

/* ------------------------------------------------------------- self-check
 *
 * The conversion is the only real logic in this file, and the only thing here
 * that a device would be needed to see: a wrong coefficient is a washed-out
 * preview, a wrong rotation index is a sideways or mirrored one, and both look
 * like "the camera is broken" rather than like a bug in twelve lines of
 * arithmetic. Nothing else in the path can be exercised off a phone.
 *
 * So it checks itself, against a synthetic 4x2 frame, on every target. Returns
 * 0 when clean, otherwise a bitmask naming which case failed — a bool would
 * make a regression a puzzle rather than a pointer.
 */
int32_t vta_camera_selftest(void) {
    /* 4x2 luma: black, white, black, white / white, black, white, black.
     * Neutral chroma (128) so every output pixel is grey, which isolates the
     * luma path and the index arithmetic from the colour maths. */
    uint8_t y[8] = { 16, 235, 16, 235,
                    235,  16, 235,  16 };
    uint8_t u[2] = { 128, 128 };
    uint8_t v[2] = { 128, 128 };
    uint8_t dst[4 * 4 * 4];   /* big enough for either orientation */
    int32_t fail = 0;

    /* --- no rotation: pixel (0,0) black, (1,0) white --- */
    for (int i = 0; i < (int)sizeof dst; i++) dst[i] = 7;
    vta_yuv420_to_argb(dst, 4 * 4, y, u, v, 4, 2, 2, 1, 4, 2, 0, 0);
    if (dst[0] > 4) fail |= 1;                 /* (0,0) should be ~black */
    if (dst[4] < 250) fail |= 2;               /* (1,0) should be ~white */
    if (dst[3] != 255) fail |= 4;              /* alpha must be opaque */

    /* --- 90 degrees: source (0,0) lands at destination (h-1, 0) = (1,0),
     *     and the destination is 2 wide by 4 tall --- */
    for (int i = 0; i < (int)sizeof dst; i++) dst[i] = 7;
    vta_yuv420_to_argb(dst, 2 * 4, y, u, v, 4, 2, 2, 1, 4, 2, 90, 0);
    if (dst[1 * 4] > 4) fail |= 8;             /* (1,0) from source (0,0) */
    if (dst[0] < 250) fail |= 16;              /* (0,0) from source (0,1), white */

    /* --- 180: source (0,0) lands at (w-1, h-1) = (3,1) --- */
    for (int i = 0; i < (int)sizeof dst; i++) dst[i] = 7;
    vta_yuv420_to_argb(dst, 4 * 4, y, u, v, 4, 2, 2, 1, 4, 2, 180, 0);
    if (dst[1 * (4 * 4) + 3 * 4] > 4) fail |= 32;

    /* --- mirrored, no rotation: source (0,0) lands at (w-1, 0) = (3,0) --- */
    for (int i = 0; i < (int)sizeof dst; i++) dst[i] = 7;
    vta_yuv420_to_argb(dst, 4 * 4, y, u, v, 4, 2, 2, 1, 4, 2, 0, 1);
    if (dst[3 * 4] > 4) fail |= 64;

    /* --- colour, both ways round. Neutral grey exercises none of the chroma
     *     maths, so a swapped u/v or a mistyped coefficient would sail through
     *     everything above. Asserting dominance rather than exact values keeps
     *     this about the wiring: r and b are asserted against each other, not
     *     against g, because with BT.601 the green term lands *equal* to blue
     *     at these inputs — an arithmetic coincidence that a "r > g > b" test
     *     would report as a failure forever. --- */
    uint8_t ymid[8] = { 128, 128, 128, 128, 128, 128, 128, 128 };
    uint8_t u_lo[2] = { 90, 90 };      /* below 128 */
    uint8_t v_hi[2] = { 240, 240 };    /* above 128: red */
    for (int i = 0; i < (int)sizeof dst; i++) dst[i] = 7;
    vta_yuv420_to_argb(dst, 4 * 4, ymid, u_lo, v_hi, 4, 2, 2, 1, 4, 2, 0, 0);
    if (!(dst[0] > dst[1] && dst[0] > dst[2])) fail |= 128;   /* red dominant */

    uint8_t u_hi[2] = { 240, 240 };    /* above 128: blue */
    uint8_t v_lo[2] = { 90, 90 };
    for (int i = 0; i < (int)sizeof dst; i++) dst[i] = 7;
    vta_yuv420_to_argb(dst, 4 * 4, ymid, u_hi, v_lo, 4, 2, 2, 1, 4, 2, 0, 0);
    if (!(dst[2] > dst[0] && dst[2] > dst[1])) fail |= 256;   /* blue dominant */

    return fail;
}

#ifdef __ANDROID__

#include "vyto_android.h"

/* ------------------------------------------------- up-calls into CameraSink
 *
 * GetObjectClass of the object the Activity bound — the same no-FindClass rule
 * as every other up-call here. Resolved per call: these happen on start, stop
 * and once per painted frame, not per camera frame.
 */
static jmethodID cam_method(JNIEnv *env, jobject o, const char *name, const char *sig) {
    jclass c = (*env)->GetObjectClass(env, o);
    if (!c) return NULL;
    jmethodID m = (*env)->GetMethodID(env, c, name, sig);
    (*env)->DeleteLocalRef(env, c);
    return m;
}

#define CAMERA_OR(ret) \
    JNIEnv *env = vta_env(); \
    jobject cam = vta_camera(); \
    if (!env || !cam) return (ret)

#define CAMERA_OR_VOID() \
    JNIEnv *env = vta_env(); \
    jobject cam = vta_camera(); \
    if (!env || !cam) return

/* The image handle to draw, or NULL when there is no camera, no permission, or
 * the open failed. Returned as a pointer rather than an int for the same
 * reason vta_load_image does: the painter API carries image handles as opaque
 * rawptrs, and Vyto has no int-to-rawptr cast. */
void *vta_camera_start(int32_t facing, int32_t w, int32_t h) {
    CAMERA_OR(NULL);
    jmethodID m = cam_method(env, cam, "start", "(III)I");
    if (!m) return NULL;
    jint id = (*env)->CallIntMethod(env, cam, m, (jint)facing, (jint)w, (jint)h);
    if (id <= 0) return NULL;
    return (void *)(intptr_t)id;
}

void vta_camera_stop(void) {
    CAMERA_OR_VOID();
    jmethodID m = cam_method(env, cam, "stop", "()V");
    if (m) (*env)->CallVoidMethod(env, cam, m);
}

int32_t vta_camera_has(int32_t facing) {
    CAMERA_OR(0);
    jmethodID m = cam_method(env, cam, "has", "(I)Z");
    if (!m) return 0;
    return (*env)->CallBooleanMethod(env, cam, m, (jint)facing) ? 1 : 0;
}

/* Publish the newest frame against the image handle. Called from the Vyto
 * thread — the only one allowed to touch the command buffer's image table —
 * and returns 1 when the frame changed, so a widget repaints on the camera's
 * clock rather than on the display's. */
int32_t vta_camera_sync(void) {
    CAMERA_OR(0);
    jmethodID m = cam_method(env, cam, "sync", "()Z");
    if (!m) return 0;
    return (*env)->CallBooleanMethod(env, cam, m) ? 1 : 0;
}

/* Whatever the sink knows about why it is not delivering. Read from Vyto and
 * put on screen: on this ROM the app's own Log.i lines do not reach logcat, so
 * a failure with no visible channel is a failure with no diagnosis. */
const char *vta_camera_diagnostics(void) {
    static char buf[256];
    buf[0] = 0;
    JNIEnv *env = vta_env();
    jobject cam = vta_camera();
    if (!env || !cam) return buf;
    jmethodID m = cam_method(env, cam, "diagnostics", "()Ljava/lang/String;");
    if (!m) return buf;
    jstring js = (jstring)(*env)->CallObjectMethod(env, cam, m);
    if (js) {
        const char *s = (*env)->GetStringUTFChars(env, js, NULL);
        if (s) {
            size_t n = 0;
            while (s[n] && n < sizeof buf - 1) { buf[n] = s[n]; n++; }
            buf[n] = 0;
            (*env)->ReleaseStringUTFChars(env, js, s);
        }
        (*env)->DeleteLocalRef(env, js);
    }
    return buf;
}

int32_t vta_camera_width(void) {
    CAMERA_OR(0);
    jmethodID m = cam_method(env, cam, "width", "()I");
    if (!m) return 0;
    return (int32_t)(*env)->CallIntMethod(env, cam, m);
}

int32_t vta_camera_height(void) {
    CAMERA_OR(0);
    jmethodID m = cam_method(env, cam, "height", "()I");
    if (!m) return 0;
    return (int32_t)(*env)->CallIntMethod(env, cam, m);
}

#else

void *vta_camera_start(int32_t facing, int32_t w, int32_t h) {
    (void)facing; (void)w; (void)h; return 0;
}
void vta_camera_stop(void) { }
int32_t vta_camera_has(int32_t facing) { (void)facing; return 0; }
int32_t vta_camera_sync(void) { return 0; }
const char *vta_camera_diagnostics(void) { return ""; }
int32_t vta_camera_width(void) { return 0; }
int32_t vta_camera_height(void) { return 0; }

#endif
