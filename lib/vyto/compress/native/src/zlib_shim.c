#include "zlib_shim.h"

#ifdef VT_NO_LIBC

/* --freestanding arm. zlib is built on malloc, so there is nothing to degrade
 * to: vzlib_*.c compile to nothing and these stubs answer instead. Every symbol
 * is kept so a program that merely imports vyto/compress still links, and each
 * fails exactly as an out-of-memory would.
 *
 * Deliberately no #include at all, not even <stddef.h>, so this arm cannot
 * regress into depending on a hosted header — the rule
 * lib/vyto/media/image/native/src/image_shim.c:27-28 states. */

void *vzl_deflate(const void *s, int n, int f, int l)
                            { (void)s; (void)n; (void)f; (void)l; return 0; }
void *vzl_inflate(const void *s, int n, int f, int m)
                            { (void)s; (void)n; (void)f; (void)m; return 0; }
void *vzl_stream_new(int c, int f, int l)  { (void)c; (void)f; (void)l; return 0; }
int   vzl_stream_push(void *s, const void *b, int n)
                            { (void)s; (void)b; (void)n; return -1; }
int   vzl_stream_finish(void *s)           { (void)s; return -1; }
int   vzl_stream_pending(void *s)          { (void)s; return 0; }
int   vzl_stream_take(void *s, void *d, int c) { (void)s; (void)d; (void)c; return 0; }
void  vzl_stream_free(void *s)             { (void)s; }
int   vzl_buf_len(void *h)                 { (void)h; return 0; }
int   vzl_buf_take(void *h, void *d, int c) { (void)h; (void)d; (void)c; return 0; }
void  vzl_buf_free(void *h)                { (void)h; }
const char *vzl_last_error(void)  { return "vyto/compress: unavailable in a freestanding build"; }
int   vzl_available(void)                  { return 0; }
const char *vzl_version(void)              { return "unavailable"; }

#else

#include <stdlib.h>
#include <string.h>
#include "zlib_config.h"
#include "zlib/zlib.h"

/* zlib's next_in is Bytef* rather than const Bytef*, a pre-const API that
 * deflate() and inflate() never actually write through. The cast below drops
 * const deliberately; it is the same one zlib's own compress2() makes.
 *
 * ---- the allocator, and why it is not Z_NULL ----
 *
 * Z_SOLO compiles out zlib's own zcalloc/zcfree, so a NULL zalloc is a hard
 * error rather than a request for the default: deflateInit2_ returns
 * Z_STREAM_ERROR on it (deflate.c:393-396). That is the point of Z_SOLO — it
 * makes no allocator assumption and hands the choice to the caller. So every
 * z_stream here gets zl_alloc/zl_free below.
 *
 * They wrap malloc/free rather than the runtime's pool. A package shim cannot
 * reach vt_host_alloc: it is static in hosted builds (runtime/vyto_rt.c:61),
 * extern only under VT_NO_LIBC, and this file has no -I to runtime/ regardless.
 * Every other hosted shim arm in the tree calls malloc directly for the same
 * reason. */

static voidpf zl_alloc(voidpf opaque, uInt items, uInt size) {
    (void)opaque;
    /* zlib asks for items*size; the multiply is guarded because the operands
     * come from window and hash sizes rather than from anything user-supplied,
     * but a wrapped allocation would be a heap overflow rather than a failure. */
    if (size != 0 && items > (uInt)-1 / size) return Z_NULL;
    return (voidpf)malloc((size_t)items * (size_t)size);
}

static void zl_free(voidpf opaque, voidpf address) {
    (void)opaque;
    free(address);
}

/* Point a freshly zeroed z_stream at our allocator. Every init path uses this,
 * so there is one place where the Z_SOLO contract is honoured. */
static void zl_set_alloc(z_streamp zs) {
    zs->zalloc = zl_alloc;
    zs->zfree  = zl_free;
    zs->opaque = Z_NULL;
}

/* One growable output buffer. Doubling growth; the caller copies out and frees
 * through vzl_buf_take. */
typedef struct {
    unsigned char *p;
    int len;
    int cap;
} Buf;

static __thread char g_err[256];

static void set_err(const char *msg) {
    if (!msg) msg = "unknown error";
    /* Bounded copy; g_err is never read as anything but a C string. */
    size_t n = strlen(msg);
    if (n >= sizeof g_err) n = sizeof g_err - 1;
    memcpy(g_err, msg, n);
    g_err[n] = 0;
}

static void set_zerr(const char *what, int rc, z_streamp zs) {
    const char *detail = (zs && zs->msg) ? zs->msg : Z_NULL;
    if (!detail) {
        switch (rc) {
            case Z_MEM_ERROR:   detail = "out of memory";        break;
            case Z_DATA_ERROR:  detail = "corrupt or truncated";  break;
            case Z_BUF_ERROR:   detail = "no progress possible";  break;
            case Z_STREAM_ERROR:detail = "invalid parameters";    break;
            case Z_VERSION_ERROR: detail = "zlib version mismatch"; break;
            default:            detail = "failed";                break;
        }
    }
    char b[256];
    size_t i = 0, j;
    for (j = 0; what[j] && i < sizeof b - 3; j++) b[i++] = what[j];
    b[i++] = ':'; b[i++] = ' ';
    for (j = 0; detail[j] && i < sizeof b - 1; j++) b[i++] = detail[j];
    b[i] = 0;
    set_err(b);
}

/* zlib selects the framing through windowBits: 8..15 is zlib, negated is raw,
 * and +16 is gzip. Encoding that here keeps the magic in one place. */
static int window_bits(int format) {
    switch (format) {
        case VZL_RAW:  return -15;
        case VZL_GZIP: return 15 + 16;
        default:       return 15;
    }
}

static Buf *buf_new(int cap) {
    Buf *b;
    if (cap < 64) cap = 64;
    b = (Buf *)malloc(sizeof *b);
    if (!b) return 0;
    b->p = (unsigned char *)malloc((size_t)cap);
    if (!b->p) { free(b); return 0; }
    b->len = 0;
    b->cap = cap;
    return b;
}

static int buf_reserve(Buf *b, int extra) {
    int want;
    unsigned char *q;
    if (b->cap - b->len >= extra) return 1;
    want = b->cap;
    /* Doubling, with an overflow guard: a bomb must fail as an error rather
     * than wrap to a small allocation. */
    while (want - b->len < extra) {
        if (want > (int)0x7FFFFFFF / 2) { set_err("output too large"); return 0; }
        want *= 2;
    }
    q = (unsigned char *)realloc(b->p, (size_t)want);
    if (!q) { set_err("out of memory"); return 0; }
    b->p = q;
    b->cap = want;
    return 1;
}

static void buf_free(Buf *b) {
    if (!b) return;
    free(b->p);
    free(b);
}

#define CHUNK 32768

void *vzl_deflate(const void *src, int n, int format, int level) {
    z_stream zs;
    Buf *out;
    int rc;

    set_err("");
    if (n < 0 || (!src && n > 0)) { set_err("deflate: bad input"); return 0; }
    if (level < 0 || level > 9) { set_err("deflate: level must be 0-9"); return 0; }

    memset(&zs, 0, sizeof zs);
    zl_set_alloc(&zs);
    rc = deflateInit2(&zs, level, Z_DEFLATED, window_bits(format), 8,
                      Z_DEFAULT_STRATEGY);
    if (rc != Z_OK) { set_zerr("deflateInit2", rc, &zs); return 0; }

    /* deflateBound is an upper bound for the whole input in one pass, so the
     * common case never reallocates. */
    out = buf_new((int)deflateBound(&zs, (uLong)n) + 64);
    if (!out) { deflateEnd(&zs); set_err("out of memory"); return 0; }

    zs.next_in = (Bytef *)(void *)(char *)src;
    zs.avail_in = (uInt)n;

    for (;;) {
        if (!buf_reserve(out, CHUNK)) { buf_free(out); deflateEnd(&zs); return 0; }
        zs.next_out = out->p + out->len;
        zs.avail_out = (uInt)(out->cap - out->len);
        rc = deflate(&zs, Z_FINISH);
        out->len = out->cap - (int)zs.avail_out;
        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK && rc != Z_BUF_ERROR) {
            set_zerr("deflate", rc, &zs);
            buf_free(out); deflateEnd(&zs);
            return 0;
        }
    }
    deflateEnd(&zs);
    return out;
}

void *vzl_inflate(const void *src, int n, int format, int limit) {
    z_stream zs;
    Buf *out;
    int rc;

    set_err("");
    if (n < 0 || (!src && n > 0)) { set_err("inflate: bad input"); return 0; }

    memset(&zs, 0, sizeof zs);
    zl_set_alloc(&zs);
    rc = inflateInit2(&zs, window_bits(format));
    if (rc != Z_OK) { set_zerr("inflateInit2", rc, &zs); return 0; }

    out = buf_new(n > 0 && n < 0x1000000 ? n * 4 : CHUNK);
    if (!out) { inflateEnd(&zs); set_err("out of memory"); return 0; }

    zs.next_in = (Bytef *)(void *)(char *)src;
    zs.avail_in = (uInt)n;

    for (;;) {
        if (!buf_reserve(out, CHUNK)) { buf_free(out); inflateEnd(&zs); return 0; }
        zs.next_out = out->p + out->len;
        zs.avail_out = (uInt)(out->cap - out->len);
        rc = inflate(&zs, Z_NO_FLUSH);
        out->len = out->cap - (int)zs.avail_out;

        /* Refuse a decompression bomb before it exhausts memory. Checked after
         * each chunk rather than at the end, so the limit caps what is actually
         * allocated rather than merely what is reported. */
        if (limit > 0 && out->len > limit) {
            set_err("inflate: output exceeds limit");
            buf_free(out); inflateEnd(&zs);
            return 0;
        }
        if (rc == Z_STREAM_END) break;
        if (rc == Z_OK) continue;
        if (rc == Z_BUF_ERROR && zs.avail_in == 0) {
            /* Input ran out with no end-of-stream marker. */
            set_err("inflate: truncated stream");
            buf_free(out); inflateEnd(&zs);
            return 0;
        }
        if (rc != Z_BUF_ERROR) {
            set_zerr("inflate", rc, &zs);
            buf_free(out); inflateEnd(&zs);
            return 0;
        }
    }
    inflateEnd(&zs);
    return out;
}

/* ---- streaming ---- */

typedef struct {
    z_stream zs;
    Buf *out;
    int compressing;
    int done;
    int broken;
} Stream;

void *vzl_stream_new(int compressing, int format, int level) {
    Stream *s;
    int rc;

    set_err("");
    if (compressing && (level < 0 || level > 9)) {
        set_err("stream: level must be 0-9");
        return 0;
    }
    s = (Stream *)malloc(sizeof *s);
    if (!s) { set_err("out of memory"); return 0; }
    memset(s, 0, sizeof *s);
    zl_set_alloc(&s->zs);

    if (compressing) {
        rc = deflateInit2(&s->zs, level, Z_DEFLATED, window_bits(format), 8,
                          Z_DEFAULT_STRATEGY);
        if (rc != Z_OK) { set_zerr("deflateInit2", rc, &s->zs); free(s); return 0; }
    } else {
        rc = inflateInit2(&s->zs, window_bits(format));
        if (rc != Z_OK) { set_zerr("inflateInit2", rc, &s->zs); free(s); return 0; }
    }
    s->out = buf_new(CHUNK);
    if (!s->out) {
        if (compressing) deflateEnd(&s->zs); else inflateEnd(&s->zs);
        free(s);
        set_err("out of memory");
        return 0;
    }
    s->compressing = compressing;
    return s;
}

static int stream_run(Stream *s, int flush) {
    int rc;
    for (;;) {
        if (!buf_reserve(s->out, CHUNK)) { s->broken = 1; return -1; }
        s->zs.next_out = s->out->p + s->out->len;
        s->zs.avail_out = (uInt)(s->out->cap - s->out->len);

        rc = s->compressing ? deflate(&s->zs, flush) : inflate(&s->zs, flush);
        s->out->len = s->out->cap - (int)s->zs.avail_out;

        if (rc == Z_STREAM_END) { s->done = 1; return 0; }
        if (rc == Z_OK) {
            /* Keep going while there is input left or room was the constraint. */
            if (s->zs.avail_in == 0 && s->zs.avail_out != 0) return 0;
            continue;
        }
        if (rc == Z_BUF_ERROR) {
            /* No progress possible: for a push that just means "need more
             * input", which is not an error. */
            if (flush == Z_NO_FLUSH) return 0;
            set_err(s->compressing ? "deflate: no progress"
                                   : "inflate: truncated stream");
            s->broken = 1;
            return -1;
        }
        set_zerr(s->compressing ? "deflate" : "inflate", rc, &s->zs);
        s->broken = 1;
        return -1;
    }
}

int vzl_stream_push(void *h, const void *src, int n) {
    Stream *s = (Stream *)h;
    set_err("");
    if (!s || s->broken) { set_err("stream: unusable"); return -1; }
    if (s->done) { set_err("stream: already finished"); return -1; }
    if (n < 0 || (!src && n > 0)) { set_err("stream: bad input"); return -1; }
    if (n == 0) return 0;

    s->zs.next_in = (Bytef *)(void *)(char *)src;
    s->zs.avail_in = (uInt)n;
    return stream_run(s, Z_NO_FLUSH);
}

int vzl_stream_finish(void *h) {
    Stream *s = (Stream *)h;
    set_err("");
    if (!s || s->broken) { set_err("stream: unusable"); return -1; }
    if (s->done) return 0;
    s->zs.next_in = 0;
    s->zs.avail_in = 0;
    if (stream_run(s, Z_FINISH) != 0) return -1;
    if (!s->done) {
        /* Z_FINISH returned without Z_STREAM_END: for decompression that means
         * the trailer never arrived. Reporting it is the whole point of having
         * a finish step. */
        set_err(s->compressing ? "deflate: incomplete"
                               : "inflate: truncated stream");
        s->broken = 1;
        return -1;
    }
    return 0;
}

int vzl_stream_pending(void *h) {
    Stream *s = (Stream *)h;
    return s ? s->out->len : 0;
}

int vzl_stream_take(void *h, void *dst, int cap) {
    Stream *s = (Stream *)h;
    int n;
    if (!s || !dst || cap <= 0) return 0;
    n = s->out->len;
    if (n > cap) n = cap;
    memcpy(dst, s->out->p, (size_t)n);
    /* Shift the remainder down rather than reallocating: a caller that sizes
     * its buffer from vzl_stream_pending takes everything and this is a no-op. */
    if (n < s->out->len) {
        memmove(s->out->p, s->out->p + n, (size_t)(s->out->len - n));
    }
    s->out->len -= n;
    return n;
}

void vzl_stream_free(void *h) {
    Stream *s = (Stream *)h;
    if (!s) return;
    if (s->compressing) deflateEnd(&s->zs); else inflateEnd(&s->zs);
    buf_free(s->out);
    free(s);
}

/* ---- output buffer handles ---- */

int vzl_buf_len(void *h) {
    Buf *b = (Buf *)h;
    return b ? b->len : 0;
}

int vzl_buf_take(void *h, void *dst, int cap) {
    Buf *b = (Buf *)h;
    int n;
    if (!b) return 0;
    n = b->len;
    if (n > cap) n = cap;
    if (dst && n > 0) memcpy(dst, b->p, (size_t)n);
    /* Frees even on a short copy, so no caller path leaks. */
    buf_free(b);
    return n;
}

void vzl_buf_free(void *h) { buf_free((Buf *)h); }

const char *vzl_last_error(void) { return g_err; }
int vzl_available(void) { return 1; }
const char *vzl_version(void) { return ZLIB_VERSION; }

#endif /* VT_NO_LIBC */
