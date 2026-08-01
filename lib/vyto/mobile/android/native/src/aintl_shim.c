/* aintl_shim.c — internationalization over android.icu, for
 * vyto/mobile/android/intl.
 *
 * Vyto half: vyto/mobile/android/intl. Java half: dev.vyto.android.Intl.
 *
 * A translator, like anet_shim.c: every formatting decision belongs to ICU on
 * the Java side, and all this file does is marshal strings and own the handle
 * that Vyto's `deinit` releases.
 *
 * STATUS: written, never compiled — no NDK toolchain in this clone. The body
 * is wrapped in #ifdef __ANDROID__, so it is empty on every other target.
 * Design of record: local/docs/ANDROID.md.
 *
 * -- why android.icu and not libicuuc -------------------------------------------
 *
 * The device has ICU, and its C symbols are private and version-suffixed
 * (u_strFoldCase_66). The NDK ships no headers for them and the suffix moves
 * with the platform release, so a binary that links them today fails to load
 * on the next Android version. android.icu.* (API 24+) is the supported door
 * to the same tables. See net.vt / intl.vt for the same trade on the network.
 *
 * -- handles ---------------------------------------------------------------------
 *
 * Every open() returns a JNI global ref to a Java object, cast to void* for
 * Vyto. There is no C-side struct: the Java object *is* the handle, and close()
 * is DeleteGlobalRef. The one exception is the break iterator, which also
 * carries the string it is walking — that lives on the Java side too.
 *
 * -- the out/cap protocol --------------------------------------------------------
 *
 * Every formatting entry point writes UTF-8 into `out` and returns the number
 * of bytes the result *wants*, which is what lets the caller retry exactly once
 * at the right size. A result that fits is NUL-terminated; a truncated one
 * returns >= cap and the buffer contents are not to be used.
 */
/* vytoc globs native/src/*.c flat and compiles every file for every
 * target, with no per-file platform filter, so an Android-only source
 * must exclude its own body. Same pattern as vsurf_android.c. */
#ifdef __ANDROID__

#include "vyto_android.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ method cache */

static jclass c_intl = NULL;   /* global ref: dev/vyto/android/Intl */
static jmethodID m_defaultLocale = NULL;
static jmethodID m_numOpen = NULL, m_numFmtDouble = NULL, m_numFmtLong = NULL,
                 m_numFmtCurrency = NULL, m_numSetFraction = NULL;
static jmethodID m_datOpen = NULL, m_datFmt = NULL;
static jmethodID m_normalize = NULL, m_caseMap = NULL;
static jmethodID m_brkOpen = NULL, m_brkNext = NULL;
static jmethodID m_colOpen = NULL, m_colCompare = NULL, m_colSortKey = NULL;
static jmethodID m_pluralOpen = NULL, m_pluralSelect = NULL;
static int ids_ready = 0;

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
    c_intl = global_class(env, "dev/vyto/android/Intl");
    if (!c_intl) {
        vta_logf("aintl: dev.vyto.android.Intl not found — is the Java half in the apk?");
        return;
    }

    m_defaultLocale = (*env)->GetStaticMethodID(env, c_intl, "defaultLocale",
                                                "()Ljava/lang/String;");

    m_numOpen = (*env)->GetStaticMethodID(env, c_intl, "numOpen",
                                          "(Ljava/lang/String;I)Ljava/lang/Object;");
    m_numFmtDouble = (*env)->GetStaticMethodID(env, c_intl, "numFmtDouble",
                                               "(Ljava/lang/Object;D)Ljava/lang/String;");
    m_numFmtLong = (*env)->GetStaticMethodID(env, c_intl, "numFmtLong",
                                             "(Ljava/lang/Object;J)Ljava/lang/String;");
    m_numFmtCurrency = (*env)->GetStaticMethodID(
        env, c_intl, "numFmtCurrency",
        "(Ljava/lang/Object;DLjava/lang/String;)Ljava/lang/String;");
    m_numSetFraction = (*env)->GetStaticMethodID(env, c_intl, "numSetFraction",
                                                 "(Ljava/lang/Object;II)V");

    m_datOpen = (*env)->GetStaticMethodID(
        env, c_intl, "datOpen",
        "(Ljava/lang/String;IILjava/lang/String;)Ljava/lang/Object;");
    m_datFmt = (*env)->GetStaticMethodID(env, c_intl, "datFmt",
                                         "(Ljava/lang/Object;J)Ljava/lang/String;");

    m_normalize = (*env)->GetStaticMethodID(env, c_intl, "normalize",
                                            "(Ljava/lang/String;I)Ljava/lang/String;");
    m_caseMap = (*env)->GetStaticMethodID(
        env, c_intl, "caseMap",
        "(Ljava/lang/String;Ljava/lang/String;I)Ljava/lang/String;");

    m_brkOpen = (*env)->GetStaticMethodID(
        env, c_intl, "brkOpen",
        "(ILjava/lang/String;Ljava/lang/String;)Ljava/lang/Object;");
    m_brkNext = (*env)->GetStaticMethodID(env, c_intl, "brkNext", "(Ljava/lang/Object;)I");

    m_colOpen = (*env)->GetStaticMethodID(env, c_intl, "colOpen",
                                          "(Ljava/lang/String;)Ljava/lang/Object;");
    m_colCompare = (*env)->GetStaticMethodID(
        env, c_intl, "colCompare",
        "(Ljava/lang/Object;Ljava/lang/String;Ljava/lang/String;)I");
    m_colSortKey = (*env)->GetStaticMethodID(env, c_intl, "colSortKey",
                                             "(Ljava/lang/Object;Ljava/lang/String;)[B");

    m_pluralOpen = (*env)->GetStaticMethodID(env, c_intl, "pluralOpen",
                                             "(Ljava/lang/String;I)Ljava/lang/Object;");
    m_pluralSelect = (*env)->GetStaticMethodID(env, c_intl, "pluralSelect",
                                               "(Ljava/lang/Object;D)Ljava/lang/String;");

    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        vta_logf("aintl: Intl method lookup failed — Java half out of sync");
        return;
    }
    ids_ready = 1;
}

/* Called from JNI_OnLoad, which is the only place FindClass can see app
 * classes — see the comment there. Everything else just reads the cache. */
void vta_intl_bind(JNIEnv *env) { resolve_ids(env); }

#define ENV_OR(ret) \
    JNIEnv *env = vta_env(); \
    if (!env) return (ret); \
    resolve_ids(env); \
    if (!ids_ready) return (ret)

static jstring mkstr(JNIEnv *env, const char *s) {
    return (*env)->NewStringUTF(env, s ? s : "");
}

/* Copy a Java String into out/cap as UTF-8 and return the byte length it
 * wanted. Deletes the local ref, so callers hand the jstring over and forget
 * it. Returns -1 on a null string (which is how the Java side reports a
 * failure it already logged). */
static int32_t emit(JNIEnv *env, jstring s, char *out, int32_t cap) {
    if (!s) return -1;
    const char *u = (*env)->GetStringUTFChars(env, s, NULL);
    if (!u) {
        (*env)->DeleteLocalRef(env, s);
        return -1;
    }
    size_t n = strlen(u);
    if (out && cap > 0 && n < (size_t)cap) {
        memcpy(out, u, n);
        out[n] = 0;
    }
    (*env)->ReleaseStringUTFChars(env, s, u);
    (*env)->DeleteLocalRef(env, s);
    return (int32_t)n;
}

/* Promote a returned Java object to a global ref and drop the local one. */
static void *keep(JNIEnv *env, jobject local) {
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        if (local) (*env)->DeleteLocalRef(env, local);
        return NULL;
    }
    if (!local) return NULL;
    jobject g = (*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    return g;
}

/* Shared close for every handle shape: they are all just global refs. */
static void drop(void *h) {
    if (!h) return;
    JNIEnv *env = vta_env();
    if (env) (*env)->DeleteGlobalRef(env, (jobject)h);
}

/* ---------------------------------------------------------------- locale */

int32_t vta_intl_default_locale(char *out, int32_t cap) {
    ENV_OR(-1);
    jstring s = (jstring)(*env)->CallStaticObjectMethod(env, c_intl, m_defaultLocale);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        if (s) (*env)->DeleteLocalRef(env, s);
        return -1;
    }
    return emit(env, s, out, cap);
}

/* --------------------------------------------------------------- numbers */

void *vta_intl_num_open(const char *locale, int32_t style) {
    ENV_OR(NULL);
    jstring l = mkstr(env, locale);
    jobject o = (*env)->CallStaticObjectMethod(env, c_intl, m_numOpen, l, (jint)style);
    (*env)->DeleteLocalRef(env, l);
    return keep(env, o);
}

void vta_intl_num_close(void *h) { drop(h); }

int32_t vta_intl_num_fmt_double(void *h, double v, char *out, int32_t cap) {
    if (!h) return -1;
    ENV_OR(-1);
    jstring s = (jstring)(*env)->CallStaticObjectMethod(env, c_intl, m_numFmtDouble,
                                                        (jobject)h, (jdouble)v);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return -1; }
    return emit(env, s, out, cap);
}

int32_t vta_intl_num_fmt_int(void *h, int64_t v, char *out, int32_t cap) {
    if (!h) return -1;
    ENV_OR(-1);
    jstring s = (jstring)(*env)->CallStaticObjectMethod(env, c_intl, m_numFmtLong,
                                                        (jobject)h, (jlong)v);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return -1; }
    return emit(env, s, out, cap);
}

int32_t vta_intl_num_fmt_currency(void *h, double v, const char *iso3,
                                  char *out, int32_t cap) {
    if (!h) return -1;
    ENV_OR(-1);
    jstring c = mkstr(env, iso3);
    jstring s = (jstring)(*env)->CallStaticObjectMethod(env, c_intl, m_numFmtCurrency,
                                                        (jobject)h, (jdouble)v, c);
    (*env)->DeleteLocalRef(env, c);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return -1; }
    return emit(env, s, out, cap);
}

void vta_intl_num_set_fraction(void *h, int32_t minFrac, int32_t maxFrac) {
    if (!h) return;
    JNIEnv *env = vta_env();
    if (!env) return;
    resolve_ids(env);
    if (!ids_ready) return;
    (*env)->CallStaticVoidMethod(env, c_intl, m_numSetFraction, (jobject)h,
                                 (jint)minFrac, (jint)maxFrac);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}

/* ----------------------------------------------------------------- dates */

void *vta_intl_dat_open(const char *locale, int32_t dateStyle, int32_t timeStyle,
                        const char *tz) {
    ENV_OR(NULL);
    jstring l = mkstr(env, locale);
    jstring z = mkstr(env, tz);
    jobject o = (*env)->CallStaticObjectMethod(env, c_intl, m_datOpen, l,
                                               (jint)dateStyle, (jint)timeStyle, z);
    (*env)->DeleteLocalRef(env, l);
    (*env)->DeleteLocalRef(env, z);
    return keep(env, o);
}

void vta_intl_dat_close(void *h) { drop(h); }

int32_t vta_intl_dat_fmt(void *h, int64_t unix_ms, char *out, int32_t cap) {
    if (!h) return -1;
    ENV_OR(-1);
    jstring s = (jstring)(*env)->CallStaticObjectMethod(env, c_intl, m_datFmt,
                                                        (jobject)h, (jlong)unix_ms);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return -1; }
    return emit(env, s, out, cap);
}

/* ------------------------------------------------------ normalize / case */

int32_t vta_intl_normalize(const char *s, int32_t mode, char *out, int32_t cap) {
    ENV_OR(-1);
    jstring j = mkstr(env, s);
    jstring r = (jstring)(*env)->CallStaticObjectMethod(env, c_intl, m_normalize,
                                                        j, (jint)mode);
    (*env)->DeleteLocalRef(env, j);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return -1; }
    return emit(env, r, out, cap);
}

int32_t vta_intl_case(const char *s, const char *locale, int32_t op,
                      char *out, int32_t cap) {
    ENV_OR(-1);
    jstring j = mkstr(env, s);
    jstring l = mkstr(env, locale);
    jstring r = (jstring)(*env)->CallStaticObjectMethod(env, c_intl, m_caseMap,
                                                        j, l, (jint)op);
    (*env)->DeleteLocalRef(env, j);
    (*env)->DeleteLocalRef(env, l);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return -1; }
    return emit(env, r, out, cap);
}

/* ---------------------------------------------------------- segmentation */

void *vta_intl_brk_open(int32_t kind, const char *locale, const char *s) {
    ENV_OR(NULL);
    jstring l = mkstr(env, locale);
    jstring j = mkstr(env, s);
    jobject o = (*env)->CallStaticObjectMethod(env, c_intl, m_brkOpen,
                                               (jint)kind, l, j);
    (*env)->DeleteLocalRef(env, l);
    (*env)->DeleteLocalRef(env, j);
    return keep(env, o);
}

/* Next boundary as a *byte* offset into the original UTF-8, or -1 at the end.
 * The conversion from Java's UTF-16 index happens on the Java side, because
 * that is where both encodings of the string are in hand. */
int32_t vta_intl_brk_next(void *h) {
    if (!h) return -1;
    ENV_OR(-1);
    jint n = (*env)->CallStaticIntMethod(env, c_intl, m_brkNext, (jobject)h);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return -1; }
    return (int32_t)n;
}

void vta_intl_brk_close(void *h) { drop(h); }

/* -------------------------------------------------------------- collation */

void *vta_intl_col_open(const char *locale) {
    ENV_OR(NULL);
    jstring l = mkstr(env, locale);
    jobject o = (*env)->CallStaticObjectMethod(env, c_intl, m_colOpen, l);
    (*env)->DeleteLocalRef(env, l);
    return keep(env, o);
}

int32_t vta_intl_col_compare(void *h, const char *a, const char *b) {
    if (!h) return 0;
    ENV_OR(0);
    jstring ja = mkstr(env, a);
    jstring jb = mkstr(env, b);
    jint r = (*env)->CallStaticIntMethod(env, c_intl, m_colCompare, (jobject)h, ja, jb);
    (*env)->DeleteLocalRef(env, ja);
    (*env)->DeleteLocalRef(env, jb);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return 0; }
    return (int32_t)r;
}

int32_t vta_intl_col_sortkey(void *h, const char *s, char *out, int32_t cap) {
    if (!h) return -1;
    ENV_OR(-1);
    jstring j = mkstr(env, s);
    jbyteArray a = (jbyteArray)(*env)->CallStaticObjectMethod(env, c_intl, m_colSortKey,
                                                              (jobject)h, j);
    (*env)->DeleteLocalRef(env, j);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        if (a) (*env)->DeleteLocalRef(env, a);
        return -1;
    }
    if (!a) return -1;
    jsize n = (*env)->GetArrayLength(env, a);
    /* A sort key is binary, not text: it is copied by length and never
     * NUL-terminated, which is why this does not go through emit(). */
    if (out && cap > 0 && n <= cap) {
        (*env)->GetByteArrayRegion(env, a, 0, n, (jbyte *)out);
    }
    (*env)->DeleteLocalRef(env, a);
    return (int32_t)n;
}

void vta_intl_col_close(void *h) { drop(h); }

/* ---------------------------------------------------------------- plurals */

void *vta_intl_plural_open(const char *locale, int32_t kind) {
    ENV_OR(NULL);
    jstring l = mkstr(env, locale);
    jobject o = (*env)->CallStaticObjectMethod(env, c_intl, m_pluralOpen, l, (jint)kind);
    (*env)->DeleteLocalRef(env, l);
    return keep(env, o);
}

int32_t vta_intl_plural_select(void *h, double v, char *out, int32_t cap) {
    if (!h) return -1;
    ENV_OR(-1);
    jstring s = (jstring)(*env)->CallStaticObjectMethod(env, c_intl, m_pluralSelect,
                                                        (jobject)h, (jdouble)v);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return -1; }
    return emit(env, s, out, cap);
}

void vta_intl_plural_close(void *h) { drop(h); }

#endif /* __ANDROID__ */
