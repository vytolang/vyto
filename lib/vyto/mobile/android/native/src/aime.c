/* aime.c — the soft keyboard.
 * Android IME (Input Method Editor) 
 *
 * VytoView.setTextInputActive and VytoSurfaceView.setTextInputActive are
 * complete on the Java side — onCheckIsTextEditor, onCreateInputConnection,
 * showSoftInput, a shared VytoInputConnection — and until this file existed
 * nothing called them. There was no JNI entry and no path from Vyto, so
 * TextField, TextArea and SearchField were decorative on a device: focus one,
 * no keyboard, no input.
 *
 * The decision of *when* is not here. Widget.wants_text() says whether a
 * widget takes text, Window.focus_changed() fires when the focus moves, and
 * AndroidWindow.focus_changed turns the pair into the one call below. Only the
 * widget can know, and on a touch platform there is no other signal — nothing
 * else can decide a keyboard should exist.
 *
 * Same both-arms rule as ainsets.c: vytoc globs native/src/*.c flat and
 * compiles every file for every target, and vyto/mobile/android/ui has to keep
 * linking on the desktop for the widget goldens in tests/ui/*_mobile_*.vt. So
 * the non-Android arm is a real no-op rather than the file being #ifdef-ed
 * away — a desktop window has no soft keyboard, and doing nothing is the
 * honest answer.
 */

#include <stdint.h>

#ifdef __ANDROID__

#include "vyto_android.h"

/* Raise or hide the soft keyboard.
 *
 * GetObjectClass of the view Java handed us, exactly like the painter and
 * intent shims. That needs no class loader, so it cannot hit the FindClass
 * trap that killed Intl and Http on the first device run: FindClass resolves
 * against the *calling frame's* loader, and a pthread attached with
 * AttachCurrentThread has no frame.
 *
 * VytoView and VytoSurfaceView both declare setTextInputActive with this
 * signature, which is why the lookup is on the object's class rather than on
 * either named class. Resolved per call rather than cached because a call
 * happens on a focus change, not per frame.
 *
 * setTextInputActive posts to the UI thread itself, so this is safe to call
 * from the Vyto thread — which is where every caller is.
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

void vta_ime_set(int32_t on) { (void)on; }

#endif
