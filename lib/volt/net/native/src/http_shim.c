/* volt/net/http native backing — a full request over libcurl.

   Same opaque-handle + explicit-free shape as net_shim.c: http_request returns
   an HttpResp owned by a Volt Response whose deinit calls http_free exactly
   once. Response body and header block are captured into growable buffers;
   the Volt side copies out (str / http_body_copy) so nothing aliases the
   malloc'd memory past the free. */

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *body; size_t blen, bcap;
    char *hdr;  size_t hlen, hcap;
    long status;
} HttpResp;

static int grow(char **buf, size_t *len, size_t *cap, const char *p, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t nc = *cap ? *cap : 8192;
        while (nc < *len + n + 1) nc *= 2;
        char *nb = (char *)realloc(*buf, nc);
        if (!nb) return 0;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, p, n);
    *len += n;
    (*buf)[*len] = 0;
    return 1;
}

static size_t body_cb(void *ptr, size_t sz, size_t nm, void *ud) {
    HttpResp *r = (HttpResp *)ud;
    size_t n = sz * nm;
    return grow(&r->body, &r->blen, &r->bcap, (const char *)ptr, n) ? n : 0;
}
static size_t hdr_cb(void *ptr, size_t sz, size_t nm, void *ud) {
    HttpResp *r = (HttpResp *)ud;
    size_t n = sz * nm;
    grow(&r->hdr, &r->hlen, &r->hcap, (const char *)ptr, n); /* best-effort */
    return n;
}

/* header_lines: newline-separated "Key: Value" entries (may be empty).
   body/body_len: request body (may be NULL/0). timeout_ms: 0 => defaults. */
HttpResp *http_perform(const char *method, const char *url, const char *header_lines,
                       const char *body, long body_len, long timeout_ms) {
    static int inited = 0;
    if (!inited) { curl_global_init(CURL_GLOBAL_DEFAULT); inited = 1; }

    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    HttpResp *r = (HttpResp *)calloc(1, sizeof *r);
    if (!r) { curl_easy_cleanup(curl); return NULL; }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, r);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, hdr_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, r);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "volt-http/0.1");
    if (timeout_ms > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    } else {
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    }

    if (method && *method && strcmp(method, "GET") != 0)
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);

    if (body && body_len > 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body_len);
    } else if (method && strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
    }

    struct curl_slist *hl = NULL;
    if (header_lines && *header_lines) {
        const char *s = header_lines;
        while (*s) {
            const char *nl = strchr(s, '\n');
            size_t linelen = nl ? (size_t)(nl - s) : strlen(s);
            if (linelen > 0) {
                char *line = (char *)malloc(linelen + 1);
                if (line) {
                    memcpy(line, s, linelen);
                    line[linelen] = 0;
                    hl = curl_slist_append(hl, line);
                    free(line);
                }
            }
            if (!nl) break;
            s = nl + 1;
        }
        if (hl) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hl);
    }

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r->status);
    else
        r->status = 0; /* transport failure — soft, status 0 */

    if (hl) curl_slist_free_all(hl);
    curl_easy_cleanup(curl);
    return r;
}

long http_status(HttpResp *r) { return r ? r->status : 0; }
const char *http_body_data(HttpResp *r) { return (r && r->body) ? r->body : ""; }
long http_body_len(HttpResp *r) { return r ? (long)r->blen : 0; }
const char *http_headers(HttpResp *r) { return (r && r->hdr) ? r->hdr : ""; }

/* Binary-safe copy of up to `cap` body bytes into a Volt buffer; returns the
   number copied. */
long http_body_copy(HttpResp *r, char *out, long cap) {
    if (!r || !r->body || cap <= 0) return 0;
    long n = (long)r->blen;
    if (n > cap) n = cap;
    memcpy(out, r->body, (size_t)n);
    return n;
}

void http_free(HttpResp *r) {
    if (!r) return;
    free(r->body);
    free(r->hdr);
    free(r);
}

/* ---- streaming ----------------------------------------------------------

   Instead of buffering the whole body, invoke `sink(ud, chunk, len)` for each
   piece libcurl delivers. `sink`/`ud` come from the Volt side as
   cthunk(closure) + (closure as rawptr): cthunk prepends the userdata, so the
   pointer matches `chunk_sink` exactly. Returns the HTTP status (0 on error).
   No total-time cap by default (streams can be long-lived, e.g. SSE); only a
   connect timeout, plus an optional total cap when timeout_ms > 0. */

typedef void (*chunk_sink)(void *ud, const char *data, int len);

typedef struct {
    chunk_sink sink;
    void *ud;
} StreamCtx;

static size_t stream_write_cb(void *ptr, size_t sz, size_t nm, void *ud) {
    StreamCtx *c = (StreamCtx *)ud;
    size_t n = sz * nm;
    if (c->sink && n > 0) c->sink(c->ud, (const char *)ptr, (int)n);
    return n; /* always consume everything so curl keeps going */
}

long http_stream(const char *method, const char *url, const char *header_lines,
                 const char *body, long body_len, long timeout_ms,
                 chunk_sink sink, void *sink_ud) {
    static int inited = 0;
    if (!inited) { curl_global_init(CURL_GLOBAL_DEFAULT); inited = 1; }

    CURL *curl = curl_easy_init();
    if (!curl) return 0;

    StreamCtx ctx;
    ctx.sink = sink;
    ctx.ud = sink_ud;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "volt-http/0.1");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    if (timeout_ms > 0) curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);

    if (method && *method && strcmp(method, "GET") != 0)
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    if (body && body_len > 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body_len);
    } else if (method && strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
    }

    struct curl_slist *hl = NULL;
    if (header_lines && *header_lines) {
        const char *s = header_lines;
        while (*s) {
            const char *nl = strchr(s, '\n');
            size_t linelen = nl ? (size_t)(nl - s) : strlen(s);
            if (linelen > 0) {
                char *line = (char *)malloc(linelen + 1);
                if (line) {
                    memcpy(line, s, linelen);
                    line[linelen] = 0;
                    hl = curl_slist_append(hl, line);
                    free(line);
                }
            }
            if (!nl) break;
            s = nl + 1;
        }
        if (hl) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hl);
    }

    long status = 0;
    if (curl_easy_perform(curl) == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    if (hl) curl_slist_free_all(hl);
    curl_easy_cleanup(curl);
    return status;
}

/* Binary-safe copy of a streamed chunk into a Volt buffer. */
void http_chunk_copy(const char *src, char *dst, int len) {
    if (src && dst && len > 0) memcpy(dst, src, (size_t)len);
}
