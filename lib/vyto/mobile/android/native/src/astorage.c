/* astorage.c — the app's private directories, and the IME switch.
 *
 * Two things an Android app cannot do without, both of which are one call
 * each and neither of which had a path from Vyto:
 *
 *   1. Storage. `appDir()` resolves through os_app_dir(), which prefers
 *      $VYTO_APP_DIR and otherwise falls back to the path vytoc baked at
 *      build time (-DVYTO_APPDIR = the entry file's directory). On a device
 *      that baked path is a Linux directory on the *build host* that does not
 *      exist, so nothing persisted: no settings, no cache, no offline data,
 *      no session token. Native.setAppDirs sets the environment variable
 *      before the Vyto thread starts, which corrects every consumer at once —
 *      vyto/io, vyto/asset, anything that ever opens a file.
 *
 *   2. The soft keyboard. VytoSurfaceView.setTextInputActive is complete and
 *      had no caller: no JNI entry, no path from Vyto. TextField, TextArea and
 *      SearchField were therefore decorative on a device — focus one, no
 *      keyboard, no input. This is the up-call that raises it.
 *
 * Same both-arms rule as ainsets.c: vytoc globs native/src/*.c flat and
 * compiles every file for every target, and vyto/mobile/android/ui has to keep
 * linking on the desktop for the widget goldens. So the desktop arm is real
 * (empty strings, no IME) rather than #ifdef-ed away.
 */

#include <stdint.h>
#include <string.h>

#ifdef __ANDROID__

#include "vyto_android.h"
#include <stdlib.h>

/* Owned copies: the jstrings they came from are released immediately, and
 * these outlive every caller. Set once per process, before the Vyto thread
 * exists, so no lock. */
static char *g_files_dir = NULL;
static char *g_cache_dir = NULL;

/* Set from Java (VytoActivity.onCreate) before Native.start. It must be before:
 * os_app_dir() caches its answer on the first call, so a later setenv would be
 * ignored by everything that had already asked. */
void vta_set_app_dirs(const char *files, const char *cache) {
    if (files && *files) {
        free(g_files_dir);
        g_files_dir = strdup(files);
        /* The one that matters — os_app_dir() prefers it over the baked path,
         * so appDir() and every consumer of it are corrected by this line. */
        setenv("VYTO_APP_DIR", files, 1);
    }
    if (cache && *cache) {
        free(g_cache_dir);
        g_cache_dir = strdup(cache);
    }
}

const char *vta_files_dir(void) { return g_files_dir ? g_files_dir : ""; }
const char *vta_cache_dir(void) { return g_cache_dir ? g_cache_dir : ""; }

/* Raise or hide the soft keyboard.
 *
 * GetObjectClass of the view Java handed us, exactly like the painter and
 * intent shims: that needs no class loader and so cannot hit the FindClass
 * trap that killed Intl and Http on the first device run. VytoView and
 * VytoSurfaceView both declare setTextInputActive with this signature, and the
 * method is resolved per call rather than cached because a call happens on a
 * focus change, not per frame.
 *
 * setTextInputActive posts to the UI thread itself, so this is safe from the
 * Vyto thread — which is where every caller is.
 */
void vta_ime_set(int32_t on) {
    JNIEnv *env = vta_env();
    jobject view = vta_view();
    if (!env || !view) return;
    jclass cls = (*env)->GetObjectClass(env, view);
    if (!cls) return;
    jmethodID m = (*env)->GetMethodID(env, cls, "setTextInputActive", "(Z)V");
    if (m) (*env)->CallVoidMethod(env, view, m, on ? JNI_TRUE : JNI_FALSE);
    (*env)->DeleteLocalRef(env, cls);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}

#else

const char *vta_files_dir(void) { return ""; }
const char *vta_cache_dir(void) { return ""; }
void vta_ime_set(int32_t on) { (void)on; }

#endif
