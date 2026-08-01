package dev.vyto.android;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.IOException;

/**
 * Minimal {@link ContentProvider} replacing {@code androidx.core.content.FileProvider}.
 *
 * <p>AndroidX would be an AAR, which is Maven resolution, which is Gradle — and
 * the whole build path is deliberately Gradle-free (ANDROID.md, "No Gradle").
 * This covers exactly what the two callers need: an authority,
 * {@link #openFile}, and enough of {@link #query} that a receiving app can read
 * a display name and size.
 *
 * <p>STATUS: STUB — never exercised against a real camera app.
 *
 * <h2>Why this exists at all</h2>
 *
 * {@code ACTION_IMAGE_CAPTURE} does not hand back a photo. It writes to a URI
 * <i>you</i> supply, and passing a {@code file://} URI has thrown
 * {@code FileUriExposedException} since API 24. So a camera app without a
 * provider does not degrade — it crashes on capture. Same for sharing a file:
 * the receiving app gets a URI it has no permission to read unless it arrives
 * through a provider with a grant attached.
 *
 * <h2>Scope</h2>
 *
 * Serves only the app's own {@code cache/shared} directory. Nothing else is
 * reachable, which is the entire security model here — no path traversal
 * surface, no configurable roots, no {@code res/xml/file_paths.xml} to get
 * wrong. Anything outside that directory returns null rather than resolving.
 */
public class VytoProvider extends ContentProvider {

    /** Subdirectory of getCacheDir() that this provider will serve. */
    private static final String SHARED_DIR = "shared";

    private static String authorityOf(Context ctx) {
        return ctx.getPackageName() + ".fileprovider";
    }

    private static File sharedRoot(Context ctx) {
        File dir = new File(ctx.getCacheDir(), SHARED_DIR);
        if (!dir.exists() && !dir.mkdirs()) return null;
        return dir;
    }

    /**
     * Allocate a fresh file under the shared root and return its content URI.
     * Used for camera output, where the file must exist as a destination before
     * the intent is launched.
     */
    public static Uri newCaptureUri(Context ctx, String suffix) {
        File root = sharedRoot(ctx);
        if (root == null) return null;
        try {
            File f = File.createTempFile("capture_", suffix, root);
            return new Uri.Builder()
                    .scheme("content")
                    .authority(authorityOf(ctx))
                    .path(f.getName())
                    .build();
        } catch (IOException e) {
            android.util.Log.e("Vyto", "cannot create capture file", e);
            return null;
        }
    }

    /**
     * Resolve a URI to a file, refusing anything that escapes the shared root.
     *
     * <p>The check is on the canonical path, not the raw one: a URI path of
     * {@code ../../databases/app.db} normalises inside {@link File} and would
     * otherwise resolve to a real file outside the root. Comparing canonical
     * prefixes is what actually closes that, and it is the only security-
     * relevant line in this class.
     */
    private File resolve(Uri uri) {
        Context ctx = getContext();
        if (ctx == null) return null;
        File root = sharedRoot(ctx);
        if (root == null) return null;

        String name = uri.getPath();
        if (name == null) return null;
        if (name.startsWith("/")) name = name.substring(1);

        File f = new File(root, name);
        try {
            String rootPath = root.getCanonicalPath() + File.separator;
            String filePath = f.getCanonicalPath();
            if (!filePath.startsWith(rootPath)) {
                android.util.Log.w("Vyto", "provider refused out-of-root path");
                return null;
            }
            return f;
        } catch (IOException e) {
            return null;
        }
    }

    @Override
    public boolean onCreate() { return true; }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode)
            throws java.io.FileNotFoundException {
        File f = resolve(uri);
        if (f == null) throw new java.io.FileNotFoundException("not in shared root");
        int flags = "r".equals(mode)
                ? ParcelFileDescriptor.MODE_READ_ONLY
                : ParcelFileDescriptor.MODE_READ_WRITE | ParcelFileDescriptor.MODE_CREATE;
        return ParcelFileDescriptor.open(f, flags);
    }

    /**
     * Enough of a cursor that a receiving app can show a filename and size.
     * Share sheets and mail clients both query this before accepting an attachment.
     */
    @Override
    public Cursor query(Uri uri, String[] projection, String selection,
                        String[] selectionArgs, String sortOrder) {
        File f = resolve(uri);
        if (f == null) return null;
        String[] cols = {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
        MatrixCursor c = new MatrixCursor(cols, 1);
        c.addRow(new Object[]{f.getName(), f.length()});
        return c;
    }

    @Override
    public String getType(Uri uri) {
        String p = uri.getPath();
        if (p == null) return "application/octet-stream";
        if (p.endsWith(".jpg") || p.endsWith(".jpeg")) return "image/jpeg";
        if (p.endsWith(".png")) return "image/png";
        if (p.endsWith(".mp4")) return "video/mp4";
        return "application/octet-stream";
    }

    // Write access goes through openFile with a "w" mode; nothing needs the
    // row-oriented half of ContentProvider, so these stay unimplemented rather
    // than pretending to work.
    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException("VytoProvider is file-only");
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        File f = resolve(uri);
        if (f == null) return 0;
        return f.delete() ? 1 : 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection,
                      String[] selectionArgs) {
        throw new UnsupportedOperationException("VytoProvider is file-only");
    }
}
