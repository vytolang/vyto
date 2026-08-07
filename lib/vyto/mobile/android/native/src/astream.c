/* astream.c — sampled state from the platform: sensors and location.
 *
 * Deliberately NOT the queue that aintent_shim.c uses. An intent result is an
 * event: there are few, each matters, and losing one loses a user's action. A
 * sensor is state: an accelerometer at SENSOR_DELAY_GAME produces 50-200
 * samples a second, a UI reads "what is it now" once a frame, and a queue
 * would grow without bound between reads for no gain at all. So each channel
 * is one slot, and a new sample overwrites the old one.
 *
 * Five doubles per slot because location needs five (lat, lon, alt, speed,
 * accuracy) and a sensor needs three. Separate scalar getters rather than one
 * call with out-params, for the same reason ainsets.c has five: Vyto's FFI
 * takes C-shaped scalars, and a pointer dance at every call site would be
 * worse than a lock per read. The lock is uncontended in practice — one writer
 * on a HandlerThread, one reader on the Vyto thread.
 *
 * `fresh` is what lets a widget skip a repaint: it is set on every put and
 * cleared by the reader, so "has this changed since I last looked" costs one
 * call and no state on the Vyto side.
 *
 * Both arms, like ainsets.c: this file compiles for every target, and the
 * desktop arm reports "no sample ever" rather than being #ifdef-ed away, so
 * sensors.vt and location.vt link and run (returning null) on a laptop.
 */

#include <stdint.h>
#include <string.h>

#define VTA_STREAM_CHANNELS 8
#define VTA_STREAM_VALUES   5

#ifdef __ANDROID__

#include "vyto_android.h"
#include <pthread.h>

typedef struct {
    double v[VTA_STREAM_VALUES];
    int64_t t_ms;     /* 0 until the first sample lands */
    int fresh;
} Slot;

static Slot g_slots[VTA_STREAM_CHANNELS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Called from Java on the HandlerThread the sensor and location callbacks run
 * on — never the UI thread, and never the Vyto thread. */
void vta_stream_put(int32_t ch, double v0, double v1, double v2,
                    double v3, double v4, int64_t t_ms) {
    if (ch < 0 || ch >= VTA_STREAM_CHANNELS) return;
    pthread_mutex_lock(&g_lock);
    Slot *s = &g_slots[ch];
    s->v[0] = v0; s->v[1] = v1; s->v[2] = v2; s->v[3] = v3; s->v[4] = v4;
    s->t_ms = t_ms;
    s->fresh = 1;
    pthread_mutex_unlock(&g_lock);
}

double vta_stream_v(int32_t ch, int32_t i) {
    if (ch < 0 || ch >= VTA_STREAM_CHANNELS || i < 0 || i >= VTA_STREAM_VALUES) return 0.0;
    pthread_mutex_lock(&g_lock);
    double d = g_slots[ch].v[i];
    pthread_mutex_unlock(&g_lock);
    return d;
}

/* 0 means no sample has ever arrived on this channel — which is how a caller
 * distinguishes "not started / no permission / no such sensor" from a genuine
 * reading of zero. */
int64_t vta_stream_t(int32_t ch) {
    if (ch < 0 || ch >= VTA_STREAM_CHANNELS) return 0;
    pthread_mutex_lock(&g_lock);
    int64_t t = g_slots[ch].t_ms;
    pthread_mutex_unlock(&g_lock);
    return t;
}

/* Reads and clears: "has this changed since I last asked". */
int32_t vta_stream_take_fresh(int32_t ch) {
    if (ch < 0 || ch >= VTA_STREAM_CHANNELS) return 0;
    pthread_mutex_lock(&g_lock);
    int f = g_slots[ch].fresh;
    g_slots[ch].fresh = 0;
    pthread_mutex_unlock(&g_lock);
    return (int32_t)f;
}

/* A stopped channel must not read as live forever — otherwise poll() keeps
 * handing back the last fix an hour after location was switched off. */
void vta_stream_clear(int32_t ch) {
    if (ch < 0 || ch >= VTA_STREAM_CHANNELS) return;
    pthread_mutex_lock(&g_lock);
    memset(&g_slots[ch], 0, sizeof g_slots[ch]);
    pthread_mutex_unlock(&g_lock);
}

/* ------------------------------------------------- up-calls into Streams.java
 *
 * GetObjectClass of the object the Activity bound, like every other up-call in
 * this package: no FindClass, so no class-loader trap. Resolved per call —
 * these happen when an app starts or stops a stream, not per sample.
 */
static jmethodID method_of(JNIEnv *env, jobject o, const char *name, const char *sig) {
    jclass c = (*env)->GetObjectClass(env, o);
    if (!c) return NULL;
    jmethodID m = (*env)->GetMethodID(env, c, name, sig);
    (*env)->DeleteLocalRef(env, c);
    return m;
}

/* Two spellings because C89 macros cannot have an empty argument portably and
 * a void function cannot `return 0`. */
#define STREAMS_OR(ret) \
    JNIEnv *env = vta_env(); \
    jobject st = vta_streams(); \
    if (!env || !st) return (ret)

#define STREAMS_OR_VOID() \
    JNIEnv *env = vta_env(); \
    jobject st = vta_streams(); \
    if (!env || !st) return

int32_t vta_sensor_start(int32_t kind, int32_t rate_us) {
    STREAMS_OR(0);
    jmethodID m = method_of(env, st, "startSensor", "(II)Z");
    if (!m) return 0;
    return (*env)->CallBooleanMethod(env, st, m, (jint)kind, (jint)rate_us) ? 1 : 0;
}

void vta_sensor_stop(int32_t kind) {
    STREAMS_OR_VOID();
    jmethodID m = method_of(env, st, "stopSensor", "(I)V");
    if (m) (*env)->CallVoidMethod(env, st, m, (jint)kind);
}

int32_t vta_sensor_has(int32_t kind) {
    STREAMS_OR(0);
    jmethodID m = method_of(env, st, "hasSensor", "(I)Z");
    if (!m) return 0;
    return (*env)->CallBooleanMethod(env, st, m, (jint)kind) ? 1 : 0;
}

int32_t vta_location_start(int64_t min_ms, double min_m) {
    STREAMS_OR(0);
    jmethodID m = method_of(env, st, "startLocation", "(JD)Z");
    if (!m) return 0;
    return (*env)->CallBooleanMethod(env, st, m, (jlong)min_ms, (jdouble)min_m) ? 1 : 0;
}

void vta_location_stop(void) {
    STREAMS_OR_VOID();
    jmethodID m = method_of(env, st, "stopLocation", "()V");
    if (m) (*env)->CallVoidMethod(env, st, m);
}

#else

void vta_stream_put(int32_t ch, double v0, double v1, double v2,
                    double v3, double v4, int64_t t_ms) {
    (void)ch; (void)v0; (void)v1; (void)v2; (void)v3; (void)v4; (void)t_ms;
}
double vta_stream_v(int32_t ch, int32_t i) { (void)ch; (void)i; return 0.0; }
int64_t vta_stream_t(int32_t ch) { (void)ch; return 0; }
int32_t vta_stream_take_fresh(int32_t ch) { (void)ch; return 0; }
void vta_stream_clear(int32_t ch) { (void)ch; }

int32_t vta_sensor_start(int32_t kind, int32_t rate_us) { (void)kind; (void)rate_us; return 0; }
void vta_sensor_stop(int32_t kind) { (void)kind; }
int32_t vta_sensor_has(int32_t kind) { (void)kind; return 0; }
int32_t vta_location_start(int64_t min_ms, double min_m) { (void)min_ms; (void)min_m; return 0; }
void vta_location_stop(void) { }

#endif
