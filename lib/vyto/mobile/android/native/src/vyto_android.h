/* vyto_android.h — internal contract between the Android shims.
 *
 * Not a public API. These symbols are shared between jni_boot.c,
 * apainter_shim.c, atext_cache.c, aintent_shim.c, and the Android arm of
 * surface/native/src/vsurf.c. Everything links into one .so, so the vsurf arm
 * can call in here despite living in a different package directory.
 *
 * STATUS: written, never compiled — no NDK toolchain in this clone. This
 * header includes <jni.h> unconditionally, so every file that includes it
 * must guard itself with #ifdef __ANDROID__; vytoc compiles native/src/*.c
 * for every target with no per-file platform filter.
 * Design of record: local/docs/ANDROID.md.
 */
#ifndef VYTO_ANDROID_H
#define VYTO_ANDROID_H

#include <jni.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ JNI env
 *
 * Two threads call into Java: the Android UI thread (input, lifecycle,
 * results) and the Vyto thread (rendering, measurement). A JNIEnv is
 * per-thread and must never be cached across threads, so everything goes
 * through vta_env(), which attaches the calling thread on first use.
 */
JNIEnv *vta_env(void);

/* Cached global refs, valid between Native.start and Native.stop.
 * NULL before start — every caller must tolerate that, because the Vyto
 * thread can outlive a stop by a few instructions. */
jobject vta_view(void);      /* dev.vyto.android.VytoView */
jobject vta_commands(void);  /* dev.vyto.android.CommandBuffer */
jobject vta_actions(void);   /* dev.vyto.android.Actions */

/* Log helper; wraps __android_log_print so the shims do not each include it. */
void vta_logf(const char *fmt, ...);

/* ------------------------------------------------------------- app entry
 *
 * Emitted by vytoc under --shared instead of `int main` (Track A, src/emit.c).
 * Called once on the Vyto thread. Returns when the app's main returns, which
 * for a UI app means Window.run() exited.
 */
void vyto_app_main(void);

/* -------------------------------------------------------- surface bridge
 *
 * Implemented by the Android arm of vsurf.c. jni_boot.c pushes events in from
 * the UI thread; the Vyto thread drains them inside vs_wait/vs_poll.
 * All of these are safe to call from any thread.
 */
void vs_android_bind(int w, int h, float density);
void vs_android_unbind(void);
void vs_android_push_touch(int action, int pointer_id, float x, float y, int64_t time_ms);
void vs_android_push_key(int keycode, const char *utf8, int mods, int down);
void vs_android_push_resize(int w, int h, float density);
void vs_android_push_insets(int l, int t, int r, int b, int ime_h);
void vs_android_set_paused(int paused);
/* Ask the loop to exit: makes vs_wait return VS_EV_CLOSE. */
void vs_android_request_close(void);

/* ------------------------------------------------------------ intent queue
 *
 * Implemented by aintent_shim.c. Pushed from the UI thread by jni_boot.c,
 * drained from the Vyto thread by Actions.pump() in actions.vt.
 */
void vta_result_push(int request_id, int ok, char **uris, int n_uris);
void vta_intent_shutdown(void);

/* ---------------------------------------------------------------- network
 *
 * Implemented by anet_shim.c over dev.vyto.android.Http. Blocking, and called
 * only from the Vyto thread — which is a background pthread, so blocking is
 * correct here and would be an exception on the UI thread.
 *
 * Two handle shapes come back and vta_http_free takes either: a finished
 * response (vta_http_perform) or a live connection (vta_http_open).
 */
/* Bind the Java classes these shims call. MUST be called from JNI_OnLoad and
 * nowhere else: FindClass uses the calling frame's class loader, and a
 * pthread attached with AttachCurrentThread has none, so it falls back to the
 * system loader and cannot see anything in the apk. */
void vta_http_bind(JNIEnv *env);
void vta_intl_bind(JNIEnv *env);

void *vta_http_perform(const char *method, const char *url, const char *header_lines,
                       const char *body, int64_t body_len, int64_t timeout_ms);
int64_t vta_http_status(void *h);
const char *vta_http_body_data(void *h);
int64_t vta_http_body_len(void *h);
int64_t vta_http_body_copy(void *h, char *out, int64_t cap);
const char *vta_http_headers(void *h);
void vta_http_free(void *h);

void *vta_http_open(const char *method, const char *url, const char *header_lines,
                    const char *body, int64_t body_len, int64_t timeout_ms);
/* Bytes read, or -1 at end of stream and on error alike. */
int32_t vta_http_read(void *h, char *buf, int32_t cap);
void vta_http_chunk_copy(const char *src, char *dst, int32_t len);

void *vta_http_pool_new(int32_t max_parallel);
int32_t vta_http_pool_add(void *p, const char *method, const char *url,
                          const char *header_lines, const char *body,
                          int64_t body_len, int64_t timeout_ms);
int32_t vta_http_pool_next(void *p, int32_t timeout_ms);
void *vta_http_pool_take(void *p, int32_t id);
void vta_http_pool_free(void *p);

/* ----------------------------------------------------------------- intl
 *
 * Implemented by aintl_shim.c over dev.vyto.android.Intl (android.icu.*).
 *
 * Every open() returns a JNI global ref cast to void*, and its close() is the
 * matching DeleteGlobalRef — there is no C-side struct.
 *
 * Every formatting call writes UTF-8 into out/cap and returns the byte length
 * the result *wants*, so a caller whose buffer was too small retries exactly
 * once at the returned size. A result that fits is NUL-terminated; -1 is a
 * failure the Java side has already logged.
 */
int32_t vta_intl_default_locale(char *out, int32_t cap);

void *vta_intl_num_open(const char *locale, int32_t style);
void vta_intl_num_close(void *h);
int32_t vta_intl_num_fmt_double(void *h, double v, char *out, int32_t cap);
int32_t vta_intl_num_fmt_int(void *h, int64_t v, char *out, int32_t cap);
int32_t vta_intl_num_fmt_currency(void *h, double v, const char *iso3,
                                  char *out, int32_t cap);
void vta_intl_num_set_fraction(void *h, int32_t minFrac, int32_t maxFrac);

void *vta_intl_dat_open(const char *locale, int32_t dateStyle, int32_t timeStyle,
                        const char *tz);
void vta_intl_dat_close(void *h);
int32_t vta_intl_dat_fmt(void *h, int64_t unix_ms, char *out, int32_t cap);

int32_t vta_intl_normalize(const char *s, int32_t mode, char *out, int32_t cap);
int32_t vta_intl_case(const char *s, const char *locale, int32_t op,
                      char *out, int32_t cap);

void *vta_intl_brk_open(int32_t kind, const char *locale, const char *s);
/* Next boundary as a UTF-8 *byte* offset, or -1 at the end. ICU indexes UTF-16,
 * so the conversion happens on the Java side where both encodings are in hand. */
int32_t vta_intl_brk_next(void *h);
void vta_intl_brk_close(void *h);

void *vta_intl_col_open(const char *locale);
int32_t vta_intl_col_compare(void *h, const char *a, const char *b);
/* Binary, copied by length and never NUL-terminated — a sort key is not text. */
int32_t vta_intl_col_sortkey(void *h, const char *s, char *out, int32_t cap);
void vta_intl_col_close(void *h);

void *vta_intl_plural_open(const char *locale, int32_t kind);
int32_t vta_intl_plural_select(void *h, double v, char *out, int32_t cap);
void vta_intl_plural_close(void *h);

/* --------------------------------------------------------------- text cache
 *
 * Implemented by atext_cache.c. Pure C — no JNI. The caller does the JNI round
 * trip on a miss and inserts the answer, which keeps the one expensive path
 * (Paint.measureText) visible at the call site instead of buried in a lookup.
 */
typedef struct VtaText VtaText;

VtaText *vta_text_new(void);
void vta_text_free(VtaText *t);

/* Stable id for a string, allocating on first sight.
 * *out_is_new is set to 1 when the caller must now define the string on the
 * Java side; the id is otherwise already known to Java. */
int32_t vta_text_intern(VtaText *t, const char *s, int *out_is_new);

/* Measured advance for (id, size, weight), or -1.0 on a miss. */
double vta_text_get_width(VtaText *t, int32_t id, double size, int weight);
void vta_text_put_width(VtaText *t, int32_t id, double size, int weight, double w);

/* Font metrics memo, keyed on (size, weight) alone. */
int vta_text_get_metrics(VtaText *t, double size, int weight,
                         double *out_ascent, double *out_height);
void vta_text_put_metrics(VtaText *t, double size, int weight,
                          double ascent, double height);

#ifdef __cplusplus
}
#endif
#endif /* VYTO_ANDROID_H */
