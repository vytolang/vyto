package dev.vyto.android;

import android.app.Activity;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.WindowManager;

/**
 * Host Activity. Owns the {@link VytoView}, starts and stops the Vyto thread,
 * and routes lifecycle, back, and intent results.
 *
 * <p>Apps normally use this directly — the manifest names it and nothing else
 * is required. Subclass only to add behaviour; the library name comes from
 * {@link #libraryName()}.
 *
 * <p>STATUS: written, never compiled — see {@link Native}.
 * Design of record: {@code local/docs/ANDROID.md}.
 *
 * <h2>Why this Activity never dies on rotation</h2>
 *
 * {@code manifest.vt} emits a broad {@code android:configChanges}, so
 * orientation and density changes arrive as {@link #onConfigurationChanged}
 * rather than as a destroy/recreate. That is not a performance choice: Vyto's
 * {@code Window} tears down its entire widget tree on {@code EV_CLOSE}
 * ({@code ui/core.vt:2486-2508}) with no path back, so an Activity restart is
 * unrecoverable. If you find yourself removing configChanges, read
 * ANDROID.md first.
 */
public class VytoActivity extends Activity {

    private VytoView view;
    private Actions actions;

    /**
     * The {@code vytoc -o} stem for this app's {@code .so}. Override in a
     * subclass, or set {@code vyto.lib} metadata in the manifest.
     */
    protected String libraryName() {
        return "vytoapp";
    }

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);

        if (!Native.load(libraryName())) {
            // An ABI or packaging failure is not a crash to bury — Native.load
            // already logged which ABI ran. Show something rather than dying
            // with a stack trace the user cannot act on.
            setContentView(new ErrorView(this, "Vyto library '"
                    + libraryName() + "' failed to load."));
            return;
        }

        view = new VytoView(this);
        actions = new Actions(this);
        setContentView(view);

        // Bind before start(): the Vyto thread can reach for Actions as soon as
        // it is running, and a null there would drop the first launch.
        Native.bindActions(actions);

        // adjustResize (set in the manifest) shrinks the window for the IME,
        // which fires EV_RESIZE and re-runs layout. Ask for inset callbacks so
        // the app can also do safe-area work.
        view.setFitsSystemWindows(false);
        if (Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
        }

        // The Vyto thread starts once the View has a size; onSizeChanged is the
        // first point where the backbuffer exists. Posting rather than starting
        // here avoids a 0x0 first frame.
        view.post(new Runnable() {
            @Override public void run() {
                float density = getResources().getDisplayMetrics().density;
                Native.start(view, view.getWidth(), view.getHeight(), density);
            }
        });
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (Native.isLoaded()) Native.resume();
    }

    @Override
    protected void onPause() {
        // Park the loop so animations stop waking it. The widget tree and the
        // backbuffer are untouched — there is no surface to lose, which is the
        // main simplification the Bitmap design buys over SurfaceView.
        if (Native.isLoaded()) Native.pause();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        if (Native.isLoaded()) Native.stop();
        if (actions != null) actions.dispose();
        if (view != null) view.releaseBitmap();
        super.onDestroy();
    }

    @Override
    public void onConfigurationChanged(android.content.res.Configuration cfg) {
        super.onConfigurationChanged(cfg);
        // The View gets its own onSizeChanged, which calls Native.resize. This
        // override exists so the Activity is not recreated; the work happens
        // there. Live density re-scaling is still a Track C gap — Window.scale
        // is sampled once in init (ui/core.vt:1872) and never re-read.
    }

    @Override
    public void onBackPressed() {
        // Vyto gets first refusal: it may close an overlay or pop a screen.
        // Until back navigation exists (Track C item 3) this always returns
        // false and back exits the app, which is the current honest behaviour.
        if (Native.isLoaded() && Native.back()) return;
        super.onBackPressed();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, android.content.Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (actions != null) actions.onActivityResult(requestCode, resultCode, data);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions,
                                           int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (actions != null) actions.onPermissionResult(requestCode, grantResults);
    }

    public VytoView vytoView() { return view; }
    public Actions actions() { return actions; }

    /** Minimal failure screen; avoids pulling in any resource or layout file. */
    private static final class ErrorView extends View {
        private final String msg;
        private final android.graphics.Paint p = new android.graphics.Paint();

        ErrorView(android.content.Context c, String msg) {
            super(c);
            this.msg = msg;
            p.setColor(0xFFFFFFFF);
            p.setTextSize(36f);
            p.setAntiAlias(true);
        }

        @Override protected void onDraw(android.graphics.Canvas c) {
            c.drawColor(0xFF802020);
            c.drawText(msg, 40f, getHeight() / 2f, p);
        }
    }
}
