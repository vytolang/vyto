/* ahaptics.c — the vibrator.
 *
 * One up-call on the Actions instance the Activity already bound, so this file
 * needs no class of its own, no FindClass and no binding step: GetObjectClass
 * of an object Java handed us resolves without a class loader, which is what
 * keeps it clear of the trap that killed Intl and Http on the first device run.
 *
 * Called from the Vyto thread. Vibrator is thread-safe and this is deliberately
 * NOT posted to the UI thread: a buzz that lands a frame after the gesture that
 * caused it reads as a glitch rather than as feedback.
 *
 * Same both-arms rule as aime.c and ainsets.c — vytoc compiles every
 * native/src/*.c for every target, and this package has to keep linking on the
 * desktop for the widget goldens. A desktop has nothing to buzz, so the
 * non-Android arm does nothing, which is the honest answer rather than a stub.
 */

#include <stdint.h>

#ifdef __ANDROID__

#include "vyto_android.h"

void vta_haptic(int32_t ms, int32_t kind) {
    JNIEnv *env = vta_env();
    jobject acts = vta_actions();
    if (!env || !acts) return;
    jclass cls = (*env)->GetObjectClass(env, acts);
    if (!cls) return;
    jmethodID m = (*env)->GetMethodID(env, cls, "vibrate", "(II)V");
    if (m) (*env)->CallVoidMethod(env, acts, m, (jint)ms, (jint)kind);
    (*env)->DeleteLocalRef(env, cls);
    /* A vibrator an OEM has crippled can throw; a failed buzz is not worth
     * propagating anywhere, but an un-cleared exception would poison the next
     * JNI call the caller makes. */
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}

#else

void vta_haptic(int32_t ms, int32_t kind) { (void)ms; (void)kind; }

#endif
