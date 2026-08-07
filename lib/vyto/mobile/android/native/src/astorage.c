/* astorage.c — the app's private directories.
 *
 * `appDir()` resolves through os_app_dir(), which prefers $VYTO_APP_DIR and
 * otherwise falls back to the path vytoc baked at build time (-DVYTO_APPDIR =
 * the entry file's directory). On a device that baked path is a directory on
 * the *build host* that does not exist, so nothing persisted: no settings, no
 * cache, no offline data, no session token.
 *
 * Native.setAppDirs sets the environment variable before the Vyto thread
 * starts, which corrects every consumer at once — vyto/io, vyto/asset,
 * anything that ever opens a file.
 *
 * Same both-arms rule as ainsets.c: vytoc globs native/src/*.c flat and
 * compiles every file for every target, and vyto/mobile/android/storage has to
 * keep linking on the desktop. So the non-Android arm is real (empty strings,
 * which callers fall back to appDir() for) rather than #ifdef-ed away.
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

#else

const char *vta_files_dir(void) { return ""; }
const char *vta_cache_dir(void) { return ""; }

#endif
