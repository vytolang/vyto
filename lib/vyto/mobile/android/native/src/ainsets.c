/* ainsets.c — window insets, as five plain integer reads for Vyto.
 *
 * Unlike every other file in this directory, this one is NOT wrapped entirely
 * in #ifdef __ANDROID__, and that is deliberate: vyto/mobile/android/ui is a
 * leaf package that compiles and runs on the *desktop* too — the widget
 * goldens in tests/ui/*_mobile_*.vt exercise it under X11 and headless. A
 * declaration in ui.vt therefore has to resolve on every target Vyto builds
 * for, not just android-arm64.
 *
 * So both arms are defined. On Android these report the real system-bar and
 * IME insets that VytoView.onApplyWindowInsets pushed down; everywhere else a
 * window has no notch, no gesture bar and no soft keyboard, and zero is the
 * honest answer rather than a stub.
 *
 * Five separate calls rather than one out-param struct because Vyto's FFI
 * takes C-shaped scalars and returning five values would need a pointer dance
 * at every call site. Each call re-reads all five under the lock; that costs a
 * mutex per inset per layout, which is nothing next to a layout pass.
 */

#include <stdint.h>

#ifdef __ANDROID__

#include "vyto_android.h"

int32_t vta_inset_left(void) {
    int l = 0;
    vs_android_get_insets(&l, NULL, NULL, NULL, NULL);
    return (int32_t)l;
}

int32_t vta_inset_top(void) {
    int t = 0;
    vs_android_get_insets(NULL, &t, NULL, NULL, NULL);
    return (int32_t)t;
}

int32_t vta_inset_right(void) {
    int r = 0;
    vs_android_get_insets(NULL, NULL, &r, NULL, NULL);
    return (int32_t)r;
}

int32_t vta_inset_bottom(void) {
    int b = 0;
    vs_android_get_insets(NULL, NULL, NULL, &b, NULL);
    return (int32_t)b;
}

int32_t vta_inset_ime(void) {
    int ime = 0;
    vs_android_get_insets(NULL, NULL, NULL, NULL, &ime);
    return (int32_t)ime;
}

#else

int32_t vta_inset_left(void)   { return 0; }
int32_t vta_inset_top(void)    { return 0; }
int32_t vta_inset_right(void)  { return 0; }
int32_t vta_inset_bottom(void) { return 0; }
int32_t vta_inset_ime(void)    { return 0; }

#endif
