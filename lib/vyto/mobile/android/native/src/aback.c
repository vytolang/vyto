/* aback.c — how many things the system Back key could pop, as one integer.
 *
 * Native.back() has to answer the UI thread synchronously: "did the app
 * consume that press?" Only the Vyto thread knows, and it is off running the
 * event loop. Rather than a round trip with a UI thread blocked on the other
 * end of it, Vyto *publishes* the answer in advance — AndroidWindow.nav_changed
 * writes the depth whenever a screen is pushed or popped or an overlay opens or
 * closes, and Native_back reads it.
 *
 * Deliberately not atomic. It is a single aligned int; a torn read is not a
 * thing on any target Vyto builds for, and correctness needs only eventual
 * visibility, not ordering — nothing else is published alongside it. The one
 * real race is Back pressed within a frame of the tap that changed the depth,
 * and its worst case is one press that does nothing: AndroidWindow sets
 * close_on_esc = false precisely so that a stale-high depth synthesises an
 * Escape which falls quietly on the floor rather than quitting the app.
 *
 * Like ainsets.c, and for the same reason, this is not wrapped in
 * #ifdef __ANDROID__: ui.vt declares these and vyto/mobile/android/ui compiles
 * on the desktop too, for the widget goldens in tests/ui/*_mobile_*.vt. There
 * is nothing platform-specific to guard anyway — it is one int.
 */

#include <stdint.h>

static volatile int g_back_depth = 0;

void vta_set_back_depth(int32_t n) {
    g_back_depth = (n < 0) ? 0 : (int)n;
}

int32_t vta_back_depth(void) {
    return (int32_t)g_back_depth;
}
