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

    /**
     * The render target, as a plain View. Exactly one of {@link #view} /
     * {@link #surfaceView} is non-null; see {@link #useSurfaceView()}.
     */
    private View target;
    private VytoView view;
    private VytoSurfaceView surfaceView;
    private Actions actions;
    private Streams streams;

    /**
     * Which rendering target to build: {@link VytoSurfaceView} (the default) or
     * {@link VytoView}.
     *
     * <p><b>SurfaceView is the default because it measurably fixed the stutter
     * and flicker</b> that survived every other change — dp-scaled hit targets,
     * removing the {@code custom_paint} strobe, Choreographer-gated present.
     * On a Redmi Note 9S the two were compared back to back on the same build
     * and the same finger: {@link VytoView} stuttered and flickered under
     * touch, this one did neither. The cause is in that class's comment.
     *
     * <p>{@link VytoView} is kept, not deprecated. SurfaceView owns a separate
     * compositor layer, which costs things a full-screen app does not miss but
     * an embedded one would: it does not transform with the View hierarchy
     * (no animating, scaling or shared-element transition of the surface), it
     * does not interleave in z-order with sibling Views, and
     * {@code dumpsys gfxinfo} no longer sees the drawing. An app that embeds
     * Vyto inside a larger Android layout should override this to false.
     *
     * <p>Switchable per launch without a rebuild, which is how the two were
     * compared and how a regression can be bisected:
     *
     * <pre>am start -n &lt;pkg&gt;/dev.vyto.android.VytoActivity --ez vyto.surfaceview false</pre>
     */
    protected boolean useSurfaceView() {
        return getIntent() == null
                || getIntent().getBooleanExtra("vyto.surfaceview", true);
    }

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

        if (useSurfaceView()) {
            surfaceView = new VytoSurfaceView(this);
            target = surfaceView;
        } else {
            view = new VytoView(this);
            target = view;
        }
        android.util.Log.i("Vyto", "render target: "
                + target.getClass().getSimpleName());
        actions = new Actions(this);
        streams = new Streams(this);
        setContentView(target);

        // Bind before start(): the Vyto thread can reach for Actions as soon as
        // it is running, and a null there would drop the first launch.
        Native.bindActions(actions);
        Native.bindStreams(streams);

        // Also before start(), and for a sharper reason: os_app_dir() caches
        // its answer the first time anything asks, and the Vyto thread asks as
        // soon as it opens a file. Without this appDir() is a build-host path
        // that does not exist here, and nothing an app writes survives.
        Native.setAppDirs(getFilesDir().getAbsolutePath(),
                          getCacheDir().getAbsolutePath());

        // adjustResize (set in the manifest) shrinks the window for the IME,
        // which fires EV_RESIZE and re-runs layout. Ask for inset callbacks so
        // the app can also do safe-area work.
        target.setFitsSystemWindows(false);
        if (Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
        }

        // The intent that launched us: a deep link, a share, or a notification
        // tap. Pushed before the thread starts, which the native queue is
        // built to allow — losing this one would lose the most common way an
        // app is opened by something other than its icon.
        deliverIntent(getIntent());

        // The Vyto thread starts once the View has a size; onSizeChanged is the
        // first point where the backbuffer exists. Posting rather than starting
        // here avoids a 0x0 first frame.
        target.post(new Runnable() {
            @Override public void run() {
                float density = getResources().getDisplayMetrics().density;
                Native.start(target, target.getWidth(), target.getHeight(), density);
            }
        });
    }

    /**
     * The app was already running when a link or a share arrived.
     * {@code launchMode} is singleTop, so this fires instead of a second
     * Activity being created — which is also why the widget tree survives it.
     */
    @Override
    protected void onNewIntent(android.content.Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        deliverIntent(intent);
    }

    /** Flatten an Intent into the four strings Vyto reads, and queue it. */
    private void deliverIntent(android.content.Intent i) {
        if (i == null || !Native.isLoaded()) return;
        String action = i.getAction() == null ? "" : i.getAction();
        int notification = i.getIntExtra("vyto.notification", 0);
        // MAIN with no data is the launcher icon: not an incoming intent, and
        // queueing it would make every cold start look like a deep link.
        if (notification == 0 && i.getData() == null
                && android.content.Intent.ACTION_MAIN.equals(action)) {
            return;
        }
        String uri = i.getData() == null ? "" : i.getData().toString();
        String mime = i.getType() == null ? "" : i.getType();
        String text = "";
        CharSequence extra = i.getCharSequenceExtra(android.content.Intent.EXTRA_TEXT);
        if (extra != null) {
            text = extra.toString();
        } else if (i.hasExtra(android.content.Intent.EXTRA_STREAM)) {
            // A shared file arrives as a content:// URI in EXTRA_STREAM, not as
            // data. Surfacing it as `uri` keeps one field to read, and it is
            // the field Actions.open_fd already knows how to open.
            Object stream = i.getParcelableExtra(android.content.Intent.EXTRA_STREAM);
            if (stream != null && uri.isEmpty()) uri = stream.toString();
        }
        Native.incomingIntent(action, uri, mime, text, notification);
    }

    @Override
    protected void onResume() {
        super.onResume();
        setVsyncPaused(false);
        if (Native.isLoaded()) Native.resume();
    }

    @Override
    protected void onPause() {
        // Every sensor and the location provider, off. Not a nicety: a
        // registered listener keeps waking the device for an app the user has
        // left, and nothing else in the stack will notice.
        if (streams != null) streams.stopAll();
        // Park the loop so animations stop waking it. The widget tree and the
        // backbuffer are untouched — there is no surface to lose, which is the
        // main simplification the Bitmap design buys over SurfaceView.
        if (Native.isLoaded()) Native.pause();
        // After Native.pause, so the loop is already parked: a frame callback
        // that slips through in between is dropped natively rather than waking it.
        setVsyncPaused(true);
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        if (Native.isLoaded()) Native.stop();
        if (actions != null) actions.dispose();
        if (streams != null) streams.dispose();
        if (view != null) view.releaseBitmap();
        if (surfaceView != null) surfaceView.releaseBitmap();
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

    /** Non-null only on the VytoView path; null when the SurfaceView is in use. */
    public VytoView vytoView() { return view; }

    /** Non-null only on the VytoSurfaceView path. */
    public VytoSurfaceView vytoSurfaceView() { return surfaceView; }

    /** The active render target, whichever kind it is. */
    public View renderTarget() { return target; }

    /**
     * Fan the pause flag out to whichever target exists. The two classes cannot
     * share a supertype carrying this — one extends View, the other SurfaceView
     * — and an interface for two methods is not worth the indirection while
     * this is an experiment.
     */
    private void setVsyncPaused(boolean paused) {
        if (view != null) view.setVsyncPaused(paused);
        if (surfaceView != null) surfaceView.setVsyncPaused(paused);
    }
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
