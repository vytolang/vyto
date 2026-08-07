/* jni_boot.c — the JNI entry points and the Vyto thread.
 *
 * Everything Java calls down into lands here. The shims that call *up* into
 * Java (apainter_shim.c, aintent_shim.c) get their JNIEnv and their global
 * refs from this file.
 *
 * STATUS: written, never compiled — there is no NDK toolchain in this clone.
 * vyto_app_main() now exists: `vytoc build --shared` emits it (src/emit.c,
 * ENTRY_SHARED). Design of record: local/docs/ANDROID.md.
 */
/* vytoc globs native/src/*.c flat and compiles every file for every
 * target, with no per-file platform filter, so an Android-only source
 * must exclude its own body. Same pattern as vsurf_android.c. */
#ifdef __ANDROID__

#include "vyto_android.h"

#include <android/log.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static JavaVM *g_vm = NULL;

static jobject g_view = NULL;      /* global ref, VytoView */
static jobject g_commands = NULL;  /* global ref, CommandBuffer */
static jobject g_actions = NULL;   /* global ref, Actions */

static pthread_t g_thread;
static int g_thread_live = 0;

/* ------------------------------------------------------------------- env */

JNIEnv *vta_env(void) {
    JNIEnv *env = NULL;
    if (!g_vm) return NULL;
    if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) == JNI_OK) return env;
    /* The Vyto thread was created by us and is not known to the VM yet. It
     * stays attached for its whole life; detaching per call would cost a
     * thread-local lookup and a barrier on every frame. */
    if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) {
        vta_logf("AttachCurrentThread failed");
        return NULL;
    }
    return env;
}

jobject vta_view(void)     { return g_view; }
jobject vta_commands(void) { return g_commands; }
jobject vta_actions(void)  { return g_actions; }

void vta_logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, "Vyto", fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------- thread */

static void *vyto_thread_main(void *arg) {
    (void)arg;
    /* Attach once for the thread's whole life. Everything the painter and the
     * intent queue do from here reuses this env. */
    JNIEnv *env = vta_env();
    if (!env) return NULL;

    vta_logf("vyto thread up");
    vyto_app_main();      /* returns when Window.run() exits */
    /* Nothing is left to pop. Without this a stale positive depth would make
     * Native_back claim every press for a loop that is no longer running, and
     * Back would be dead for as long as the Activity outlives the thread. */
    vta_set_back_depth(0);
    vta_logf("vyto thread down");

    if (g_vm) (*g_vm)->DetachCurrentThread(g_vm);
    return NULL;
}

/* ------------------------------------------------------------ OnLoad/Unload */

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    g_vm = vm;

    /* Resolve the shims' Java classes HERE and nowhere else.
     *
     * FindClass does not search a fixed namespace: it uses the class loader
     * associated with the *calling* frame. On a thread created with
     * pthread_create and attached with AttachCurrentThread there is no Java
     * frame, so it falls back to the system class loader — which cannot see
     * anything in the apk. Every dev/vyto/android/* lookup from the Vyto thread
     * therefore fails, with the class sitting in classes.dex the whole time.
     *
     * JNI_OnLoad is called from System.loadLibrary, so the app class loader is
     * on the stack and FindClass resolves normally. The shims cache what they
     * find in global refs, so every later call from any thread just reads the
     * cache.
     *
     * Found by running on a device: the app died 42ms after "vyto thread up"
     * with "Intl not found", and nothing in the build-time chain could have
     * caught it. The painter and intent shims never hit this because they take
     * GetObjectClass() of an object Java handed them, which needs no loader.
     */
    JNIEnv *env = NULL;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) == JNI_OK && env) {
        vta_intl_bind(env);
        vta_http_bind(env);
    } else {
        vta_logf("JNI_OnLoad: no JNIEnv — net and intl will be unavailable");
    }
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *vm, void *reserved) {
    (void)vm; (void)reserved;
    g_vm = NULL;
}

/* ------------------------------------------------------- Native.* entries */

JNIEXPORT void JNICALL
Java_dev_vyto_android_Native_start(JNIEnv *env, jclass cls,
                                   jobject view, jint w, jint h, jfloat density) {
    (void)cls;
    if (g_thread_live) return;   /* start is idempotent; onCreate can re-run */

    g_view = (*env)->NewGlobalRef(env, view);

    /* CommandBuffer comes from the view rather than being constructed here, so
     * there is exactly one and the Java side owns its lifetime. */
    jclass viewCls = (*env)->GetObjectClass(env, view);
    jmethodID mCommands = (*env)->GetMethodID(env, viewCls,
                                              "commands",
                                              "()Ldev/vyto/android/CommandBuffer;");
    if (mCommands) {
        jobject cb = (*env)->CallObjectMethod(env, view, mCommands);
        if (cb) {
            g_commands = (*env)->NewGlobalRef(env, cb);
            (*env)->DeleteLocalRef(env, cb);
        }
    }
    (*env)->DeleteLocalRef(env, viewCls);

    if (!g_commands) {
        vta_logf("start: no CommandBuffer — aborting");
        return;
    }

    vs_android_bind(w, h, density);

    g_thread_live = 1;
    if (pthread_create(&g_thread, NULL, vyto_thread_main, NULL) != 0) {
        vta_logf("pthread_create failed");
        g_thread_live = 0;
    }
}

/* Called by VytoActivity so the intent shim has an Actions instance to talk
 * to. Separate from start() because Actions is constructed alongside the view
 * and the ordering between them is the Activity's business, not ours. */
JNIEXPORT void JNICALL
Java_dev_vyto_android_Native_bindActions(JNIEnv *env, jclass cls, jobject actions) {
    (void)cls;
    if (g_actions) (*env)->DeleteGlobalRef(env, g_actions);
    g_actions = actions ? (*env)->NewGlobalRef(env, actions) : NULL;
}

/* Called by VytoActivity before start(), with Context.getFilesDir() and
 * getCacheDir(). Before start() is not a style choice: os_app_dir() caches on
 * first use, and the Vyto thread reaches for it as soon as it is running. */
JNIEXPORT void JNICALL
Java_dev_vyto_android_Native_setAppDirs(JNIEnv *env, jclass cls,
                                        jstring files, jstring cache) {
    (void)cls;
    const char *f = files ? (*env)->GetStringUTFChars(env, files, NULL) : NULL;
    const char *c = cache ? (*env)->GetStringUTFChars(env, cache, NULL) : NULL;
    vta_set_app_dirs(f, c);   /* copies both */
    if (f) (*env)->ReleaseStringUTFChars(env, files, f);
    if (c) (*env)->ReleaseStringUTFChars(env, cache, c);
}

JNIEXPORT void JNICALL
Java_dev_vyto_android_Native_stop(JNIEnv *env, jclass cls) {
    (void)cls;
    if (!g_thread_live) return;

    /* Ask the loop to exit, then wait for it. Joining matters: the Vyto thread
     * holds JNI refs and touches the Bitmap, and VytoActivity.onDestroy
     * recycles that Bitmap immediately after this returns. */
    vs_android_request_close();
    pthread_join(g_thread, NULL);
    g_thread_live = 0;

    vs_android_unbind();
    vta_intent_shutdown();

    if (g_commands) { (*env)->DeleteGlobalRef(env, g_commands); g_commands = NULL; }
    if (g_actions)  { (*env)->DeleteGlobalRef(env, g_actions);  g_actions  = NULL; }
    if (g_view)     { (*env)->DeleteGlobalRef(env, g_view);     g_view     = NULL; }
}

JNIEXPORT void JNICALL
Java_dev_vyto_android_Native_pause(JNIEnv *env, jclass cls) {
    (void)env; (void)cls;
    vs_android_set_paused(1);
}

JNIEXPORT void JNICALL
Java_dev_vyto_android_Native_resume(JNIEnv *env, jclass cls) {
    (void)env; (void)cls;
    vs_android_set_paused(0);
}

JNIEXPORT void JNICALL
Java_dev_vyto_android_Native_resize(JNIEnv *env, jclass cls,
                                    jint w, jint h, jfloat density) {
    (void)env; (void)cls;
    vs_android_push_resize(w, h, density);
}

JNIEXPORT void JNICALL
Java_dev_vyto_android_Native_vsync(JNIEnv *env, jclass cls) {
    (void)env; (void)cls;
    vs_android_push_vsync();
}

JNIEXPORT void JNICALL
Java_dev_vyto_android_Native_touch(JNIEnv *env, jclass cls, jint action,
                                   jint pointer_id, jfloat x, jfloat y, jlong time_ms) {
    (void)env; (void)cls;
    vs_android_push_touch(action, pointer_id, x, y, (int64_t)time_ms);
}

JNIEXPORT void JNICALL
Java_dev_vyto_android_Native_key(JNIEnv *env, jclass cls, jint keycode,
                                 jstring text, jint mods, jboolean down) {
    (void)cls;
    const char *utf8 = "";
    if (text) utf8 = (*env)->GetStringUTFChars(env, text, NULL);
    /* The queue copies; IME commits are unbounded in length, so nothing here
     * may reuse vsurf's 32-byte last_text buffer. */
    vs_android_push_key(keycode, utf8 ? utf8 : "", mods, down ? 1 : 0);
    if (text && utf8) (*env)->ReleaseStringUTFChars(env, text, utf8);
}

JNIEXPORT jboolean JNICALL
Java_dev_vyto_android_Native_back(JNIEnv *env, jclass cls) {
    (void)env; (void)cls;
    /* The question the UI thread is asking is one only the Vyto thread can
     * answer, and it must be answered now. So it is answered from a depth the
     * Vyto thread published in advance (aback.c) rather than by waking it and
     * waiting: AndroidWindow.nav_changed writes how many screens and overlays
     * are poppable, and this reads it. Zero means nothing to pop — return
     * false and let the Activity finish, which is the one path that ends the
     * app.
     *
     * The press itself travels as Escape rather than a key code of its own,
     * for three reasons. Window.on_key_ev already reads Escape as "dismiss the
     * open overlay"; AndroidWindow.on_back turns the no-overlay case into "pop
     * a screen"; and — the load-bearing one — the key push is also the
     * *wakeup*. Window.run() blocks in surf.wait() whenever nothing is
     * animating, which is exactly the state a Back press arrives in, so a bare
     * flag store would not be seen until some unrelated touch happened to
     * arrive. Pushing an event wakes the loop and carries the meaning at once.
     *
     * KEY_ESC is 1002 (vyto/surface/surface.vt) — a vsurf-level key code, not
     * an Android one, since that is what the queue speaks. */
    if (!g_thread_live || vta_back_depth() <= 0) return JNI_FALSE;
    vs_android_push_key(1002 /* KEY_ESC */, "", 0, 1);
    vs_android_push_key(1002 /* KEY_ESC */, "", 0, 0);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_dev_vyto_android_Native_insets(JNIEnv *env, jclass cls,
                                    jint l, jint t, jint r, jint b, jint ime_h) {
    (void)env; (void)cls;
    vs_android_push_insets(l, t, r, b, ime_h);
}

JNIEXPORT void JNICALL
Java_dev_vyto_android_Native_actionResult(JNIEnv *env, jclass cls,
                                          jint request_id, jboolean ok, jobjectArray uris) {
    (void)cls;
    int n = uris ? (int)(*env)->GetArrayLength(env, uris) : 0;
    char **copies = NULL;

    if (n > 0) {
        copies = (char **)calloc((size_t)n, sizeof(char *));
        if (!copies) n = 0;
    }
    for (int i = 0; i < n; i++) {
        jstring js = (jstring)(*env)->GetObjectArrayElement(env, uris, i);
        if (!js) { copies[i] = NULL; continue; }
        const char *s = (*env)->GetStringUTFChars(env, js, NULL);
        copies[i] = s ? strdup(s) : NULL;
        if (s) (*env)->ReleaseStringUTFChars(env, js, s);
        /* Explicit, because a multi-select can return dozens and the default
         * local-ref table is 512 entries. */
        (*env)->DeleteLocalRef(env, js);
    }

    /* Takes ownership of copies and every strdup in it. */
    vta_result_push(request_id, ok ? 1 : 0, copies, n);
}

#endif /* __ANDROID__ */
