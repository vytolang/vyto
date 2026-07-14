/* vyto/net/http native backing — a full request over libcurl.

   Same opaque-handle + explicit-free shape as net_shim.c: http_request returns
   an HttpResp owned by a Vyto Response whose deinit calls http_free exactly
   once. Response body and header block are captured into growable buffers;
   the Vyto side copies out (str / http_body_copy) so nothing aliases the
   malloc'd memory past the free. */

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *body; size_t blen, bcap;
    char *hdr;  size_t hlen, hcap;
    long status;
    struct curl_slist *hl;  /* request headers, freed in http_free */
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
    /* FOLLOWLOCATION delivers every hop's header block through this callback.
       A status line starts each block — reset so only the FINAL response's
       headers are kept (a redirect's stale values must not shadow them). */
    if (n >= 5 && strncmp((const char *)ptr, "HTTP/", 5) == 0) {
        r->hlen = 0;
        if (r->hdr) r->hdr[0] = 0;
    }
    grow(&r->hdr, &r->hlen, &r->hcap, (const char *)ptr, n); /* best-effort */
    return n;
}

/* Build a curl_slist from newline-separated "Key: Value" lines (NULL if empty). */
static struct curl_slist *build_headers(const char *header_lines) {
    struct curl_slist *hl = NULL;
    if (!header_lines || !*header_lines) return NULL;
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
    return hl;
}

static void ensure_global_init(void) {
    static int inited = 0;
    if (!inited) { curl_global_init(CURL_GLOBAL_DEFAULT); inited = 1; }
}

/* Configure a fresh easy handle to write into `r`. Stashes the header slist in
   r->hl so it outlives the transfer (needed for the multi path) and is freed by
   http_free. */
static void configure_easy(CURL *curl, const char *method, const char *url,
                           const char *header_lines, const char *body,
                           long body_len, long timeout_ms, HttpResp *r) {
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, r);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, hdr_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, r);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "vyto-http/0.1");
    if (timeout_ms > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    } else {
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    }

    if (method && *method && strcmp(method, "GET") != 0)
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);

    if (body && body_len > 0) {
        /* COPYPOSTFIELDS (size set first, per curl docs): POSTFIELDS is the
           one string option curl does NOT copy, and on the multi path the
           Vyto Request (and its body string) may be freed long before the
           transfer runs — a use-after-free with plain POSTFIELDS. */
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body_len);
        curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, body);
    } else if (method && strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
    }

    r->hl = build_headers(header_lines);
    if (r->hl) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, r->hl);
}

/* header_lines: newline-separated "Key: Value" entries (may be empty).
   body/body_len: request body (may be NULL/0). timeout_ms: 0 => defaults. */
HttpResp *http_perform(const char *method, const char *url, const char *header_lines,
                       const char *body, long body_len, long timeout_ms) {
    ensure_global_init();

    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    HttpResp *r = (HttpResp *)calloc(1, sizeof *r);
    if (!r) { curl_easy_cleanup(curl); return NULL; }

    configure_easy(curl, method, url, header_lines, body, body_len, timeout_ms, r);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r->status);
    else
        r->status = 0; /* transport failure — soft, status 0 */

    curl_easy_cleanup(curl);
    return r;
}

long http_status(HttpResp *r) { return r ? r->status : 0; }
const char *http_body_data(HttpResp *r) { return (r && r->body) ? r->body : ""; }
long http_body_len(HttpResp *r) { return r ? (long)r->blen : 0; }
const char *http_headers(HttpResp *r) { return (r && r->hdr) ? r->hdr : ""; }

/* Binary-safe copy of up to `cap` body bytes into a Vyto buffer; returns the
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
    if (r->hl) curl_slist_free_all(r->hl);
    free(r->body);
    free(r->hdr);
    free(r);
}

/* ---- streaming ----------------------------------------------------------

   Instead of buffering the whole body, invoke `sink(ud, chunk, len)` for each
   piece libcurl delivers. `sink`/`ud` come from the Vyto side as
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
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "vyto-http/0.1");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    if (timeout_ms > 0) curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);

    if (method && *method && strcmp(method, "GET") != 0)
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    if (body && body_len > 0) {
        /* copy the body (size first) — see configure_easy */
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body_len);
        curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, body);
    } else if (method && strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
    }

    struct curl_slist *hl = build_headers(header_lines);
    if (hl) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hl);

    long status = 0;
    if (curl_easy_perform(curl) == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    if (hl) curl_slist_free_all(hl);
    curl_easy_cleanup(curl);
    return status;
}

/* Binary-safe copy of a streamed chunk into a Vyto buffer. */
void http_chunk_copy(const char *src, char *dst, int len) {
    if (src && dst && len > 0) memcpy(dst, src, (size_t)len);
}

/* ---- concurrent client (curl_multi) -------------------------------------

   Drive N requests on one thread. http_multi_add queues an easy handle whose
   HttpResp is stashed via CURLOPT_PRIVATE; http_multi_perform pumps them all
   (curl_multi_poll blocks up to timeout_ms waiting on every handle at once);
   http_multi_next_done pops each finished transfer, detaches its easy handle,
   and hands the HttpResp to Vyto (whose Response.deinit frees it). */

typedef struct {
    CURLM *m;
    CURL **handles;   /* live easy handles, for teardown */
    int n, cap;
} HttpMulti;

HttpMulti *http_multi_new(void) {
    ensure_global_init();
    HttpMulti *hm = (HttpMulti *)calloc(1, sizeof *hm);
    if (!hm) return NULL;
    hm->m = curl_multi_init();
    if (!hm->m) { free(hm); return NULL; }
    return hm;
}

static void multi_track(HttpMulti *hm, CURL *curl) {
    if (hm->n == hm->cap) {
        int want = hm->cap ? hm->cap * 2 : 8;
        CURL **nh = (CURL **)realloc(hm->handles, (size_t)want * sizeof *nh);
        if (!nh) return; /* on OOM we simply don't track it (teardown may leak it) */
        hm->handles = nh; hm->cap = want;
    }
    hm->handles[hm->n++] = curl;
}

static void multi_untrack(HttpMulti *hm, CURL *curl) {
    for (int i = 0; i < hm->n; i++) {
        if (hm->handles[i] == curl) {
            hm->handles[i] = hm->handles[hm->n - 1];
            hm->n--;
            return;
        }
    }
}

/* Queue a request; returns the HttpResp the result will land in (NULL on OOM).
   Owned by Vyto: freed by http_free once the transfer is popped. */
HttpResp *http_multi_add(HttpMulti *hm, const char *method, const char *url,
                         const char *header_lines, const char *body,
                         long body_len, long timeout_ms) {
    if (!hm) return NULL;
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    HttpResp *r = (HttpResp *)calloc(1, sizeof *r);
    if (!r) { curl_easy_cleanup(curl); return NULL; }
    configure_easy(curl, method, url, header_lines, body, body_len, timeout_ms, r);
    curl_easy_setopt(curl, CURLOPT_PRIVATE, r);
    curl_multi_add_handle(hm->m, curl);
    multi_track(hm, curl);
    return r;
}

/* Pump all queued transfers, blocking up to timeout_ms for activity. Returns
   the number still running (>=0), -1 on error. */
int http_multi_perform(HttpMulti *hm, int timeout_ms) {
    if (!hm) return -1;
    int running = 0;
    if (curl_multi_perform(hm->m, &running) != CURLM_OK) return -1;
    if (running > 0) {
        /* wait for activity so the caller loop doesn't spin */
        if (curl_multi_poll(hm->m, NULL, 0, timeout_ms, NULL) != CURLM_OK) return -1;
    }
    return running;
}

/* Pop the next completed transfer, or NULL if none finished yet. Fills status,
   detaches and frees the easy handle (leaving the HttpResp for Vyto). */
HttpResp *http_multi_next_done(HttpMulti *hm) {
    if (!hm) return NULL;
    int msgs = 0;
    CURLMsg *msg;
    while ((msg = curl_multi_info_read(hm->m, &msgs))) {
        if (msg->msg != CURLMSG_DONE) continue;
        CURL *curl = msg->easy_handle;
        HttpResp *r = NULL;
        curl_easy_getinfo(curl, CURLINFO_PRIVATE, &r);
        if (r) {
            if (msg->data.result == CURLE_OK)
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r->status);
            else
                r->status = 0; /* transport failure — soft, status 0 */
        }
        curl_multi_remove_handle(hm->m, curl);
        multi_untrack(hm, curl);
        curl_easy_cleanup(curl);
        return r;
    }
    return NULL;
}

/* Free the multi handle and any easy handles still attached. Does NOT free the
   HttpResps — those belong to Vyto Response objects. */
void http_multi_free(HttpMulti *hm) {
    if (!hm) return;
    /* tear down any easy handles never popped (still-running or done). Their
       HttpResps are owned by Vyto and are freed separately by http_free. */
    for (int i = 0; i < hm->n; i++) {
        curl_multi_remove_handle(hm->m, hm->handles[i]);
        curl_easy_cleanup(hm->handles[i]);
    }
    free(hm->handles);
    curl_multi_cleanup(hm->m);
    free(hm);
}
