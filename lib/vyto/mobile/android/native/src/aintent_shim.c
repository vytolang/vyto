/* aintent_shim.c — the vta_* action API and the result queue.
 *
 * Java half: dev.vyto.android.Actions. Vyto half: vyto/mobile/android/actions.
 *
 * STATUS: written, never compiled — no NDK toolchain in this clone. The body
 * is wrapped in #ifdef __ANDROID__, so it is empty on every other target.
 * Design of record: local/docs/ANDROID.md.
 *
 * -- threading -----------------------------------------------------------------
 *
 * Two threads meet in this file, which is the whole reason it is not a set of
 * direct calls:
 *
 *   - Launchers are called from the VYTO thread and call up into Java. The
 *     Java side marshals to the UI thread where required (startActivity,
 *     requestPermissions), but the resolveActivity check happens synchronously
 *     so a "no app can handle this" answer still comes back as a return value.
 *   - Results are pushed from the UI THREAD by jni_boot.c and drained from the
 *     VYTO thread by Actions.pump(). That crossing is the queue below.
 *
 * Nothing here ever calls into Vyto code. A callback must run on the Vyto
 * thread, so the queue is polled rather than dispatched.
 */
/* vytoc globs native/src/*.c flat and compiles every file for every
 * target, with no per-file platform filter, so an Android-only source
 * must exclude its own body. Same pattern as vsurf_android.c. */
#ifdef __ANDROID__

#include "vyto_android.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ queue */

typedef struct Result {
    struct Result *next;
    int    request_id;
    int    ok;
    char **uris;      /* owned */
    int    n_uris;
} Result;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Result *g_head = NULL;   /* FIFO: pop head, push tail */
static Result *g_tail = NULL;

/* The result vta_result_poll() last handed out. Its accessors read from here,
 * so it must outlive the poll call — freed on the next poll. Only ever touched
 * by the Vyto thread, so it needs no lock. */
static Result *g_current = NULL;

static void result_free(Result *r) {
    if (!r) return;
    for (int i = 0; i < r->n_uris; i++) free(r->uris[i]);
    free(r->uris);
    free(r);
}

/* Takes ownership of uris and every string in it. Called from the UI thread. */
void vta_result_push(int request_id, int ok, char **uris, int n_uris) {
    Result *r = (Result *)calloc(1, sizeof(Result));
    if (!r) {
        for (int i = 0; i < n_uris; i++) free(uris[i]);
        free(uris);
        return;
    }
    r->request_id = request_id;
    r->ok = ok;
    r->uris = uris;
    r->n_uris = n_uris;

    pthread_mutex_lock(&g_lock);
    if (g_tail) g_tail->next = r; else g_head = r;
    g_tail = r;
    pthread_mutex_unlock(&g_lock);

    /* No wake: Actions.pump() is polled from the app's loop turn, not blocked
     * on. If a result arrives while the loop is idle in vs_wait it sits here
     * until the next event — which is the KNOWN GAP recorded in actions.vt,
     * and the reason an EV_ACTION_RESULT event type belongs with the Track B
     * lifecycle work. */
}

void vta_intent_shutdown(void) {
    pthread_mutex_lock(&g_lock);
    Result *r = g_head;
    g_head = g_tail = NULL;
    pthread_mutex_unlock(&g_lock);

    while (r) { Result *n = r->next; result_free(r); r = n; }
    result_free(g_current);
    g_current = NULL;
}

int32_t vta_result_poll(void) {
    result_free(g_current);
    g_current = NULL;

    pthread_mutex_lock(&g_lock);
    Result *r = g_head;
    if (r) {
        g_head = r->next;
        if (!g_head) g_tail = NULL;
        r->next = NULL;
    }
    pthread_mutex_unlock(&g_lock);

    if (!r) return -1;
    g_current = r;
    return r->request_id;
}

int32_t vta_result_ok(void)    { return g_current ? g_current->ok : 0; }
int32_t vta_result_count(void) { return g_current ? g_current->n_uris : 0; }

const char *vta_result_uri(int32_t i) {
    if (!g_current || i < 0 || i >= g_current->n_uris) return "";
    return g_current->uris[i] ? g_current->uris[i] : "";
}

/* ------------------------------------------------------- Java call helpers */

static jmethodID m_pickDocument = NULL, m_createDocument = NULL;
static jmethodID m_pickDirectory = NULL, m_pickMedia = NULL;
static jmethodID m_captureImage = NULL, m_captureVideo = NULL;
static jmethodID m_shareText = NULL, m_shareUri = NULL;
static jmethodID m_viewUrl = NULL, m_dial = NULL, m_sendEmail = NULL;
static jmethodID m_openAppSettings = NULL;
static jmethodID m_hasPermission = NULL, m_shouldExplain = NULL, m_requestPermission = NULL;
static jmethodID m_openFd = NULL, m_displayName = NULL, m_sizeOf = NULL, m_takePersistable = NULL;
static int ids_ready = 0;

static void resolve_ids(JNIEnv *env) {
    if (ids_ready) return;
    jobject a = vta_actions();
    if (!a) return;
    jclass c = (*env)->GetObjectClass(env, a);

    m_pickDocument     = (*env)->GetMethodID(env, c, "pickDocument", "(Ljava/lang/String;ZI)I");
    m_createDocument   = (*env)->GetMethodID(env, c, "createDocument", "(Ljava/lang/String;Ljava/lang/String;I)I");
    m_pickDirectory    = (*env)->GetMethodID(env, c, "pickDirectory", "(I)I");
    m_pickMedia        = (*env)->GetMethodID(env, c, "pickMedia", "(Ljava/lang/String;ZI)I");
    m_captureImage     = (*env)->GetMethodID(env, c, "captureImage", "(I)I");
    m_captureVideo     = (*env)->GetMethodID(env, c, "captureVideo", "(I)I");
    m_shareText        = (*env)->GetMethodID(env, c, "shareText", "(Ljava/lang/String;Ljava/lang/String;)I");
    m_shareUri         = (*env)->GetMethodID(env, c, "shareUri", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I");
    m_viewUrl          = (*env)->GetMethodID(env, c, "viewUrl", "(Ljava/lang/String;)I");
    m_dial             = (*env)->GetMethodID(env, c, "dial", "(Ljava/lang/String;)I");
    m_sendEmail        = (*env)->GetMethodID(env, c, "sendEmail", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I");
    m_openAppSettings  = (*env)->GetMethodID(env, c, "openAppSettings", "()I");
    m_hasPermission    = (*env)->GetMethodID(env, c, "hasPermission", "(Ljava/lang/String;)Z");
    m_shouldExplain    = (*env)->GetMethodID(env, c, "shouldExplain", "(Ljava/lang/String;)Z");
    m_requestPermission= (*env)->GetMethodID(env, c, "requestPermission", "(Ljava/lang/String;I)I");
    m_openFd           = (*env)->GetMethodID(env, c, "openFd", "(Ljava/lang/String;Ljava/lang/String;)I");
    m_displayName      = (*env)->GetMethodID(env, c, "displayName", "(Ljava/lang/String;)Ljava/lang/String;");
    m_sizeOf           = (*env)->GetMethodID(env, c, "sizeOf", "(Ljava/lang/String;)J");
    m_takePersistable  = (*env)->GetMethodID(env, c, "takePersistable", "(Ljava/lang/String;)Z");

    (*env)->DeleteLocalRef(env, c);
    ids_ready = 1;
}

/* Small helpers so each entry point stays one line of intent. Every one of
 * these must DeleteLocalRef its jstrings: a long-lived Vyto thread that leaks
 * one per call will exhaust the 512-entry local table. */
#define ENV_OR(ret) \
    JNIEnv *env = vta_env(); \
    if (!env || !vta_actions()) return (ret); \
    resolve_ids(env); \
    if (!ids_ready) return (ret)

static jstring mkstr(JNIEnv *env, const char *s) {
    return (*env)->NewStringUTF(env, s ? s : "");
}

/* ------------------------------------------------------------- launchers */

int32_t vta_pick_document(const char *mime, int32_t multi, int32_t req) {
    ENV_OR(-1);
    jstring m = mkstr(env, mime);
    jint rc = (*env)->CallIntMethod(env, vta_actions(), m_pickDocument,
                                    m, (jboolean)(multi != 0), (jint)req);
    (*env)->DeleteLocalRef(env, m);
    return rc;
}

int32_t vta_create_document(const char *mime, const char *name, int32_t req) {
    ENV_OR(-1);
    jstring m = mkstr(env, mime);
    jstring n = mkstr(env, name);
    jint rc = (*env)->CallIntMethod(env, vta_actions(), m_createDocument, m, n, (jint)req);
    (*env)->DeleteLocalRef(env, m);
    (*env)->DeleteLocalRef(env, n);
    return rc;
}

int32_t vta_pick_directory(int32_t req) {
    ENV_OR(-1);
    return (*env)->CallIntMethod(env, vta_actions(), m_pickDirectory, (jint)req);
}

int32_t vta_pick_media(const char *mime, int32_t multi, int32_t req) {
    ENV_OR(-1);
    jstring m = mkstr(env, mime);
    jint rc = (*env)->CallIntMethod(env, vta_actions(), m_pickMedia,
                                    m, (jboolean)(multi != 0), (jint)req);
    (*env)->DeleteLocalRef(env, m);
    return rc;
}

int32_t vta_capture_image(int32_t req) {
    ENV_OR(-1);
    return (*env)->CallIntMethod(env, vta_actions(), m_captureImage, (jint)req);
}

int32_t vta_capture_video(int32_t req) {
    ENV_OR(-1);
    return (*env)->CallIntMethod(env, vta_actions(), m_captureVideo, (jint)req);
}

/* ------------------------------------------------------- fire and forget */

int32_t vta_share_text(const char *text, const char *title) {
    ENV_OR(-1);
    jstring a = mkstr(env, text), b = mkstr(env, title);
    jint rc = (*env)->CallIntMethod(env, vta_actions(), m_shareText, a, b);
    (*env)->DeleteLocalRef(env, a); (*env)->DeleteLocalRef(env, b);
    return rc;
}

int32_t vta_share_uri(const char *uri, const char *mime, const char *title) {
    ENV_OR(-1);
    jstring a = mkstr(env, uri), b = mkstr(env, mime), c = mkstr(env, title);
    jint rc = (*env)->CallIntMethod(env, vta_actions(), m_shareUri, a, b, c);
    (*env)->DeleteLocalRef(env, a); (*env)->DeleteLocalRef(env, b);
    (*env)->DeleteLocalRef(env, c);
    return rc;
}

int32_t vta_view_url(const char *url) {
    ENV_OR(-1);
    jstring a = mkstr(env, url);
    jint rc = (*env)->CallIntMethod(env, vta_actions(), m_viewUrl, a);
    (*env)->DeleteLocalRef(env, a);
    return rc;
}

int32_t vta_dial(const char *number) {
    ENV_OR(-1);
    jstring a = mkstr(env, number);
    jint rc = (*env)->CallIntMethod(env, vta_actions(), m_dial, a);
    (*env)->DeleteLocalRef(env, a);
    return rc;
}

int32_t vta_send_email(const char *to, const char *subject, const char *body) {
    ENV_OR(-1);
    jstring a = mkstr(env, to), b = mkstr(env, subject), c = mkstr(env, body);
    jint rc = (*env)->CallIntMethod(env, vta_actions(), m_sendEmail, a, b, c);
    (*env)->DeleteLocalRef(env, a); (*env)->DeleteLocalRef(env, b);
    (*env)->DeleteLocalRef(env, c);
    return rc;
}

int32_t vta_open_app_settings(void) {
    ENV_OR(-1);
    return (*env)->CallIntMethod(env, vta_actions(), m_openAppSettings);
}

/* ----------------------------------------------------------- permissions */

int32_t vta_permission_has(const char *name) {
    ENV_OR(0);
    jstring a = mkstr(env, name);
    jboolean b = (*env)->CallBooleanMethod(env, vta_actions(), m_hasPermission, a);
    (*env)->DeleteLocalRef(env, a);
    return b ? 1 : 0;
}

int32_t vta_permission_should_explain(const char *name) {
    ENV_OR(0);
    jstring a = mkstr(env, name);
    jboolean b = (*env)->CallBooleanMethod(env, vta_actions(), m_shouldExplain, a);
    (*env)->DeleteLocalRef(env, a);
    return b ? 1 : 0;
}

int32_t vta_permission_request(const char *name, int32_t req) {
    ENV_OR(-1);
    jstring a = mkstr(env, name);
    jint rc = (*env)->CallIntMethod(env, vta_actions(), m_requestPermission, a, (jint)req);
    (*env)->DeleteLocalRef(env, a);
    return rc;
}

/* ------------------------------------------------------- content:// bridge */

int32_t vta_uri_open_fd(const char *uri, const char *mode) {
    ENV_OR(-1);
    jstring a = mkstr(env, uri), b = mkstr(env, mode);
    jint fd = (*env)->CallIntMethod(env, vta_actions(), m_openFd, a, b);
    (*env)->DeleteLocalRef(env, a); (*env)->DeleteLocalRef(env, b);
    return fd;
}

/* Returned pointer stays valid until the next call, matching the contract in
 * actions.vt. One static buffer, only ever read from the Vyto thread. */
static char *g_name_buf = NULL;

const char *vta_uri_display_name(const char *uri) {
    JNIEnv *env = vta_env();
    if (!env || !vta_actions()) return "";
    resolve_ids(env);
    if (!ids_ready) return "";

    jstring a = mkstr(env, uri);
    jstring js = (jstring)(*env)->CallObjectMethod(env, vta_actions(), m_displayName, a);
    (*env)->DeleteLocalRef(env, a);
    if (!js) return "";

    const char *s = (*env)->GetStringUTFChars(env, js, NULL);
    free(g_name_buf);
    g_name_buf = s ? strdup(s) : NULL;
    if (s) (*env)->ReleaseStringUTFChars(env, js, s);
    (*env)->DeleteLocalRef(env, js);
    return g_name_buf ? g_name_buf : "";
}

int64_t vta_uri_size(const char *uri) {
    ENV_OR(-1);
    jstring a = mkstr(env, uri);
    jlong n = (*env)->CallLongMethod(env, vta_actions(), m_sizeOf, a);
    (*env)->DeleteLocalRef(env, a);
    return (int64_t)n;
}

int32_t vta_uri_take_persistable(const char *uri) {
    ENV_OR(0);
    jstring a = mkstr(env, uri);
    jboolean b = (*env)->CallBooleanMethod(env, vta_actions(), m_takePersistable, a);
    (*env)->DeleteLocalRef(env, a);
    return b ? 1 : 0;
}

#endif /* __ANDROID__ */
