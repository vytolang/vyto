package dev.vyto.android;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffXfermode;
import android.os.Build;
import android.os.Handler;
import android.text.InputType;
import android.view.Choreographer;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowInsets;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

import java.nio.ByteBuffer;

/**
 * The default rendering target. {@link VytoView} is kept alongside, not
 * replaced; pick one at launch via {@link VytoActivity#useSurfaceView()}.
 *
 * <p><b>Confirmed on hardware.</b> Compared back to back against
 * {@link VytoView} on a Redmi Note 9S (Android 12), same build, same finger:
 * the stutter and flicker that had survived every other fix were gone here and
 * present there. That result is what overturned ANDROID.md's rejection of
 * SurfaceView, so read the rest of this comment before reverting it.
 *
 * <h2>Why this exists</h2>
 *
 * {@link VytoView} composites through a hardware canvas: {@code onDraw} records
 * a {@code drawBitmap} into a display list, and the <b>RenderThread</b> uploads
 * the pixels afterwards — after {@code onDraw} returned and after the bitmap
 * lock was released. The Vyto thread is free to start the next replay at that
 * moment, and the first thing a frame does is fill the whole surface with the
 * background colour. So the compositor can sample a bitmap that has been
 * cleared but not yet repainted, which presents as a blank frame.
 *
 * <p>That race is invisible to every measurement taken against it: the command
 * stream is complete and non-oscillating, and presents land on the display
 * clock. It is a pixel-ownership bug, not a pacing or a drawing bug.
 *
 * <p>Here the blit is <b>synchronous and on the Vyto thread</b>:
 * {@code lockCanvas} hands back a real swapchain buffer, {@code drawBitmap}
 * copies into it immediately, and {@code unlockCanvasAndPost} queues it. No
 * other thread reads our bitmap, ever, so there is nothing to race with. It
 * also drops the UI-thread hop — no {@code postInvalidate}, no waiting for a
 * traversal — which is the second half of the input-to-glass latency.
 *
 * <h2>Why this does not break partial repaint</h2>
 *
 * ANDROID.md rejected SurfaceView because {@code Window.repaint()} does partial
 * <i>drawing</i>, not just partial presenting ({@code ui/core.vt:2111-2146}):
 * it redraws only the dirty rect and assumes everything outside is still in the
 * backbuffer. A swapchain cannot promise that — {@code lockCanvas} returns a
 * buffer whose contents are undefined and typically two or three frames stale.
 *
 * <p>The resolution is that the swapchain is <b>not</b> the backbuffer. Vyto
 * keeps drawing into the same persistent {@link Bitmap} it always has, so the
 * partial-repaint invariant is untouched; the surface only ever receives a full
 * blit of that bitmap. The cost is one full-screen copy per frame even for a
 * one-pixel dirty rect — which is exactly what {@code Canvas.drawBitmap} was
 * already paying, since Android has no partial-texture-update path.
 *
 * <p>Consequently {@code lockCanvas(dirtyRect)} must <b>not</b> be used here:
 * it promises to preserve the pixels outside the rect, which a swapchain does
 * not actually do.
 */
public class VytoSurfaceView extends SurfaceView implements SurfaceHolder.Callback {

    private final CommandBuffer commands = new CommandBuffer();

    /**
     * The persistent backbuffer — ours alone. Unlike the {@link VytoView} case
     * this is never read by another thread, so {@link #bitmapLock} guards it
     * only against the resize on the UI thread.
     */
    private Bitmap bitmap;
    private Canvas bitmapCanvas;
    private final Object bitmapLock = new Object();

    /**
     * Guards the surface's liveness against an in-flight blit. surfaceDestroyed
     * must not return while the Vyto thread holds a locked canvas, so it blocks
     * on this — bounded by one frame.
     */
    private final Object surfaceLock = new Object();
    private boolean hasSurface = false;

    /**
     * Paint for the present blit. {@code SRC} rather than the default
     * {@code SRC_OVER}: the destination is a swapchain buffer we are about to
     * overwrite completely, so blending against whatever it held two frames ago
     * is both wrong in principle and the dominant cost in practice. Belt and
     * braces with {@code setHasAlpha(false)} — either alone should pick the
     * fast path, and neither is guaranteed to across Skia versions.
     */
    private final Paint blitPaint = new Paint();
    { blitPaint.setXfermode(new PorterDuffXfermode(PorterDuff.Mode.SRC)); }

    private boolean wantsTextInput = false;

    public VytoSurfaceView(Context ctx) {
        super(ctx);
        getHolder().addCallback(this);
        setFocusable(true);
        setFocusableInTouchMode(true);
    }

    CommandBuffer commands() { return commands; }

    // ------------------------------------------------------------- lifecycle

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        synchronized (surfaceLock) { hasSurface = true; }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {
        if (w <= 0 || h <= 0) return;
        synchronized (bitmapLock) {
            if (bitmap != null) bitmap.recycle();
            // ARGB_8888 matches Vyto's 0xAARRGGBB packing directly.
            bitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
            // Vyto fills the whole surface with the theme background every
            // frame, so the backbuffer is always fully opaque. Saying so turns
            // the present blit from a per-pixel SRC_OVER blend into a straight
            // copy — measured at 14.3ms/frame for 1080x2400 before this, which
            // was 62% of the frame and the reason scrolling was capped at 43fps.
            bitmap.setHasAlpha(false);
            bitmapCanvas = new Canvas(bitmap);
        }
        synchronized (surfaceLock) { hasSurface = true; }
        float density = getResources().getDisplayMetrics().density;
        if (Native.isLoaded()) Native.resize(w, h, density);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        // Blocks until any in-flight blit has posted. Returning early would let
        // the Vyto thread hold a canvas on a surface Android is tearing down.
        synchronized (surfaceLock) { hasSurface = false; }
    }

    // ---------------------------------------------------------------- present

    /**
     * Replay one frame and present it. Called from the <b>Vyto thread</b>.
     *
     * <p>The dirty rect is accepted for signature compatibility with
     * {@link VytoView} and deliberately ignored: a swapchain buffer does not
     * preserve what was outside it, so the blit is always full.
     */
    public void replayAndPost(ByteBuffer buf, int len,
                              int dx, int dy, int dw, int dh) {
        synchronized (bitmapLock) {
            if (bitmapCanvas == null) return;
            commands.replay(bitmapCanvas, buf, len);
        }
        synchronized (surfaceLock) {
            if (!hasSurface) return;
            SurfaceHolder h = getHolder();
            Canvas c = h.lockCanvas();
            if (c == null) return;
            try {
                synchronized (bitmapLock) {
                    if (bitmap != null) c.drawBitmap(bitmap, 0f, 0f, blitPaint);
                }
            } finally {
                h.unlockCanvasAndPost(c);
            }
        }
    }

    // ----------------------------------------------------------------- vsync

    private boolean vsyncWanted = false;
    private boolean vsyncPaused = false;
    private boolean vsyncPosted = false;

    private final Choreographer.FrameCallback frameCallback = new Choreographer.FrameCallback() {
        @Override
        public void doFrame(long frameTimeNanos) {
            vsyncPosted = false;
            syncVsync();
            Native.vsync();
        }
    };

    /**
     * Same contract as {@link VytoView#setVsyncEnabled}, kept identical so the
     * only difference between the two targets is how a frame reaches the
     * screen. {@code unlockCanvasAndPost} already throttles on the BufferQueue,
     * so gating is arguably redundant here — but keeping it means a measurement
     * comparing the two isolates one variable.
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

    /** UI thread. Called by the Activity around onPause/onResume. */
    public void setVsyncPaused(boolean paused) {
        vsyncPaused = paused;
        syncVsync();
    }

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

    @Override
    protected void onDetachedFromWindow() {
        vsyncPosted = false;
        Choreographer.getInstance().removeFrameCallback(frameCallback);
        super.onDetachedFromWindow();
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        syncVsync();
    }

    // ----------------------------------------------------------------- input

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (!Native.isLoaded()) return false;

        int action = e.getActionMasked();
        int index = e.getActionIndex();
        int id = e.getPointerId(index);

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

    public void setTextInputActive(final boolean on) {
        post(new Runnable() {
            @Override public void run() {
                wantsTextInput = on;
                InputMethodManager imm = (InputMethodManager)
                        getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
                if (imm == null) return;
                if (on) {
                    requestFocus();
                    imm.restartInput(VytoSurfaceView.this);
                    imm.showSoftInput(VytoSurfaceView.this, InputMethodManager.SHOW_IMPLICIT);
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
        // Shared with VytoView: the connection only needs a View to attach to.
        return new VytoView.VytoInputConnection(this);
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
}
