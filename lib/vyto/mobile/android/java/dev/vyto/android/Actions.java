package dev.vyto.android;

import android.app.Activity;
import android.content.ClipData;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.ParcelFileDescriptor;
import android.provider.MediaStore;
import android.provider.OpenableColumns;
import android.provider.Settings;

import java.io.File;
import java.util.ArrayList;

/**
 * Java half of {@code lib/vyto/mobile/android/actions.vt}.
 *
 * <p>Launches intents and permission requests, then hands results back to the
 * native side via {@link Native#actionResult}. Nothing here calls into Vyto
 * code directly — results are queued natively and drained by
 * {@code Actions.pump()} on the Vyto thread, because everything in this file
 * runs on the UI thread.
 *
 * <p>STATUS: written, never compiled. The native queue is
 * {@code native/src/aintent_shim.c}.
 * Design of record: {@code local/docs/ANDROID.md}.
 *
 * <h2>content:// is not a path</h2>
 *
 * Every picker returns a {@code content://} URI. It cannot be opened with
 * {@code fopen}, so {@code vyto/io} and the asset VFS will both fail on it.
 * {@link #openFd} is the only bridge: it asks {@link ContentResolver} for a
 * {@link ParcelFileDescriptor} and detaches a real POSIX fd. This is the most
 * common way an Android file flow breaks.
 */
public class Actions {

    private final Activity activity;

    /**
     * Request codes are the ids Vyto allocated, offset into a range that will
     * not collide with anything else the Activity does. Vyto's ids start at 1.
     */
    private static final int REQ_BASE = 0x5600;

    public Actions(Activity a) { this.activity = a; }

    public void dispose() { /* nothing retained; here for symmetry with Vyto */ }

    private static int codeFor(int vytoId) { return REQ_BASE + (vytoId & 0xFFF); }
    private static int idFor(int requestCode) { return requestCode - REQ_BASE; }

    // ------------------------------------------------------------- launchers
    // Each returns 0 on launch, or -1 when no Activity can handle the intent.
    // A launch failure is reported synchronously because the callback will
    // never fire — actions.vt turns it into a cancelled result so callers have
    // exactly one path.

    /**
     * Every launcher here is called from the <b>Vyto thread</b>, but
     * {@code startActivityForResult} and {@code requestPermissions} must run on
     * the UI thread. The split below is what makes both true at once:
     *
     * <ul>
     *   <li>{@code resolveActivity} runs synchronously on the calling thread —
     *       PackageManager is thread-safe — so a "nothing can handle this"
     *       answer still comes back as a return value, which is what
     *       {@code actions.vt}'s {@code launched()} contract needs.
     *   <li>the actual start is posted, so the result arrives later through
     *       {@link #onActivityResult} exactly as it would have anyway.
     * </ul>
     */
    private int launch(final Intent intent, int vytoId) {
        if (intent.resolveActivity(activity.getPackageManager()) == null) {
            // On targetSdk 30+ this is usually a missing <queries> entry rather
            // than a genuinely absent app. manifest.vt derives those from
            // uses(ACT_*); a null here with the app installed means the
            // declaration was skipped.
            android.util.Log.w("Vyto", "no Activity for " + intent.getAction()
                    + " — check <queries> in the manifest");
            return -1;
        }
        final int code = codeFor(vytoId);
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                try {
                    activity.startActivityForResult(intent, code);
                } catch (Exception e) {
                    android.util.Log.e("Vyto", "startActivityForResult failed", e);
                    // The callback would otherwise never fire and the Vyto-side
                    // entry would leak, so report a cancel.
                    if (Native.isLoaded()) {
                        Native.actionResult(idFor(code), false, new String[0]);
                    }
                }
            }
        });
        return 0;
    }

    public int pickDocument(String mime, boolean multi, int vytoId) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType(mime == null || mime.isEmpty() ? "*/*" : mime);
        i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, multi);
        // Ask for a grant that survives a reboot; take_persistable() in
        // actions.vt is what actually holds onto it.
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        return launch(i, vytoId);
    }

    public int createDocument(String mime, String name, int vytoId) {
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType(mime == null || mime.isEmpty() ? "*/*" : mime);
        if (name != null && !name.isEmpty()) i.putExtra(Intent.EXTRA_TITLE, name);
        return launch(i, vytoId);
    }

    public int pickDirectory(int vytoId) {
        return launch(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), vytoId);
    }

    public int pickMedia(String mime, boolean multi, int vytoId) {
        // The system photo picker needs no permission and has a media-shaped
        // UI, so it is preferred over SAF for images. Falls back to SAF on
        // devices without it.
        if (Build.VERSION.SDK_INT >= 33) {
            Intent i = new Intent(MediaStore.ACTION_PICK_IMAGES);
            i.setType(mime == null || mime.isEmpty() ? "image/*" : mime);
            if (multi) i.putExtra(MediaStore.EXTRA_PICK_IMAGES_MAX, 50);
            return launch(i, vytoId);
        }
        return pickDocument(mime == null || mime.isEmpty() ? "image/*" : mime,
                multi, vytoId);
    }

    public int captureImage(int vytoId) {
        Intent i = new Intent(MediaStore.ACTION_IMAGE_CAPTURE);
        Uri out = VytoProvider.newCaptureUri(activity, ".jpg");
        if (out == null) return -1;
        i.putExtra(MediaStore.EXTRA_OUTPUT, out);
        i.addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        pendingCapture.put(codeFor(vytoId), out);
        return launch(i, vytoId);
    }

    public int captureVideo(int vytoId) {
        Intent i = new Intent(MediaStore.ACTION_VIDEO_CAPTURE);
        Uri out = VytoProvider.newCaptureUri(activity, ".mp4");
        if (out == null) return -1;
        i.putExtra(MediaStore.EXTRA_OUTPUT, out);
        i.addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        pendingCapture.put(codeFor(vytoId), out);
        return launch(i, vytoId);
    }

    /**
     * ACTION_IMAGE_CAPTURE writes to the URI we supplied and returns a null
     * data Intent, so the output URI has to be remembered across the round trip.
     */
    private final android.util.SparseArray<Uri> pendingCapture =
            new android.util.SparseArray<>();

    // ----------------------------------------------------- fire and forget

    public int shareText(String text, String title) {
        Intent i = new Intent(Intent.ACTION_SEND);
        i.setType("text/plain");
        i.putExtra(Intent.EXTRA_TEXT, text);
        return startChooser(i, title);
    }

    public int shareUri(String uri, String mime, String title) {
        Intent i = new Intent(Intent.ACTION_SEND);
        i.setType(mime == null || mime.isEmpty() ? "*/*" : mime);
        i.putExtra(Intent.EXTRA_STREAM, Uri.parse(uri));
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        return startChooser(i, title);
    }

    private int startChooser(Intent i, String title) {
        return post(Intent.createChooser(i, title));
    }

    public int viewUrl(String url) {
        return fireAndForget(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
    }

    public int dial(String number) {
        return fireAndForget(new Intent(Intent.ACTION_DIAL, Uri.parse("tel:" + number)));
    }

    public int sendEmail(String to, String subject, String body) {
        Intent i = new Intent(Intent.ACTION_SENDTO, Uri.parse("mailto:" + to));
        i.putExtra(Intent.EXTRA_SUBJECT, subject);
        i.putExtra(Intent.EXTRA_TEXT, body);
        return fireAndForget(i);
    }

    public int openAppSettings() {
        Intent i = new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                Uri.parse("package:" + activity.getPackageName()));
        return fireAndForget(i);
    }

    private int fireAndForget(Intent i) {
        if (i.resolveActivity(activity.getPackageManager()) == null) {
            android.util.Log.w("Vyto", "no Activity for " + i.getAction()
                    + " — check <queries> in the manifest");
            return -1;
        }
        return post(i);
    }

    /** Start on the UI thread; no result, so failure is logged, not reported. */
    private int post(final Intent i) {
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                try { activity.startActivity(i); }
                catch (Exception e) { android.util.Log.e("Vyto", "startActivity failed", e); }
            }
        });
        return 0;
    }

    // ----------------------------------------------------------- permissions

    public boolean hasPermission(String name) {
        return activity.checkSelfPermission(name) == PackageManager.PERMISSION_GRANTED;
    }

    public boolean shouldExplain(String name) {
        return activity.shouldShowRequestPermissionRationale(name);
    }

    /** Must run on the UI thread; requestPermissions throws otherwise. */
    public int requestPermission(final String name, int vytoId) {
        final int code = codeFor(vytoId);
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                try {
                    activity.requestPermissions(new String[]{name}, code);
                } catch (Exception e) {
                    android.util.Log.e("Vyto", "requestPermissions failed", e);
                    if (Native.isLoaded()) {
                        Native.actionResult(idFor(code), false, new String[0]);
                    }
                }
            }
        });
        return 0;
    }

    // --------------------------------------------------------------- results

    public void onActivityResult(int requestCode, int resultCode, Intent data) {
        int id = idFor(requestCode);
        if (id <= 0) return;
        boolean ok = resultCode == Activity.RESULT_OK;

        ArrayList<String> uris = new ArrayList<>();
        Uri captured = pendingCapture.get(requestCode);
        pendingCapture.remove(requestCode);

        if (ok) {
            if (captured != null) {
                // Camera wrote to our own URI; data is null by contract.
                uris.add(captured.toString());
            } else if (data != null) {
                ClipData clip = data.getClipData();
                if (clip != null) {
                    for (int i = 0; i < clip.getItemCount(); i++) {
                        Uri u = clip.getItemAt(i).getUri();
                        if (u != null) uris.add(u.toString());
                    }
                } else if (data.getData() != null) {
                    uris.add(data.getData().toString());
                }
            }
        }

        if (Native.isLoaded()) {
            Native.actionResult(id, ok, uris.toArray(new String[0]));
        }
    }

    public void onPermissionResult(int requestCode, int[] grants) {
        int id = idFor(requestCode);
        if (id <= 0) return;
        boolean ok = grants.length > 0 && grants[0] == PackageManager.PERMISSION_GRANTED;
        if (Native.isLoaded()) Native.actionResult(id, ok, new String[0]);
    }

    // ------------------------------------------------------ content:// bridge

    /**
     * A real POSIX fd for a content URI, or -1. The caller owns it and must
     * close it — {@code detachFd} hands ownership over, so the
     * ParcelFileDescriptor is deliberately not closed here.
     *
     * @param mode "r" or "w"
     */
    public int openFd(String uri, String mode) {
        try {
            ContentResolver cr = activity.getContentResolver();
            ParcelFileDescriptor pfd = cr.openFileDescriptor(Uri.parse(uri), mode);
            if (pfd == null) return -1;
            return pfd.detachFd();
        } catch (Exception e) {
            android.util.Log.w("Vyto", "openFd(" + uri + ") failed", e);
            return -1;
        }
    }

    public String displayName(String uri) {
        try {
            Cursor c = activity.getContentResolver().query(
                    Uri.parse(uri), null, null, null, null);
            if (c == null) return "";
            try {
                int idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (idx >= 0 && c.moveToFirst()) return c.getString(idx);
            } finally {
                c.close();
            }
        } catch (Exception e) {
            // fall through
        }
        return "";
    }

    public long sizeOf(String uri) {
        try {
            Cursor c = activity.getContentResolver().query(
                    Uri.parse(uri), null, null, null, null);
            if (c == null) return -1;
            try {
                int idx = c.getColumnIndex(OpenableColumns.SIZE);
                if (idx >= 0 && c.moveToFirst()) return c.getLong(idx);
            } finally {
                c.close();
            }
        } catch (Exception e) {
            // fall through
        }
        return -1;
    }

    /** Hold a SAF grant across reboots; without this a "recent files" list breaks. */
    public boolean takePersistable(String uri) {
        try {
            activity.getContentResolver().takePersistableUriPermission(
                    Uri.parse(uri), Intent.FLAG_GRANT_READ_URI_PERMISSION);
            return true;
        } catch (Exception e) {
            return false;
        }
    }
}
