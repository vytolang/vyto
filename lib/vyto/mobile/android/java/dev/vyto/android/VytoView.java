package dev.vyto.android;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Rect;
import android.os.Build;
import android.os.Handler;
import android.text.InputType;
import android.view.Choreographer;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

import java.nio.ByteBuffer;

/**
 * A plain View the Vyto app can live inside. <b>No longer the default</b> —
 * see {@link VytoSurfaceView} and {@link VytoActivity#useSurfaceView()}.
 *
 * <p>Vyto renders into a persistent {@link Bitmap} on its own thread; this View
 * does nothing but blit that Bitmap in {@link #onDraw}. That split keeps Vyto's
 * loop intact and preserves the persistent backbuffer that
 * {@code Window.repaint()} depends on ({@code ui/core.vt:2111-2146} redraws
 * only the dirty rect and assumes the rest is still there).
 *
 * <h2>Known defect: the compositor can sample a half-drawn frame</h2>
 *
 * {@link #bitmapLock} does not protect the pixels, and cannot. {@code onDraw}
 * runs against a <b>hardware</b> canvas, where {@code drawBitmap} does not read
 * the bitmap — it records a draw into a display list. The RenderThread uploads
 * the pixels afterwards, once {@code onDraw} has returned and the lock has been
 * released. The Vyto thread is then free to begin the next replay, whose first
 * op fills the whole surface with the background colour, so the compositor can
 * upload a bitmap that has been cleared but not yet repainted.
 *
 * <p>On a device that presents as stutter and flicker under touch. It is
 * invisible to every obvious measurement — the command stream is complete and
 * does not oscillate, presents land on the display clock, and frame pacing is
 * clean — because it is a pixel-ownership bug, not a drawing or pacing one.
 *
 * <p>Fixing it here would mean never mutating a bitmap the compositor may still
 * be reading, i.e. alternating present buffers and a full-screen copy per
 * frame. {@link VytoSurfaceView} gets the same guarantee from a real swapchain
 * for free, which is why it took the default instead.
 *
 * <p>Kept because SurfaceView owns a separate compositor layer and an app
 * embedding Vyto in a larger View hierarchy may need this one anyway.
 * Design of record: {@code local/docs/ANDROID.md}.
 */
public class VytoView extends View {

    private final CommandBuffer commands = new CommandBuffer();

    /**
     * The persistent backbuffer. Guarded by {@link #bitmapLock} because the
     * Vyto thread writes it during replay while the UI thread reads it in
     * onDraw.
     *
     * <p>One bitmap rather than two: a naive A/B swap would hand Vyto a
     * frame-stale surface every other frame, breaking the partial-repaint
     * invariant. That reasoning is still right about A/B swapping — but it does
     * not follow that one bitmap is safe, and it was read that way for too
     * long. See the defect in this class's header: the lock does not cover the
     * RenderThread's upload, so the single bitmap is genuinely shared.
     *
     * <p>{@link VytoSurfaceView} resolves the same tension the other way: Vyto
     * keeps this exact persistent bitmap, and the *swapchain* takes the full
     * blit. Partial drawing stays valid because the swapchain was never the
     * backbuffer.
     */
    private Bitmap bitmap;
    private Canvas bitmapCanvas;
    private final Object bitmapLock = new Object();

    /** Reused so onDraw allocates nothing. */
    private final Rect srcRect = new Rect();
    private final Rect dstRect = new Rect();

    private boolean wantsTextInput = false;

    public VytoView(Context ctx) {
        super(ctx);
        setFocusable(true);
        setFocusableInTouchMode(true);
        // We draw a single bitmap; the View itself needs no layer.
        setWillNotDraw(false);
    }

    CommandBuffer commands() { return commands; }

    // ------------------------------------------------------------ backbuffer

    @Override
    protected void onSizeChanged(int w, int h, int oldW, int oldH) {
        super.onSizeChanged(w, h, oldW, oldH);
        if (w <= 0 || h <= 0) return;
        synchronized (bitmapLock) {
            if (bitmap != null) bitmap.recycle();
            // ARGB_8888 matches Vyto's 0xAARRGGBB packing directly.
            bitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
            // Always fully opaque — Vyto fills the background every frame. Lets
            // the composite skip per-pixel blending. See VytoSurfaceView.
            bitmap.setHasAlpha(false);
            bitmapCanvas = new Canvas(bitmap);
        }
        float density = getResources().getDisplayMetrics().density;
        if (Native.isLoaded()) Native.resize(w, h, density);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        synchronized (bitmapLock) {
            if (bitmap == null) return;
            canvas.drawBitmap(bitmap, 0f, 0f, null);
        }
    }

    /**
     * Replay one frame and schedule a composite. Called from the <b>Vyto
     * thread</b> by the native shim — the single JNI crossing per frame.
     *
     * @param dirty the changed region; the whole frame when Vyto did a full
     *     redraw. Passed to {@code postInvalidate} so Android's own damage
     *     tracking only composites what moved.
     */
    public void replayAndPost(ByteBuffer buf, int len,
                              int dx, int dy, int dw, int dh) {
        synchronized (bitmapLock) {
            if (bitmapCanvas == null) return;
            commands.replay(bitmapCanvas, buf, len);
        }
        // postInvalidate, never invalidate: this is not the UI thread.
        postInvalidate(dx, dy, dx + dw, dy + dh);
    }

    // ----------------------------------------------------------------- vsync

    /**
     * Whether Vyto currently wants a frame clock. Written on the UI thread only
     * (setVsyncEnabled hops there first), so it needs no synchronization.
     */
    private boolean vsyncWanted = false;

    /** Set between onPause and onResume; suppresses the clock without losing what Vyto asked for. */
    private boolean vsyncPaused = false;

    /** Whether a frame callback is currently outstanding. */
    private boolean vsyncPosted = false;

    private final Choreographer.FrameCallback frameCallback = new Choreographer.FrameCallback() {
        @Override
        public void doFrame(long frameTimeNanos) {
            vsyncPosted = false;
            // Re-post before signalling, so the clock keeps its cadence even if
            // the Vyto thread takes the whole frame to respond.
            syncVsync();
            Native.vsync();
        }
    };

    /**
     * Start or stop the display frame clock. <b>Called from the Vyto thread</b>
     * via {@code vs_set_vsync}, so it hops to the UI thread first —
     * {@code Choreographer.getInstance()} is per-Looper and the Vyto thread has
     * none.
     *
     * @return true when the clock was actually (un)scheduled. False means this
     *     View is not attached and there is no Handler to hop with, which the
     *     native side reports back to {@code Window.run()} so it presents
     *     eagerly instead of waiting for a vsync that will never arrive.
     */
    public boolean setVsyncEnabled(final boolean on) {
        Handler h = getHandler();
        if (h == null) return false;
        return h.post(new Runnable() {
            @Override
            public void run() {
                vsyncWanted = on;
                syncVsync();
            }
        });
    }

    @Override
    protected void onDetachedFromWindow() {
        // A frame callback outlives the View it was posted from, so a detached
        // View would keep waking the UI thread at 60Hz for nothing. vsyncWanted
        // is left alone: re-attaching re-posts from onAttachedToWindow without
        // Vyto having to ask again.
        vsyncPosted = false;
        Choreographer.getInstance().removeFrameCallback(frameCallback);
        super.onDetachedFromWindow();
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        syncVsync();
    }

    /** UI thread. Called by the Activity around onPause/onResume. */
    public void setVsyncPaused(boolean paused) {
        vsyncPaused = paused;
        syncVsync();
    }

    /** UI thread only. Idempotent: brings the posted state in line with the wanted one. */
    private void syncVsync() {
        boolean want = vsyncWanted && !vsyncPaused;
        if (want && !vsyncPosted) {
            Choreographer.getInstance().postFrameCallback(frameCallback);
            vsyncPosted = true;
        } else if (!want && vsyncPosted) {
            Choreographer.getInstance().removeFrameCallback(frameCallback);
            vsyncPosted = false;
        }
    }

    // ----------------------------------------------------------------- input

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (!Native.isLoaded()) return false;

        int action = e.getActionMasked();
        int index = e.getActionIndex();
        int id = e.getPointerId(index);

        // Historical samples first, in order. MotionEvent batches moves between
        // frames, and replaying them is what makes drag-scroll smooth instead of
        // stepping once per frame.
        if (action == MotionEvent.ACTION_MOVE) {
            int hist = e.getHistorySize();
            for (int h = 0; h < hist; h++) {
                for (int p = 0; p < e.getPointerCount(); p++) {
                    Native.touch(action, e.getPointerId(p),
                            e.getHistoricalX(p, h), e.getHistoricalY(p, h),
                            e.getHistoricalEventTime(h));
                }
            }
            for (int p = 0; p < e.getPointerCount(); p++) {
                Native.touch(action, e.getPointerId(p),
                        e.getX(p), e.getY(p), e.getEventTime());
            }
            return true;
        }

        Native.touch(action, id, e.getX(index), e.getY(index), e.getEventTime());
        return true;
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent e) {
        if (!Native.isLoaded()) return false;
        // Back is routed by the Activity's dispatcher, not here.
        if (keyCode == KeyEvent.KEYCODE_BACK) return super.onKeyDown(keyCode, e);
        int unicode = e.getUnicodeChar(e.getMetaState());
        String text = unicode > 0 ? new String(Character.toChars(unicode)) : "";
        Native.key(keyCode, text, modsOf(e), true);
        return true;
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent e) {
        if (!Native.isLoaded()) return false;
        if (keyCode == KeyEvent.KEYCODE_BACK) return super.onKeyUp(keyCode, e);
        Native.key(keyCode, "", modsOf(e), false);
        return true;
    }

    private static int modsOf(KeyEvent e) {
        // Matches VS_MOD_* in surface/native/src/vsurf.h:74-79.
        int m = 0;
        if (e.isShiftPressed()) m |= 1;
        if (e.isCtrlPressed())  m |= 2;
        if (e.isAltPressed())   m |= 4;
        if (e.isMetaPressed())  m |= 8;
        return m;
    }

    // ------------------------------------------------------------------- IME

    /**
     * Called by the native side when focus moves to or away from a text widget.
     *
     * <p>Vyto has no {@code wants_text_input()} predicate yet — {@code focusable}
     * is true for Button, Dropdown and ListBox as well as TextField
     * (ANDROID.md Track B), so the native side must decide and tell us rather
     * than us inferring it from focus alone.
     */
    public void setTextInputActive(final boolean on) {
        post(new Runnable() {
            @Override public void run() {
                wantsTextInput = on;
                InputMethodManager imm = (InputMethodManager)
                        getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
                if (imm == null) return;
                if (on) {
                    requestFocus();
                    // Restart so the IME re-reads our EditorInfo.
                    imm.restartInput(VytoView.this);
                    imm.showSoftInput(VytoView.this, InputMethodManager.SHOW_IMPLICIT);
                } else {
                    imm.hideSoftInputFromWindow(getWindowToken(), 0);
                }
            }
        });
    }

    @Override
    public boolean onCheckIsTextEditor() { return wantsTextInput; }

    @Override
    public InputConnection onCreateInputConnection(EditorInfo out) {
        if (!wantsTextInput) return null;
        out.inputType = InputType.TYPE_CLASS_TEXT;
        out.imeOptions = EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_FULLSCREEN;
        out.initialSelStart = -1;
        out.initialSelEnd = -1;
        return new VytoInputConnection(this);
    }

    // ---------------------------------------------------------------- insets

    @Override
    public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        if (!Native.isLoaded()) return super.onApplyWindowInsets(insets);
        int l, t, r, b, ime = 0;
        if (Build.VERSION.SDK_INT >= 30) {
            android.graphics.Insets bars =
                    insets.getInsets(WindowInsets.Type.systemBars());
            android.graphics.Insets imeIns =
                    insets.getInsets(WindowInsets.Type.ime());
            l = bars.left; t = bars.top; r = bars.right; b = bars.bottom;
            ime = imeIns.bottom;
        } else {
            l = insets.getSystemWindowInsetLeft();
            t = insets.getSystemWindowInsetTop();
            r = insets.getSystemWindowInsetRight();
            b = insets.getSystemWindowInsetBottom();
        }
        Native.insets(l, t, r, b, ime);
        return super.onApplyWindowInsets(insets);
    }

    void releaseBitmap() {
        synchronized (bitmapLock) {
            if (bitmap != null) { bitmap.recycle(); bitmap = null; }
            bitmapCanvas = null;
        }
    }

    /**
     * Commit-only IME bridge.
     *
     * <p>Nested rather than its own file because it is the View's input
     * connection and has no life apart from it.
     *
     * <p>Committed text goes down the same {@code key=0 + UTF-8} channel the
     * X11 XIM path already feeds into {@code TextField}
     * ({@code vsurf.c:2006-2012}), which is why this half is small: the
     * receiving side already exists and is exercised on desktop.
     *
     * <p><b>Not implemented: composing text.</b> {@code setComposingText} is
     * committed immediately instead of being tracked as an underlined preedit
     * range, because {@code TextField} has nowhere to put one. That is the
     * documented degraded mode (ANDROID.md Track B) — autocorrect and
     * word-replacement will feel blunt until {@code TextField} grows a
     * composing range. Everything else (insert, backspace, enter) is correct.
     */
    static final class VytoInputConnection
            extends android.view.inputmethod.BaseInputConnection {

        // Typed as View, not VytoView: BaseInputConnection only needs something
        // to attach to, and VytoSurfaceView shares this connection verbatim.
        private final View view;

        VytoInputConnection(View v) {
            super(v, false);   // false: no full editable, we forward events
            this.view = v;
        }

        @Override
        public boolean commitText(CharSequence text, int newCursorPosition) {
            if (text != null && text.length() > 0 && Native.isLoaded()) {
                // keyCode 0 means "no key, just text" — exactly what the
                // multibyte XIM path emits. Length is unbounded here, which is
                // why the native side must not reuse the 32-byte last_text
                // buffer (vsurf.c:60) for IME commits.
                Native.key(0, text.toString(), 0, true);
            }
            return true;
        }

        @Override
        public boolean setComposingText(CharSequence text, int newCursorPosition) {
            return commitText(text, newCursorPosition);
        }

        @Override
        public boolean deleteSurroundingText(int beforeLength, int afterLength) {
            if (!Native.isLoaded()) return true;
            // The soft keyboard's backspace arrives here, not as a keycode.
            for (int i = 0; i < beforeLength; i++) {
                Native.key(KeyEvent.KEYCODE_DEL, "", 0, true);
                Native.key(KeyEvent.KEYCODE_DEL, "", 0, false);
            }
            return true;
        }

        @Override
        public boolean sendKeyEvent(KeyEvent event) {
            if (!Native.isLoaded()) return true;
            boolean down = event.getAction() == KeyEvent.ACTION_DOWN;
            int unicode = event.getUnicodeChar(event.getMetaState());
            String text = (down && unicode > 0)
                    ? new String(Character.toChars(unicode)) : "";
            Native.key(event.getKeyCode(), text, modsOf(event), down);
            return true;
        }

        @Override
        public boolean performEditorAction(int actionCode) {
            if (Native.isLoaded()) {
                Native.key(KeyEvent.KEYCODE_ENTER, "", 0, true);
                Native.key(KeyEvent.KEYCODE_ENTER, "", 0, false);
            }
            return true;
        }
    }
}
