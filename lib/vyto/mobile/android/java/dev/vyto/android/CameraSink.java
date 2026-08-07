package dev.vyto.android;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.ImageFormat;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.media.Image;
import android.media.ImageReader;
import android.os.Handler;
import android.os.HandlerThread;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.util.Arrays;

/**
 * Java half of {@code camera.vt}: a camera preview that ends up as pixels
 * inside Vyto's own backbuffer, so it can be a <em>widget</em> rather than a
 * whole screen.
 *
 * <h2>Why frames are copied rather than composited</h2>
 *
 * The usual Android answer is to hand the camera a {@code SurfaceView} or
 * {@code TextureView} and let the compositor place it. That cannot work here:
 * Vyto draws every widget into one {@link Bitmap} it alone owns and blits the
 * whole thing per frame, so a foreign View would be a separate compositor
 * layer — above or below the entire UI, never interleaved with widgets, never
 * clipped to a card, and re-positioned a frame late on every scroll.
 *
 * <p>So the frames come to us instead. An {@link ImageReader} gives a
 * {@link Surface} with no View attached at all; each frame is converted to
 * ARGB and handed to the command buffer as an ordinary image handle, which
 * {@code OP_IMAGE} already knows how to draw. No new opcode, no new painter
 * method, no hole punched in the surface.
 *
 * <p>The price is one colour conversion plus one copy per frame, on the CPU,
 * because the compositing canvas is a software one. That is affordable at
 * preview sizes — which is exactly what a camera produces — and is why the
 * default is 640x480 rather than whatever the sensor can do.
 *
 * <h2>Three buffers, not one</h2>
 *
 * The reader thread converts into a Bitmap while the Vyto thread may be
 * blitting another. With one buffer that is a torn frame; with two, a producer
 * that laps the consumer writes into the very bitmap being drawn. Three, with
 * the one in use marked, means the producer always has somewhere safe to go.
 *
 * <p>This is the same pixel-ownership rule that the SurfaceView work turned
 * on: it is never enough that the *commands* are in order, the *pixels* must
 * have a single writer.
 *
 * <p>STATUS: written, never run on a device.
 */
public final class CameraSink {

    public static final int FACING_BACK = 0;
    public static final int FACING_FRONT = 1;

    private final Context context;
    private final CommandBuffer commands;
    private final CameraManager manager;

    private HandlerThread thread;
    private Handler handler;

    private CameraDevice device;
    private CameraCaptureSession session;
    private ImageReader reader;

    /** Rotating frame buffers, plus which one is ready and which is on screen. */
    private final Bitmap[] buffers = new Bitmap[3];
    private final Object lock = new Object();
    private int readyIndex = -1;    // newest converted frame
    private int inUseIndex = -1;    // handed to the command buffer

    private int imageHandle;        // 0 until the first start()
    private int outWidth, outHeight;
    private android.util.Range<Integer>[] fpsRanges;
    private String lastError = "";
    private String fpsChosen = "";
    private int rotation;           // degrees the frame must be turned by
    private boolean mirror;         // front camera: flip horizontally

    public CameraSink(Context c, CommandBuffer cb) {
        this.context = c.getApplicationContext();
        this.commands = cb;
        this.manager = (CameraManager) context.getSystemService(Context.CAMERA_SERVICE);
    }

    /** Is there a camera facing this way at all? Many tablets have no front one. */
    public boolean has(int facing) {
        return cameraIdFor(facing) != null;
    }

    private String cameraIdFor(int facing) {
        if (manager == null) return null;
        int want = facing == FACING_FRONT
                ? CameraCharacteristics.LENS_FACING_FRONT
                : CameraCharacteristics.LENS_FACING_BACK;
        try {
            for (String id : manager.getCameraIdList()) {
                Integer f = manager.getCameraCharacteristics(id).get(CameraCharacteristics.LENS_FACING);
                if (f != null && f == want) return id;
            }
        } catch (CameraAccessException e) {
            android.util.Log.w("Vyto", "getCameraIdList failed", e);
        }
        return null;
    }

    /**
     * Open the camera and start delivering frames.
     *
     * <p>Returns 0 when the permission is missing, no such camera exists, or
     * the open failed — all of which are states to handle rather than errors
     * to report. Otherwise returns the image handle to draw, which stays
     * valid until {@link #stop}.
     *
     * <p>{@code w}/{@code h} are a request; the camera picks the nearest size
     * it supports and the handle's bitmap is that size, rotated upright.
     */
    public int start(int facing, int w, int h) {
        if (context.checkSelfPermission(android.Manifest.permission.CAMERA)
                != android.content.pm.PackageManager.PERMISSION_GRANTED) {
            return 0;
        }
        String id = cameraIdFor(facing);
        if (id == null) return 0;
        stop();

        try {
            CameraCharacteristics ch = manager.getCameraCharacteristics(id);
            Integer sensor = ch.get(CameraCharacteristics.SENSOR_ORIENTATION);
            int sensorDeg = sensor == null ? 90 : sensor;
            // The sensor is mounted at some fixed angle to the device's natural
            // orientation, so a preview drawn without this is sideways on every
            // phone — the single most visible camera bug there is.
            fpsRanges = ch.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
            int displayDeg = displayRotationDegrees();
            mirror = facing == FACING_FRONT;
            rotation = mirror
                    ? (sensorDeg + displayDeg) % 360
                    : (sensorDeg - displayDeg + 360) % 360;

            thread = new HandlerThread("vyto-camera");
            thread.start();
            handler = new Handler(thread.getLooper());

            reader = ImageReader.newInstance(w, h, ImageFormat.YUV_420_888, 3);
            reader.setOnImageAvailableListener(onFrame, handler);

            // Rotated by a quarter turn, width and height swap.
            boolean quarter = rotation == 90 || rotation == 270;
            outWidth = quarter ? h : w;
            outHeight = quarter ? w : h;
            synchronized (lock) {
                for (int i = 0; i < buffers.length; i++) {
                    buffers[i] = Bitmap.createBitmap(outWidth, outHeight, Bitmap.Config.ARGB_8888);
                    buffers[i].setHasAlpha(false);
                }
                readyIndex = -1;
                inUseIndex = -1;
            }
            if (imageHandle == 0) imageHandle = commands.newImageHandle();

            manager.openCamera(id, stateCallback, handler);
            return imageHandle;
        } catch (CameraAccessException | SecurityException | IllegalArgumentException e) {
            android.util.Log.w("Vyto", "camera open failed", e);
            stop();
            return 0;
        }
    }

    private int displayRotationDegrees() {
        android.view.WindowManager wm = (android.view.WindowManager)
                context.getSystemService(Context.WINDOW_SERVICE);
        if (wm == null || wm.getDefaultDisplay() == null) return 0;
        switch (wm.getDefaultDisplay().getRotation()) {
            case Surface.ROTATION_90:  return 90;
            case Surface.ROTATION_180: return 180;
            case Surface.ROTATION_270: return 270;
            default:                   return 0;
        }
    }

    private final CameraDevice.StateCallback stateCallback = new CameraDevice.StateCallback() {
        @Override public void onOpened(CameraDevice cam) {
            device = cam;
            try {
                cam.createCaptureSession(Arrays.asList(reader.getSurface()),
                        sessionCallback, handler);
            } catch (CameraAccessException e) {
                android.util.Log.w("Vyto", "createCaptureSession failed", e);
            }
        }
        @Override public void onDisconnected(CameraDevice cam) { stop(); }
        @Override public void onError(CameraDevice cam, int error) {
            android.util.Log.w("Vyto", "camera error " + error);
            stop();
        }
    };

    private final CameraCaptureSession.StateCallback sessionCallback =
            new CameraCaptureSession.StateCallback() {
        @Override public void onConfigured(CameraCaptureSession s) {
            session = s;
            if (device == null) return;
            try {
                CaptureRequest.Builder b =
                        device.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
                b.addTarget(reader.getSurface());
                b.set(CaptureRequest.CONTROL_AF_MODE,
                      CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE);

                // Pin the frame rate, or auto-exposure lengthens the exposure
                // indoors and quietly delivers a third of what the camera can
                // do: measured at 8.6 published frames a second on a Redmi Note
                // 9S in room light, from a loop turning at 60.
                android.util.Range<Integer> fps = bestFpsRange();
                if (fps != null) {
                    b.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, fps);
                }
                try {
                    s.setRepeatingRequest(b.build(), null, handler);
                } catch (IllegalArgumentException e) {
                    // A range out of the advertised list, or one this device
                    // only honours in a mode we are not in. Retry without it:
                    // a slow preview beats no preview, and this failure would
                    // otherwise be a live camera delivering nothing — the
                    // camera indicator lit, the widget blank, and no exception
                    // anywhere the app can see.
                    android.util.Log.w("Vyto", "fps range " + fps + " rejected", e);
                    b.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, null);
                    s.setRepeatingRequest(b.build(), null, handler);
                }
            } catch (Exception e) {
                // Deliberately Exception, not CameraAccessException: everything
                // in here can throw IllegalArgumentException or IllegalState,
                // and an escape from this callback kills the camera thread
                // rather than the app, which reads as "the preview just does
                // not work" with nothing to go on.
                android.util.Log.w("Vyto", "setRepeatingRequest failed", e);
                lastError = String.valueOf(e);
            }
        }

        @Override public void onConfigureFailed(CameraCaptureSession s) {
            android.util.Log.w("Vyto", "camera session configure failed");
            stop();
        }
    };

    private final ImageReader.OnImageAvailableListener onFrame =
            new ImageReader.OnImageAvailableListener() {
        @Override public void onImageAvailable(ImageReader r) {
            Image img = null;
            try {
                img = r.acquireLatestImage();   // drop backlog; only "now" matters
                if (img == null || !Native.isLoaded()) return;
                int slot;
                synchronized (lock) {
                    slot = freeSlot();
                    if (slot < 0) return;       // consumer holds one, producer has one: skip
                }
                Image.Plane[] p = img.getPlanes();
                ByteBuffer y = p[0].getBuffer();
                ByteBuffer u = p[1].getBuffer();
                ByteBuffer v = p[2].getBuffer();
                Native.cameraFrame(buffers[slot], y, u, v,
                                   p[0].getRowStride(), p[1].getRowStride(), p[2].getRowStride(),
                                   p[1].getPixelStride(),
                                   img.getWidth(), img.getHeight(), rotation, mirror);
                synchronized (lock) { readyIndex = slot; }
            } catch (IllegalStateException e) {
                // acquireLatestImage throws when the queue is exhausted, which
                // happens if a frame is held too long. Dropping it is correct.
            } finally {
                if (img != null) img.close();
            }
        }
    };

    /**
     * The advertised range that will actually deliver frames fastest.
     *
     * <p>Highest floor wins — a [10,30] range is precisely the one that drops
     * to 10 in room light. Ranges above 30 are skipped: a device that lists
     * [60,60] usually honours it only in a high-speed session, and asking for
     * it in an ordinary one is one of the ways this call is rejected.
     */
    private android.util.Range<Integer> bestFpsRange() {
        android.util.Range<Integer>[] rs = fpsRanges;
        if (rs == null) return null;
        android.util.Range<Integer> best = null;
        for (android.util.Range<Integer> r : rs) {
            if (r.getUpper() > 30) continue;
            if (best == null
                    || r.getLower() > best.getLower()
                    || (r.getLower().equals(best.getLower())
                        && r.getUpper() > best.getUpper())) {
                best = r;
            }
        }
        fpsChosen = String.valueOf(best);
        return best;
    }

    /** What the last failure was, for an app that wants to show it. */
    public String diagnostics() {
        return "fps=" + fpsChosen + (lastError.isEmpty() ? "" : " err=" + lastError);
    }

    /** A buffer that is neither on screen nor the newest ready one. */
    private int freeSlot() {
        for (int i = 0; i < buffers.length; i++) {
            if (buffers[i] == null) return -1;
            if (i != inUseIndex && i != readyIndex) return i;
        }
        return -1;
    }

    /**
     * Publish the newest frame to the command buffer. <b>Called on the Vyto
     * thread</b>, from the widget's paint, which is the only thread allowed to
     * touch {@code CommandBuffer.images} — the map is read during replay, and a
     * write from the reader thread would race both the map and the pixels.
     *
     * <p>Returns true when a new frame was published, so a widget can repaint
     * only when there is something new: a 30fps camera must not force 60
     * repaints, and an idle one must force none.
     */
    public boolean sync() {
        int slot;
        synchronized (lock) {
            if (readyIndex < 0 || readyIndex == inUseIndex) return false;
            slot = readyIndex;
            inUseIndex = slot;
        }
        commands.putImage(imageHandle, buffers[slot]);
        return true;
    }

    public int width()  { return outWidth; }
    public int height() { return outHeight; }

    /**
     * Close the camera and release the buffers. Must run when the app is
     * backgrounded: a held camera is denied to every other app, and newer
     * Android revokes it anyway — noisily.
     */
    public void stop() {
        try {
            if (session != null) { session.close(); session = null; }
            if (device != null) { device.close(); device = null; }
            if (reader != null) { reader.close(); reader = null; }
        } catch (Exception e) {
            android.util.Log.w("Vyto", "camera close failed", e);
        }
        if (thread != null) { thread.quitSafely(); thread = null; handler = null; }
        synchronized (lock) {
            readyIndex = -1;
            inUseIndex = -1;
        }
        // The bitmaps are deliberately NOT recycled here: the command buffer may
        // still hold the last one against the image handle, and recycling a
        // bitmap out from under a draw is a crash rather than a stale frame.
        // dispose() drops the handle first, which makes it safe.
    }

    public void dispose() {
        stop();
        if (imageHandle != 0) { commands.dropImage(imageHandle); imageHandle = 0; }
        synchronized (lock) {
            for (int i = 0; i < buffers.length; i++) {
                if (buffers[i] != null) { buffers[i].recycle(); buffers[i] = null; }
            }
        }
    }
}
