/* anet_shim.c — HTTP over the platform stack, for vyto/mobile/android/net.
 *
 * Vyto half: vyto/mobile/android/net. Java half: dev.vyto.android.Http.
 *
 * This file is a translator, not a client: every HTTP decision (redirects,
 * TLS, trust store, proxy, timeouts) belongs to HttpURLConnection, and
 * everything here does is marshal arguments across JNI and own the malloc'd
 * copies the Vyto side reads. See net.vt for why the platform stack rather
 * than a cross-built libcurl.
 *
 * STATUS: written, never compiled — no NDK toolchain in this clone. The body
 * is wrapped in #ifdef __ANDROID__, so it is empty on every other target.
 * Design of record: local/docs/ANDROID.md.
 *
 * -- ownership -----------------------------------------------------------------
 *
 * Two handle shapes cross to Vyto, and vta_http_free takes either, so both
 * start with a tag:
 *
 *   AResp  a finished response. Body and headers are malloc'd C copies; the
 *          Java objects are released before the call returns, so a Response
 *          held for a long time pins no JVM memory.
 *   AConn  a live connection being streamed. Holds global refs to the Java
 *          connection and to one reusable jbyteArray, released on free.
 *
 * -- local refs ----------------------------------------------------------------
 *
 * The Vyto thread is attached for its whole life, so its local reference table
 * is never popped by a returning native frame. Every jstring and every jobject
 * taken here must be deleted explicitly or a request loop exhausts the 512-entry
 * table and aborts the VM. That is the single easiest way to break this file.
 */
/* vytoc globs native/src/*.c flat and compiles every file for every
 * target, with no per-file platform filter, so an Android-only source
 * must exclude its own body. Same pattern as vsurf_android.c. */
#ifdef __ANDROID__

#include "vyto_android.h"

#include <stdlib.h>
#include <string.h>

#define AH_RESP 1
#define AH_CONN 2

typedef struct {
    int kind;          /* AH_RESP */
    char *body;
    int64_t blen;
    char *hdr;
    int64_t status;
} AResp;

typedef struct {
    int kind;          /* AH_CONN */
    jobject conn;      /* global ref: dev.vyto.android.Http$Conn */
    jbyteArray buf;    /* global ref: reusable read buffer */
    int32_t bufcap;
} AConn;

/* ------------------------------------------------------------ method cache */

static jclass  c_http = NULL;      /* global ref */
static jmethodID m_perform = NULL, m_open = NULL;
static jmethodID m_poolNew = NULL;
static jclass  c_conn = NULL;      /* global ref */
static jmethodID m_read = NULL, m_close = NULL;
static jfieldID  f_conn_status = NULL, f_conn_headers = NULL;
static jclass  c_resp = NULL;      /* global ref */
static jfieldID f_status = NULL, f_headers = NULL, f_body = NULL;
static jclass  c_pool = NULL;      /* global ref */
static jmethodID m_pool_add = NULL, m_pool_next = NULL, m_pool_take = NULL,
                 m_pool_shutdown = NULL;
static int ids_ready = 0;

/* FindClass returns a local ref that dies with the frame, and these are used
 * from a thread that never returns — so each is promoted to a global. */
static jclass global_class(JNIEnv *env, const char *name) {
    jclass local = (*env)->FindClass(env, name);
    if (!local) {
        (*env)->ExceptionClear(env);
        return NULL;
    }
    jclass g = (jclass)(*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    return g;
}

static void resolve_ids(JNIEnv *env) {
    if (ids_ready) return;

    c_http = global_class(env, "dev/vyto/android/Http");
    c_resp = global_class(env, "dev/vyto/android/Http$Resp");
    c_conn = global_class(env, "dev/vyto/android/Http$Conn");
    c_pool = global_class(env, "dev/vyto/android/Http$Pool");
    if (!c_http || !c_resp || !c_conn || !c_pool) {
        vta_logf("anet: Http classes not found — is the Java half in the apk?");
        return;
    }

    m_perform = (*env)->GetStaticMethodID(
        env, c_http, "perform",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[BI)"
        "Ldev/vyto/android/Http$Resp;");
    m_open = (*env)->GetStaticMethodID(
        env, c_http, "open",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[BI)"
        "Ldev/vyto/android/Http$Conn;");
    m_poolNew = (*env)->GetStaticMethodID(env, c_http, "poolNew",
                                          "(I)Ldev/vyto/android/Http$Pool;");

    f_status  = (*env)->GetFieldID(env, c_resp, "status", "I");
    f_headers = (*env)->GetFieldID(env, c_resp, "headers", "Ljava/lang/String;");
    f_body    = (*env)->GetFieldID(env, c_resp, "body", "[B");

    m_read  = (*env)->GetMethodID(env, c_conn, "read", "([B)I");
    m_close = (*env)->GetMethodID(env, c_conn, "close", "()V");
    f_conn_status  = (*env)->GetFieldID(env, c_conn, "status", "I");
    f_conn_headers = (*env)->GetFieldID(env, c_conn, "headers", "Ljava/lang/String;");

    m_pool_add = (*env)->GetMethodID(
        env, c_pool, "add",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[BI)I");
    m_pool_next     = (*env)->GetMethodID(env, c_pool, "next", "(I)I");
    m_pool_take     = (*env)->GetMethodID(env, c_pool, "take",
                                          "(I)Ldev/vyto/android/Http$Resp;");
    m_pool_shutdown = (*env)->GetMethodID(env, c_pool, "shutdown", "()V");

    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        vta_logf("anet: Http method lookup failed — Java half out of sync");
        return;
    }
    ids_ready = 1;
}

/* Called from JNI_OnLoad, which is the only place FindClass can see app
 * classes — see the comment there. Everything else just reads the cache. */
void vta_http_bind(JNIEnv *env) { resolve_ids(env); }

#define ENV_OR(ret) \
    JNIEnv *env = vta_env(); \
    if (!env) return (ret); \
    resolve_ids(env); \
    if (!ids_ready) return (ret)

static jstring mkstr(JNIEnv *env, const char *s) {
    return (*env)->NewStringUTF(env, s ? s : "");
}

/* A jbyteArray holding body_len bytes of body, or NULL for a bodyless request.
 * NULL is meaningful on the Java side: it decides setDoOutput. */
static jbyteArray mkbody(JNIEnv *env, const char *body, int64_t body_len) {
    if (!body || body_len <= 0) return NULL;
    jbyteArray a = (*env)->NewByteArray(env, (jsize)body_len);
    if (!a) return NULL;
    (*env)->SetByteArrayRegion(env, a, 0, (jsize)body_len, (const jbyte *)body);
    return a;
}

/* Copy a Java String into a malloc'd C string. "" rather than NULL on failure,
 * so every reader downstream can skip a null check. */
static char *dupjstr(JNIEnv *env, jstring s) {
    if (!s) return strdup("");
    const char *u = (*env)->GetStringUTFChars(env, s, NULL);
    char *out = strdup(u ? u : "");
    if (u) (*env)->ReleaseStringUTFChars(env, s, u);
    return out ? out : strdup("");
}

/* Drain a Java Resp into C memory and release every Java object it held. */
static AResp *resp_from_java(JNIEnv *env, jobject jr) {
    AResp *r = (AResp *)calloc(1, sizeof *r);
    if (!r) return NULL;
    r->kind = AH_RESP;
    r->status = (*env)->GetIntField(env, jr, f_status);

    jstring jh = (jstring)(*env)->GetObjectField(env, jr, f_headers);
    r->hdr = dupjstr(env, jh);
    if (jh) (*env)->DeleteLocalRef(env, jh);

    jbyteArray jb = (jbyteArray)(*env)->GetObjectField(env, jr, f_body);
    if (jb) {
        jsize n = (*env)->GetArrayLength(env, jb);
        /* One extra NUL so Response.text() can hand the buffer straight to
         * str() without copying; blen still reports the true length. */
        r->body = (char *)malloc((size_t)n + 1);
        if (r->body) {
            (*env)->GetByteArrayRegion(env, jb, 0, n, (jbyte *)r->body);
            r->body[n] = 0;
            r->blen = n;
        }
        (*env)->DeleteLocalRef(env, jb);
    }
    if (!r->body) {
        r->body = (char *)calloc(1, 1);
        r->blen = 0;
    }
    return r;
}

/* -------------------------------------------------------------- blocking */

void *vta_http_perform(const char *method, const char *url, const char *header_lines,
                       const char *body, int64_t body_len, int64_t timeout_ms) {
    ENV_OR(NULL);
    jstring jm = mkstr(env, method);
    jstring ju = mkstr(env, url);
    jstring jh = mkstr(env, header_lines);
    jbyteArray jb = mkbody(env, body, body_len);

    jobject jr = (*env)->CallStaticObjectMethod(env, c_http, m_perform, jm, ju, jh, jb,
                                                (jint)timeout_ms);
    (*env)->DeleteLocalRef(env, jm);
    (*env)->DeleteLocalRef(env, ju);
    (*env)->DeleteLocalRef(env, jh);
    if (jb) (*env)->DeleteLocalRef(env, jb);

    if ((*env)->ExceptionCheck(env)) {
        /* Http.perform catches everything it can, so an exception here is a
         * VM-level failure (OOM), not a transport error. */
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        if (jr) (*env)->DeleteLocalRef(env, jr);
        return NULL;
    }
    if (!jr) return NULL;

    AResp *r = resp_from_java(env, jr);
    (*env)->DeleteLocalRef(env, jr);
    return r;
}

int64_t vta_http_status(void *h) {
    AResp *r = (AResp *)h;
    if (!r || r->kind != AH_RESP) return 0;
    return r->status;
}

const char *vta_http_body_data(void *h) {
    AResp *r = (AResp *)h;
    if (!r || r->kind != AH_RESP || !r->body) return "";
    return r->body;
}

int64_t vta_http_body_len(void *h) {
    AResp *r = (AResp *)h;
    if (!r || r->kind != AH_RESP) return 0;
    return r->blen;
}

int64_t vta_http_body_copy(void *h, char *out, int64_t cap) {
    AResp *r = (AResp *)h;
    if (!r || r->kind != AH_RESP || !out || cap <= 0) return 0;
    int64_t n = r->blen < cap ? r->blen : cap;
    if (n > 0) memcpy(out, r->body, (size_t)n);
    return n;
}

const char *vta_http_headers(void *h) {
    AResp *r = (AResp *)h;
    if (!r || r->kind != AH_RESP || !r->hdr) return "";
    return r->hdr;
}

/* ------------------------------------------------------------- streaming */

void *vta_http_open(const char *method, const char *url, const char *header_lines,
                    const char *body, int64_t body_len, int64_t timeout_ms) {
    ENV_OR(NULL);
    jstring jm = mkstr(env, method);
    jstring ju = mkstr(env, url);
    jstring jh = mkstr(env, header_lines);
    jbyteArray jb = mkbody(env, body, body_len);

    jobject jc = (*env)->CallStaticObjectMethod(env, c_http, m_open, jm, ju, jh, jb,
                                                (jint)timeout_ms);
    (*env)->DeleteLocalRef(env, jm);
    (*env)->DeleteLocalRef(env, ju);
    (*env)->DeleteLocalRef(env, jh);
    if (jb) (*env)->DeleteLocalRef(env, jb);

    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        if (jc) (*env)->DeleteLocalRef(env, jc);
        return NULL;
    }
    if (!jc) return NULL;

    AConn *c = (AConn *)calloc(1, sizeof *c);
    if (!c) {
        (*env)->DeleteLocalRef(env, jc);
        return NULL;
    }
    c->kind = AH_CONN;
    c->conn = (*env)->NewGlobalRef(env, jc);
    (*env)->DeleteLocalRef(env, jc);
    return c;
}

int32_t vta_http_read(void *h, char *buf, int32_t cap) {
    AConn *c = (AConn *)h;
    if (!c || c->kind != AH_CONN || !buf || cap <= 0) return -1;
    ENV_OR(-1);

    /* One jbyteArray for the life of the connection. Allocating per read would
     * churn the Java heap for the length of a download. */
    if (!c->buf || c->bufcap < cap) {
        if (c->buf) {
            (*env)->DeleteGlobalRef(env, c->buf);
            c->buf = NULL;
        }
        jbyteArray a = (*env)->NewByteArray(env, (jsize)cap);
        if (!a) return -1;
        c->buf = (jbyteArray)(*env)->NewGlobalRef(env, a);
        (*env)->DeleteLocalRef(env, a);
        if (!c->buf) return -1;
        c->bufcap = cap;
    }

    jint n = (*env)->CallIntMethod(env, c->conn, m_read, c->buf);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return -1;
    }
    if (n <= 0) return -1;   /* EOF and error are the same to the caller */
    (*env)->GetByteArrayRegion(env, c->buf, 0, n, (jbyte *)buf);
    return (int32_t)n;
}

void vta_http_chunk_copy(const char *src, char *dst, int32_t len) {
    if (!src || !dst || len <= 0) return;
    memcpy(dst, src, (size_t)len);
}

/* --------------------------------------------------------------- freeing */

void vta_http_free(void *h) {
    if (!h) return;
    int kind = *(int *)h;
    if (kind == AH_RESP) {
        AResp *r = (AResp *)h;
        free(r->body);
        free(r->hdr);
        free(r);
        return;
    }
    if (kind != AH_CONN) return;

    AConn *c = (AConn *)h;
    JNIEnv *env = vta_env();
    /* ids_ready guards m_close: a handle can only exist if resolve_ids
     * succeeded once, but the VM can be torn down between open and free, and
     * CallVoidMethod with a NULL jmethodID is undefined rather than a no-op. */
    if (env && ids_ready) {
        if (c->conn) {
            (*env)->CallVoidMethod(env, c->conn, m_close);
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            (*env)->DeleteGlobalRef(env, c->conn);
        }
        if (c->buf) (*env)->DeleteGlobalRef(env, c->buf);
    }
    /* No env means the thread is detaching and the VM is tearing the refs down
     * anyway; the C struct still has to go. */
    free(c);
}

/* ------------------------------------------------------------------ pool */

void *vta_http_pool_new(int32_t max_parallel) {
    ENV_OR(NULL);
    jobject p = (*env)->CallStaticObjectMethod(env, c_http, m_poolNew, (jint)max_parallel);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        if (p) (*env)->DeleteLocalRef(env, p);
        return NULL;
    }
    if (!p) return NULL;
    jobject g = (*env)->NewGlobalRef(env, p);
    (*env)->DeleteLocalRef(env, p);
    return g;
}

int32_t vta_http_pool_add(void *p, const char *method, const char *url,
                          const char *header_lines, const char *body,
                          int64_t body_len, int64_t timeout_ms) {
    if (!p) return -1;
    ENV_OR(-1);
    jstring jm = mkstr(env, method);
    jstring ju = mkstr(env, url);
    jstring jh = mkstr(env, header_lines);
    jbyteArray jb = mkbody(env, body, body_len);

    jint id = (*env)->CallIntMethod(env, (jobject)p, m_pool_add, jm, ju, jh, jb,
                                    (jint)timeout_ms);
    (*env)->DeleteLocalRef(env, jm);
    (*env)->DeleteLocalRef(env, ju);
    (*env)->DeleteLocalRef(env, jh);
    if (jb) (*env)->DeleteLocalRef(env, jb);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return -1;
    }
    return (int32_t)id;
}

int32_t vta_http_pool_next(void *p, int32_t timeout_ms) {
    if (!p) return -1;
    ENV_OR(-1);
    jint id = (*env)->CallIntMethod(env, (jobject)p, m_pool_next, (jint)timeout_ms);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return -1;
    }
    return (int32_t)id;
}

void *vta_http_pool_take(void *p, int32_t id) {
    if (!p) return NULL;
    ENV_OR(NULL);
    jobject jr = (*env)->CallObjectMethod(env, (jobject)p, m_pool_take, (jint)id);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        if (jr) (*env)->DeleteLocalRef(env, jr);
        return NULL;
    }
    if (!jr) return NULL;
    AResp *r = resp_from_java(env, jr);
    (*env)->DeleteLocalRef(env, jr);
    return r;
}

void vta_http_pool_free(void *p) {
    if (!p) return;
    JNIEnv *env = vta_env();
    if (env && ids_ready) {
        (*env)->CallVoidMethod(env, (jobject)p, m_pool_shutdown);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        (*env)->DeleteGlobalRef(env, (jobject)p);
    }
}

#endif /* __ANDROID__ */
