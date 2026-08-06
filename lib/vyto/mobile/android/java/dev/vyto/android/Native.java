package dev.vyto.android;

import java.nio.ByteBuffer;

/**
 * The whole JNI boundary, in one file.
 *
 * <p>Java-to-native methods are declared here and implemented in the shim
 * ({@code lib/vyto/mobile/android/native/src/}). Native-to-Java calls do NOT
 * live here — the shim holds global refs to the {@link VytoView} and
 * {@link Actions} instances and calls their methods directly, so that the
 * return values (a measured text width, a launch result code) come back
 * without a second hop.
 *
 * <p>STATUS: written, never compiled. {@code vytoc build --shared} now
 * produces the {@code .so} and its {@code vyto_app_main()} entry, and the
 * shims backing every {@code native} method below are written — but nothing
 * has been built for {@code android-arm64}, which needs an NDK toolchain
 * passed via {@code --cc}. Until then these throw
 * {@code UnsatisfiedLinkError}. Design of record: {@code local/docs/ANDROID.md}.
 *
 * <h2>Threading</h2>
 *
 * Vyto owns its own loop on its own thread ({@code ui/core.vt:2413}), so the
 * two directions cross different threads:
 * <ul>
 *   <li>Everything declared here is called from the <b>UI thread</b> (input,
 *       lifecycle, results) and must not block — the shim queues and returns.
 *   <li>The shim calls back into Java from the <b>Vyto thread</b>, which has
 *       attached itself with {@code AttachCurrentThread}. Anything it touches
 *       must therefore be thread-safe or explicitly locked; see the bitmap
 *       lock in {@link VytoView}.
 * </ul>
 */
public final class Native {

    /** Set once {@link #load} has succeeded, so callers can degrade instead of crashing. */
    private static boolean loaded = false;

    private Native() {}

    /**
     * Load the app's Vyto library. The name is the {@code vytoc -o} stem, not a
     * fixed value, because one APK carries exactly one Vyto app.
     *
     * @return false when the library is missing or the ABI does not match, which
     *     is worth surfacing as a real error screen rather than a crash — an
     *     ABI mismatch is a packaging bug and the message says which ABI ran.
     */
    public static synchronized boolean load(String libName) {
        if (loaded) return true;
        try {
            System.loadLibrary(libName);
            loaded = true;
        } catch (UnsatisfiedLinkError e) {
            android.util.Log.e("Vyto", "loadLibrary(" + libName + ") failed on ABI "
                    + android.os.Build.SUPPORTED_ABIS[0], e);
            loaded = false;
        }
        return loaded;
    }

    public static boolean isLoaded() { return loaded; }

    // ---------------------------------------------------------------- lifecycle

    /**
     * Start the Vyto thread and run the app's {@code main}. Returns after the
     * thread is spawned, not after main finishes.
     *
     * <p>{@code density} is the value {@code vs_scale_pct} reports, so the
     * toolkit's existing scale path ({@code Theme.apply_scale}) works without a
     * new concept. Note it is sampled once by {@code Window.init} today and
     * never re-read — see the live-density gap in ANDROID.md Track C.
     */
    public static native void start(android.view.View view, int width, int height, float density);

    /**
     * Hand the intent shim the {@link Actions} instance to call up into.
     *
     * <p>Separate from {@link #start} because Actions is constructed alongside
     * the View and the ordering between them is the Activity's business.
     */
    public static native void bindActions(Actions actions);

    /** Ask the Vyto loop to exit and join its thread. Safe to call twice. */
    public static native void stop();

    /**
     * Park the Vyto loop. Animations stop waking it, so a backgrounded app
     * stops burning battery; the loop still exists and its widget tree is
     * untouched, because the Bitmap backbuffer survives independently of any
     * Android surface.
     */
    public static native void pause();

    public static native void resume();

    /**
     * New window size. Reallocates the native-side Bitmap and fires EV_RESIZE,
     * which re-runs layout through the existing {@code on_resize} path.
     */
    public static native void resize(int width, int height, float density);

    // -------------------------------------------------------------------- input

    /**
     * One pointer sample. {@code action} uses the {@code MotionEvent} constants
     * so no translation table is needed on either side.
     *
     * <p>Historical samples are delivered as separate calls with their own
     * timestamps, in order, before the current one. That is what makes
     * drag-scroll feel smooth rather than steppy, and it is the reason this
     * takes a time rather than reading a clock natively.
     */
    public static native void touch(int action, int pointerId, float x, float y, long timeMs);

    /**
     * One display refresh elapsed. Called from the UI thread by the
     * Choreographer callback {@link VytoView} runs while Vyto asks for it, and
     * it is what {@code Window.run()} presents on — not the 16ms software timer
     * that drives {@code tick()}.
     *
     * <p>Separating the two is the point: ticking off-clock is harmless because
     * animations advance by real elapsed time, but *presenting* off-clock beats
     * against the display and shows up as judder. At most one vsync is queued
     * natively, so a slow frame coalesces rather than backing up.
     */
    public static native void vsync();

    /**
     * A key event. {@code text} is the committed UTF-8 for an insertable key and
     * "" otherwise, matching the {@code key=0 + text} channel the X11 XIM path
     * already feeds into {@code TextField} ({@code vsurf.c:2006-2012}).
     *
     * <p>Soft-keyboard commits arrive through {@link VytoInputConnection} rather
     * than here, and can be arbitrarily long — the native side must not reuse
     * the 32-byte {@code last_text} buffer for them.
     */
    public static native void key(int keyCode, String text, int mods, boolean down);

    /**
     * System back. Returns true when Vyto consumed it (closed an overlay, popped
     * a screen) and false to let the Activity finish.
     *
     * <p>Answered without blocking: {@code AndroidWindow.nav_changed} publishes
     * how many screens and overlays are poppable (aback.c) and this reads that
     * integer, because the call runs on the UI thread and the Vyto thread is
     * usually parked in {@code surf.wait()}. A true answer also pushes an
     * Escape key event, which both wakes that loop and tells it what happened.
     *
     * <p>An app that never pushes a screen keeps the old behaviour exactly:
     * depth stays 0, this returns false, and back exits.
     */
    public static native boolean back();

    /** Window insets in px, plus the IME height, for safe-area-aware layout. */
    public static native void insets(int left, int top, int right, int bottom, int imeHeight);

    // ------------------------------------------------------------------ actions

    /**
     * Deliver a completed intent or permission result. Queued natively and
     * drained by {@code Actions.pump()} on the Vyto thread — never dispatched
     * to Vyto code from here, because this runs on the UI thread.
     */
    public static native void actionResult(int requestId, boolean ok, String[] uris);
}
