package dev.vyto.android;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.List;
import java.util.Map;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * The platform HTTP client behind {@code vyto/mobile/android/net}.
 *
 * <p>Called from {@code native/src/anet_shim.c} on the Vyto thread, which is a
 * background pthread attached to the JVM — never the UI thread, so blocking
 * here is correct and {@code NetworkOnMainThreadException} cannot fire.
 *
 * <p>STATUS: written, never compiled. Design of record:
 * {@code local/docs/ANDROID.md}.
 *
 * <h2>Why HttpURLConnection</h2>
 *
 * Because it is the connection the platform configures. Trust store, user-added
 * CAs, Network Security Config, cleartext policy, proxy and VPN all apply to it
 * and to nothing an app links itself. A cross-built libcurl would be a second,
 * unmanaged network stack inside an app that the platform still holds
 * responsible for its traffic.
 *
 * <h2>Failure is a status, not an exception</h2>
 *
 * Nothing here throws across JNI. {@code vyto/net/http}'s contract is that a
 * transport failure is soft — status 0, empty body — so every method catches
 * broadly and reports that. The C side treats a pending exception as a VM-level
 * problem precisely because ordinary failures never produce one.
 */
public final class Http {

    private Http() {}

    private static final byte[] EMPTY = new byte[0];

    /** A finished response. Fields are read directly by JNI — do not rename. */
    public static final class Resp {
        public int status;
        public String headers = "";
        public byte[] body = EMPTY;
    }

    /** A connection whose body is still being read. Fields read by JNI. */
    public static final class Conn {
        public int status;
        public String headers = "";

        private final HttpURLConnection c;
        private final InputStream in;

        Conn(HttpURLConnection c, InputStream in, int status, String headers) {
            this.c = c;
            this.in = in;
            this.status = status;
            this.headers = headers;
        }

        /** Bytes into buf, or -1 at end of stream or on error. */
        public int read(byte[] buf) {
            if (in == null) return -1;
            try {
                return in.read(buf);
            } catch (Throwable t) {
                return -1;
            }
        }

        public void close() {
            try {
                if (in != null) in.close();
            } catch (Throwable ignored) {
            }
            try {
                if (c != null) c.disconnect();
            } catch (Throwable ignored) {
            }
        }
    }

    // ------------------------------------------------------------- plumbing

    /**
     * Flatten the response headers into the "Key: Value\n" block the Vyto side
     * parses. The status line comes back under a null key, which is not a
     * header and would parse as garbage, so it is dropped.
     */
    private static String headerBlock(HttpURLConnection c) {
        StringBuilder sb = new StringBuilder();
        try {
            for (Map.Entry<String, List<String>> e : c.getHeaderFields().entrySet()) {
                String k = e.getKey();
                if (k == null) continue;
                for (String v : e.getValue()) {
                    sb.append(k).append(": ").append(v).append('\n');
                }
            }
        } catch (Throwable ignored) {
        }
        return sb.toString();
    }

    /** Apply a "Key: Value\n" block to the request. Blank lines are skipped. */
    private static void applyHeaders(HttpURLConnection c, String headerLines) {
        if (headerLines == null || headerLines.isEmpty()) return;
        for (String line : headerLines.split("\n")) {
            int i = line.indexOf(':');
            if (i <= 0) continue;
            c.setRequestProperty(line.substring(0, i).trim(),
                                 line.substring(i + 1).trim());
        }
    }

    private static HttpURLConnection begin(String method, String url, String headerLines,
                                           byte[] body, int timeoutMs) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setRequestMethod(method == null || method.isEmpty() ? "GET" : method);
        applyHeaders(c, headerLines);
        // 0 means "no timeout" on both sides, so this passes straight through.
        c.setConnectTimeout(timeoutMs);
        c.setReadTimeout(timeoutMs);
        c.setInstanceFollowRedirects(true);
        if (body != null && body.length > 0) {
            c.setDoOutput(true);
            // Without this the whole body is buffered in memory before sending,
            // which defeats uploading anything large.
            c.setFixedLengthStreamingMode(body.length);
            OutputStream os = c.getOutputStream();
            try {
                os.write(body);
                os.flush();
            } finally {
                os.close();
            }
        }
        return c;
    }

    /**
     * The response body stream. A 4xx/5xx has its body on the error stream, not
     * the input stream, and getInputStream() throws for those — so an HTTP error
     * with a JSON payload only reaches the caller through this branch.
     */
    private static InputStream bodyStream(HttpURLConnection c) {
        try {
            return c.getInputStream();
        } catch (Throwable t) {
            return c.getErrorStream();
        }
    }

    private static byte[] drain(InputStream in) throws IOException {
        if (in == null) return EMPTY;
        ByteArrayOutputStream out = new ByteArrayOutputStream(8192);
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) > 0) {
            out.write(buf, 0, n);
        }
        return out.toByteArray();
    }

    // -------------------------------------------------------------- blocking

    /** One request, body buffered whole. Never throws. */
    public static Resp perform(String method, String url, String headerLines,
                               byte[] body, int timeoutMs) {
        Resp r = new Resp();
        HttpURLConnection c = null;
        try {
            c = begin(method, url, headerLines, body, timeoutMs);
            r.status = c.getResponseCode();
            r.headers = headerBlock(c);
            InputStream in = bodyStream(c);
            try {
                r.body = drain(in);
            } finally {
                if (in != null) in.close();
            }
        } catch (Throwable t) {
            // Soft failure: status stays 0 and the body stays empty, which is
            // exactly what vyto/net/http reports for a dead host or a TLS
            // rejection. The log line is the only trace, so keep it.
            android.util.Log.w("Vyto", "http " + method + " " + url + " failed: " + t);
        } finally {
            if (c != null) {
                try {
                    c.disconnect();
                } catch (Throwable ignored) {
                }
            }
        }
        return r;
    }

    /** One request, body left on the connection for incremental reads. */
    public static Conn open(String method, String url, String headerLines,
                            byte[] body, int timeoutMs) {
        HttpURLConnection c = null;
        try {
            c = begin(method, url, headerLines, body, timeoutMs);
            int status = c.getResponseCode();
            String headers = headerBlock(c);
            return new Conn(c, bodyStream(c), status, headers);
        } catch (Throwable t) {
            android.util.Log.w("Vyto", "http stream " + method + " " + url + " failed: " + t);
            if (c != null) {
                try {
                    c.disconnect();
                } catch (Throwable ignored) {
                }
            }
            return null;
        }
    }

    // ------------------------------------------------------------------ pool

    /**
     * Concurrency for a single-threaded caller. Vyto has no threads, so the
     * parallelism lives here: requests run on a small executor and completions
     * are handed back one id at a time, in finish order.
     */
    public static final class Pool {
        private final ExecutorService exec;
        private final BlockingQueue<Integer> done = new LinkedBlockingQueue<>();
        private final ConcurrentHashMap<Integer, Resp> results = new ConcurrentHashMap<>();
        private final AtomicInteger nextId = new AtomicInteger(0);

        Pool(int parallel) {
            this.exec = Executors.newFixedThreadPool(parallel < 1 ? 1 : parallel);
        }

        public int add(final String method, final String url, final String headerLines,
                       final byte[] body, final int timeoutMs) {
            final int id = nextId.getAndIncrement();
            try {
                exec.execute(new Runnable() {
                    @Override public void run() {
                        // perform never throws, so a worker cannot die and
                        // strand an id the caller is still waiting on.
                        results.put(id, perform(method, url, headerLines, body, timeoutMs));
                        done.add(id);
                    }
                });
            } catch (Throwable t) {
                android.util.Log.w("Vyto", "http pool rejected " + url + ": " + t);
                return -1;
            }
            return id;
        }

        /** Id of the next finished request, or -1 if none within timeoutMs. */
        public int next(int timeoutMs) {
            try {
                Integer id = timeoutMs <= 0
                        ? done.poll()
                        : done.poll(timeoutMs, TimeUnit.MILLISECONDS);
                return id == null ? -1 : id;
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return -1;
            }
        }

        /** The response for id, removing it. Null if unknown or already taken. */
        public Resp take(int id) {
            return results.remove(id);
        }

        public void shutdown() {
            try {
                exec.shutdownNow();
            } catch (Throwable ignored) {
            }
            results.clear();
            done.clear();
        }
    }

    public static Pool poolNew(int parallel) {
        return new Pool(parallel);
    }
}
