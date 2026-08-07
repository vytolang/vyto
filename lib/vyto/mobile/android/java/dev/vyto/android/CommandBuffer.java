package dev.vyto.android;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RadialGradient;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.os.Build;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.HashMap;

/**
 * Decodes the native command buffer onto an {@link android.graphics.Canvas}.
 *
 * <p>This is the Java half of {@code AndroidPainter}
 * ({@code lib/vyto/mobile/android/painter.vt}). Every draw call on the Vyto
 * side encodes into a native {@code DirectByteBuffer}; nothing crosses JNI
 * until present, at which point the whole frame replays here in one pass.
 * That batching is load-bearing, not an optimization — per-call JNI with a
 * {@code NewStringUTF} per {@code drawText} will not hold a frame budget.
 *
 * <p>STATUS: written, never compiled. The matching encoder is
 * {@code native/src/apainter_shim.c}; the two halves are the wire-format
 * contract below and must not drift.
 * Design of record: {@code local/docs/ANDROID.md}.
 *
 * <h2>Wire format</h2>
 *
 * Native byte order, {@code int32} opcode followed by that opcode's fixed
 * arity. Coordinates are {@code float32} — narrowed from Vyto's {@code f64}
 * by the encoder, because {@code Canvas} takes float anyway and it halves the
 * buffer. Colors are {@code int32} and pass through <b>unconverted</b>: Vyto
 * packs {@code 0xAARRGGBB} ({@code surface.vt:178-190}), which is exactly
 * {@code android.graphics.Color}.
 *
 * <p><b>These constants are duplicated in the C shim and must not drift.</b>
 * They are the contract between the two halves; a mismatch decodes as garbage
 * rather than failing, so treat any change as a wire-format break.
 *
 * <h2>Strings</h2>
 *
 * A string crosses the boundary once, ever. First sighting emits
 * {@link #OP_DEFINE_STR} carrying its UTF-8 bytes and an id; every later draw
 * or measure references the id alone. The table lives for the painter's
 * lifetime, which is the app's, because UI text repeats heavily frame to frame.
 */
public final class CommandBuffer {

    // ------------------------------------------------------------- opcodes
    public static final int OP_END            = 0;
    public static final int OP_FILL_RECT      = 1;   // f x,y,w,h            i color
    public static final int OP_ROUND_RECT     = 2;   // f x,y,w,h,r          i color
    public static final int OP_STROKE_ROUND   = 3;   // f x,y,w,h,r,width    i color
    public static final int OP_CIRCLE         = 4;   // f cx,cy,rad          i color
    public static final int OP_STROKE_CIRCLE  = 5;   // f cx,cy,rad,width    i color
    public static final int OP_LINE           = 6;   // f x0,y0,x1,y1,width  i color
    public static final int OP_ARC            = 7;   // f cx,cy,rx,ry,a0,a1,width  i color
    public static final int OP_POLYGON        = 8;   // i n  f xs[n] ys[n]   i color
    public static final int OP_GRAD_V         = 9;   // f x,y,w,h,r          i top,bottom
    public static final int OP_GRAD_N         = 10;  // f x,y,w,h,r  i n  i colors[n]  f pos[n]
    public static final int OP_RADIAL         = 11;  // f x,y,w,h,r,cx,cy,rad  i n  colors  pos
    public static final int OP_SHADOW         = 12;  // f x,y,w,h,r,blur,dy  i color
    public static final int OP_BEVEL          = 13;  // f x,y,w,h,r,width    i light,dark,raised
    public static final int OP_CLIP_PUSH      = 14;  // f x,y,w,h,r
    public static final int OP_CLIP_POP       = 15;
    public static final int OP_SAVE           = 16;
    public static final int OP_RESTORE        = 17;
    public static final int OP_TRANSLATE      = 18;  // f dx,dy
    public static final int OP_SCALE          = 19;  // f sx,sy
    public static final int OP_ROTATE         = 20;  // f deg
    public static final int OP_SET_FONT       = 21;  // f size  i weight
    public static final int OP_TEXT           = 22;  // f x,y   i strId, color
    public static final int OP_IMAGE          = 23;  // i handle  f x,y,w,h
    public static final int OP_DEFINE_STR     = 24;  // i id, byteLen, then bytes (padded to 4)

    // Must match WEIGHT_REG/MED/BOLD in ui/core.vt:490-493.
    public static final int WEIGHT_REG  = 0;
    public static final int WEIGHT_MED  = 1;
    public static final int WEIGHT_BOLD = 2;

    // --------------------------------------------------------------- state

    /** Reused across the whole frame; allocating per op would dominate the replay. */
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

    /**
     * Separate Paint for measurement. {@link #measureText} runs on the Vyto
     * thread while a replay may be in flight on the same thread — but never
     * concurrently with {@link #replay}, since both belong to the Vyto thread.
     * Keeping them apart is about not having measurement perturb the frame's
     * current font state, not about threading.
     */
    private final Paint measurePaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private final HashMap<Integer, String> strings = new HashMap<>();
    private final HashMap<Integer, Bitmap> images = new HashMap<>();
    private final RectF rf = new RectF();
    private final Path path = new Path();

    private float fontSize = 15f;
    private int fontWeight = WEIGHT_REG;

    public CommandBuffer() {
        paint.setTypeface(typefaceFor(WEIGHT_REG));
        measurePaint.setTypeface(typefaceFor(WEIGHT_REG));
    }

    private static Typeface typefaceFor(int weight) {
        if (Build.VERSION.SDK_INT >= 28) {
            int w = weight == WEIGHT_BOLD ? 700 : (weight == WEIGHT_MED ? 500 : 400);
            return Typeface.create(Typeface.DEFAULT, w, false);
        }
        // Pre-28 has no numeric weight axis, so medium collapses to regular
        // rather than rendering as bold — the same graceful degradation the
        // lean tier makes when it cannot resize its bitmap font.
        return weight == WEIGHT_BOLD ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT;
    }

    private int nextImageHandle = 1;

    /** Register a decoded bitmap against the handle the native side hands out. */
    public void putImage(int handle, Bitmap bmp) { images.put(handle, bmp); }

    /**
     * Reserve a handle with no bitmap behind it yet, for a producer that
     * supplies frames rather than decoding a file — the camera sink, and
     * anything else that ends in {@link #putImage}.
     *
     * <p>Both this and {@code putImage} must be called from the Vyto thread:
     * {@code images} is a plain map read during {@link #replay}, so a write
     * from a producer thread would race the map as well as the pixels.
     */
    public int newImageHandle() { return nextImageHandle++; }

    /**
     * Decode a file into a Bitmap and return its handle, or 0 on failure.
     * Called from the Vyto thread via {@code vta_load_image}.
     *
     * <p>BitmapFactory replaces blend2d's decoder, which is one of the things
     * dropping blend2d buys back rather than costs.
     *
     * <p>TODO: assets embedded by {@code --with-assets} live in the native VFS
     * ({@code runtime/vyto_vfs.c}), not on disk, so this path misses them.
     * They need a bytes-taking sibling that the shim feeds from
     * {@code vt_vfs_ptr}. Disk paths work today; embedded images do not.
     */
    public int loadImage(String path) {
        if (path == null || path.isEmpty()) return 0;
        Bitmap b = android.graphics.BitmapFactory.decodeFile(path);
        if (b == null) {
            android.util.Log.w("Vyto", "decodeFile failed: " + path);
            return 0;
        }
        int h = nextImageHandle++;
        images.put(h, b);
        return h;
    }

    public void dropImage(int handle) {
        Bitmap b = images.remove(handle);
        if (b != null) b.recycle();
    }

    /**
     * Define a string outside the frame buffer. Used by the measure path, which
     * runs during layout — before any frame flushes — so a string that gets
     * measured is already resident by the time it is drawn.
     */
    public void defineString(int id, String s) { strings.put(id, s); }

    /**
     * Text advance for the interned id at the given size and weight.
     *
     * <p>Called from the Vyto thread on a cache miss in the native memo table.
     * This is the one synchronous JNI round trip in the design and the reason
     * that table exists — {@code text_width} sits on the hot layout path and is
     * called thousands of times per layout ({@code Painter.text_width}).
     */
    public float measureText(int id, float size, int weight) {
        String s = strings.get(id);
        if (s == null) return 0f;
        measurePaint.setTextSize(size);
        measurePaint.setTypeface(typefaceFor(weight));
        return measurePaint.measureText(s);
    }

    public float fontAscent(float size, int weight) {
        measurePaint.setTextSize(size);
        measurePaint.setTypeface(typefaceFor(weight));
        return -measurePaint.getFontMetrics().ascent;
    }

    public float fontHeight(float size, int weight) {
        measurePaint.setTextSize(size);
        measurePaint.setTypeface(typefaceFor(weight));
        Paint.FontMetrics fm = measurePaint.getFontMetrics();
        return fm.descent - fm.ascent;
    }

    // -------------------------------------------------------------- replay

    /**
     * Decode and draw one frame.
     *
     * <p>Runs on the Vyto thread, drawing into the Bitmap-backed Canvas that
     * {@link VytoView} owns. The caller holds the bitmap lock for the duration —
     * see {@link VytoView#replayAndPost}.
     *
     * @param len byte length of valid data; the buffer itself may be larger.
     */

    public void replay(Canvas c, ByteBuffer buf, int len) {
        buf.order(ByteOrder.nativeOrder());
        buf.position(0);
        buf.limit(len);

        // The Canvas belongs to the View and is reused for every frame, so the
        // save stack is NOT reset between replays. A frame that ends unbalanced
        // — one clip_push too many, or the decode-desync return below — would
        // leave a clip or a transform in place for the entire life of the app,
        // and every later frame would draw into the wrong region. Bracketing
        // the whole replay makes each frame independent of the last one no
        // matter how it exits.
        final int base = c.save();
        try {
            replayOps(c, buf, base);
        } finally {
            c.restoreToCount(base);
        }
    }

    private void replayOps(Canvas c, ByteBuffer buf, int base) {
        while (buf.remaining() >= 4) {
            int op = buf.getInt();
            switch (op) {
                case OP_END:
                    return;

                case OP_FILL_RECT: {
                    float x = buf.getFloat(), y = buf.getFloat();
                    float w = buf.getFloat(), h = buf.getFloat();
                    solid(buf.getInt());
                    c.drawRect(x, y, x + w, y + h, paint);
                    break;
                }
                case OP_ROUND_RECT: {
                    rect(buf);
                    float r = buf.getFloat();
                    solid(buf.getInt());
                    c.drawRoundRect(rf, r, r, paint);
                    break;
                }
                case OP_STROKE_ROUND: {
                    rect(buf);
                    float r = buf.getFloat(), width = buf.getFloat();
                    stroke(buf.getInt(), width);
                    c.drawRoundRect(rf, r, r, paint);
                    break;
                }
                case OP_CIRCLE: {
                    float cx = buf.getFloat(), cy = buf.getFloat(), rad = buf.getFloat();
                    solid(buf.getInt());
                    c.drawCircle(cx, cy, rad, paint);
                    break;
                }
                case OP_STROKE_CIRCLE: {
                    float cx = buf.getFloat(), cy = buf.getFloat();
                    float rad = buf.getFloat(), width = buf.getFloat();
                    stroke(buf.getInt(), width);
                    c.drawCircle(cx, cy, rad, paint);
                    break;
                }
                case OP_LINE: {
                    float x0 = buf.getFloat(), y0 = buf.getFloat();
                    float x1 = buf.getFloat(), y1 = buf.getFloat();
                    float width = buf.getFloat();
                    stroke(buf.getInt(), width);
                    c.drawLine(x0, y0, x1, y1, paint);
                    break;
                }
                case OP_ARC: {
                    float cx = buf.getFloat(), cy = buf.getFloat();
                    float rx = buf.getFloat(), ry = buf.getFloat();
                    float a0 = buf.getFloat(), a1 = buf.getFloat();
                    float width = buf.getFloat();
                    stroke(buf.getInt(), width);
                    rf.set(cx - rx, cy - ry, cx + rx, cy + ry);
                    c.drawArc(rf, a0, a1 - a0, false, paint);
                    break;
                }
                case OP_POLYGON: {
                    // xs and ys arrive as two separate runs (Vyto passes them as
                    // two arrays), so xs is buffered before the path is built.
                    int n = buf.getInt();
                    float[] xs = new float[n];
                    for (int i = 0; i < n; i++) xs[i] = buf.getFloat();
                    path.reset();
                    for (int i = 0; i < n; i++) {
                        float py = buf.getFloat();
                        if (i == 0) path.moveTo(xs[i], py);
                        else path.lineTo(xs[i], py);
                    }
                    path.close();
                    solid(buf.getInt());
                    c.drawPath(path, paint);
                    break;
                }
                case OP_GRAD_V: {
                    rect(buf);
                    float r = buf.getFloat();
                    int top = buf.getInt(), bottom = buf.getInt();
                    shaded(new LinearGradient(rf.left, rf.top, rf.left, rf.bottom,
                            top, bottom, Shader.TileMode.CLAMP));
                    c.drawRoundRect(rf, r, r, paint);
                    paint.setShader(null);
                    break;
                }
                case OP_GRAD_N: {
                    rect(buf);
                    float r = buf.getFloat();
                    int n = buf.getInt();
                    int[] colors = new int[n];
                    float[] pos = new float[n];
                    for (int i = 0; i < n; i++) colors[i] = buf.getInt();
                    for (int i = 0; i < n; i++) pos[i] = buf.getFloat();
                    shaded(new LinearGradient(rf.left, rf.top, rf.left, rf.bottom,
                            colors, pos, Shader.TileMode.CLAMP));
                    c.drawRoundRect(rf, r, r, paint);
                    paint.setShader(null);
                    break;
                }
                case OP_RADIAL: {
                    rect(buf);
                    float r = buf.getFloat();
                    float cx = buf.getFloat(), cy = buf.getFloat(), rad = buf.getFloat();
                    int n = buf.getInt();
                    int[] colors = new int[n];
                    float[] pos = new float[n];
                    for (int i = 0; i < n; i++) colors[i] = buf.getInt();
                    for (int i = 0; i < n; i++) pos[i] = buf.getFloat();
                    shaded(new RadialGradient(cx, cy, rad, colors, pos,
                            Shader.TileMode.CLAMP));
                    c.drawRoundRect(rf, r, r, paint);
                    paint.setShader(null);
                    break;
                }
                case OP_SHADOW: {
                    rect(buf);
                    float r = buf.getFloat(), blur = buf.getFloat(), dy = buf.getFloat();
                    int color = buf.getInt();
                    // setShadowLayer is a software-canvas feature and this
                    // canvas is always software (Bitmap-backed), so it is
                    // available here in a way it would not be on a hardware
                    // canvas. Blur radius must be > 0 or it throws.
                    paint.setStyle(Paint.Style.FILL);
                    paint.setColor(0);
                    paint.setShadowLayer(Math.max(blur, 0.1f), 0f, dy, color);
                    c.drawRoundRect(rf, r, r, paint);
                    paint.clearShadowLayer();
                    // Leave the paint opaque again. The alpha-0 above is what
                    // makes the shape itself invisible so only its shadow lands,
                    // but a Shader set by a later op is modulated by that same
                    // alpha — see shaded().
                    paint.setColor(0xFF000000);
                    break;
                }
                case OP_BEVEL: {
                    rect(buf);
                    float r = buf.getFloat(), width = buf.getFloat();
                    int light = buf.getInt(), dark = buf.getInt();
                    boolean raised = buf.getInt() != 0;
                    int top = raised ? light : dark;
                    int bot = raised ? dark : light;
                    stroke(top, width);
                    c.drawArc(rf, 135f, 180f, false, paint);
                    stroke(bot, width);
                    c.drawArc(rf, -45f, 180f, false, paint);
                    break;
                }
                case OP_CLIP_PUSH: {
                    rect(buf);
                    float r = buf.getFloat();
                    c.save();
                    if (r > 0f) {
                        path.reset();
                        path.addRoundRect(rf, r, r, Path.Direction.CW);
                        c.clipPath(path);
                    } else {
                        c.clipRect(rf);
                    }
                    break;
                }
                case OP_CLIP_POP:
                case OP_RESTORE:
                    // Never pop past this frame's base: an extra restore would
                    // throw and abort the frame half-drawn. The outer
                    // restoreToCount still unwinds whatever is left.
                    if (c.getSaveCount() > base + 1) {
                        c.restore();
                    } else {
                        android.util.Log.w("Vyto", "clip_pop underflow — unbalanced clip in a widget");
                    }
                    break;
                case OP_SAVE:
                    c.save();
                    break;
                case OP_TRANSLATE:
                    c.translate(buf.getFloat(), buf.getFloat());
                    break;
                case OP_SCALE:
                    c.scale(buf.getFloat(), buf.getFloat());
                    break;
                case OP_ROTATE:
                    c.rotate(buf.getFloat());
                    break;

                case OP_SET_FONT: {
                    fontSize = buf.getFloat();
                    fontWeight = buf.getInt();
                    paint.setTextSize(fontSize);
                    paint.setTypeface(typefaceFor(fontWeight));
                    break;
                }
                case OP_TEXT: {
                    float x = buf.getFloat(), y = buf.getFloat();
                    int id = buf.getInt();
                    solid(buf.getInt());
                    String s = strings.get(id);
                    if (s != null) {
                        paint.setTextSize(fontSize);
                        paint.setTypeface(typefaceFor(fontWeight));
                        // y is a baseline on both sides, so no adjustment.
                        c.drawText(s, x, y, paint);
                    }
                    break;
                }
                case OP_IMAGE: {
                    int handle = buf.getInt();
                    float x = buf.getFloat(), y = buf.getFloat();
                    float w = buf.getFloat(), h = buf.getFloat();
                    Bitmap b = images.get(handle);
                    if (b != null) {
                        rf.set(x, y, x + w, y + h);
                        paint.setShader(null);
                        paint.setStyle(Paint.Style.FILL);
                        paint.setColor(0xFFFFFFFF);
                        c.drawBitmap(b, null, rf, paint);
                    }
                    break;
                }
                case OP_DEFINE_STR: {
                    int id = buf.getInt();
                    int n = buf.getInt();
                    byte[] bytes = new byte[n];
                    buf.get(bytes);
                    int pad = (4 - (n & 3)) & 3;   // keep the stream 4-aligned
                    buf.position(buf.position() + pad);
                    try {
                        strings.put(id, new String(bytes, "UTF-8"));
                    } catch (java.io.UnsupportedEncodingException e) {
                        strings.put(id, "");
                    }
                    break;
                }

                default:
                    // A decode desync corrupts every op after it, so stop
                    // rather than draw garbage. Loud, because it means the C
                    // shim and this file have drifted.
                    android.util.Log.e("Vyto", "bad opcode " + op
                            + " at byte " + (buf.position() - 4) + " — wire format drift");
                    return;
            }
        }
    }

    /** Reads x,y,w,h into the shared RectF as left/top/right/bottom. */
    private void rect(ByteBuffer buf) {
        float x = buf.getFloat(), y = buf.getFloat();
        float w = buf.getFloat(), h = buf.getFloat();
        rf.set(x, y, x + w, y + h);
    }

    private void solid(int color) {
        paint.setStyle(Paint.Style.FILL);
        paint.setShader(null);
        paint.setColor(color);
    }

    /**
     * Fill with a shader.
     *
     * <p>Resets the paint colour to opaque first, which is load-bearing: a
     * Shader supplies the RGB but the paint's <b>alpha still modulates it</b>,
     * and OP_SHADOW leaves the paint at {@code setColor(0)} — alpha 0. Any
     * gradient replayed after a shadow therefore drew fully transparent.
     *
     * <p>That is not a hypothetical: a Button is {@code shadow()} immediately
     * followed by {@code gradient_v()} (ui/core.vt:914-916), so on device it
     * rendered as a bare blurred shadow with no fill, and flickered whenever a
     * partial repaint happened to reorder what preceded it.
     */
    private void shaded(Shader s) {
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(0xFF000000);
        paint.setShader(s);
    }

    private void stroke(int color, float width) {
        paint.setStyle(Paint.Style.STROKE);
        paint.setShader(null);
        paint.setStrokeWidth(width);
        paint.setColor(color);
    }
}
