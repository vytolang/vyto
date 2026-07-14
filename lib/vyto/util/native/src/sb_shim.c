/* vyto/util/text native backing — a growable string buffer for StringBuilder.

   Owns a doubling char buffer, kept NUL-terminated so the Vyto side can read it
   back with str(sb_cstr(h)). The buffer is an opaque rawptr owned by a Vyto
   StringBuilder whose deinit calls sb_free exactly once — the same
   opaque-handle + explicit-free shape as file_shim.c / socket_shim.c.

   append() takes an explicit byte count, so building is binary-safe (interior
   NULs are preserved); only the strlen-based toString() truncates at a NUL,
   matching str(bytes as cstring) everywhere else in the stdlib. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *buf;
    size_t len, cap;
} StrBuf;

/* ensure room for `extra` more bytes plus the NUL terminator */
static int sb_ensure(StrBuf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return 1;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < b->len + extra + 1) nc *= 2;
    char *nb = (char *)realloc(b->buf, nc);
    if (!nb) return 0;
    b->buf = nb;
    b->cap = nc;
    return 1;
}

void *sb_new(long cap) {
    StrBuf *b = (StrBuf *)calloc(1, sizeof *b);
    if (!b) return NULL;
    size_t c0 = cap > 0 ? (size_t)cap : 64;
    b->buf = (char *)malloc(c0);
    if (!b->buf) { free(b); return NULL; }
    b->cap = c0;
    b->len = 0;
    b->buf[0] = 0;
    return b;
}

void sb_append(void *h, const char *s, long n) {
    if (!h || !s || n <= 0) return;
    StrBuf *b = (StrBuf *)h;
    if (!sb_ensure(b, (size_t)n)) return;
    memcpy(b->buf + b->len, s, (size_t)n);
    b->len += (size_t)n;
    b->buf[b->len] = 0;
}

void sb_append_i64(void *h, long long v) {
    if (!h) return;
    char tmp[32];
    int n = snprintf(tmp, sizeof tmp, "%lld", v);
    if (n > 0) sb_append(h, tmp, n);
}

void sb_append_f64(void *h, double v) {
    if (!h) return;
    char tmp[40];
    int n = snprintf(tmp, sizeof tmp, "%g", v); /* matches vt_str_from_float */
    if (n > 0) sb_append(h, tmp, n);
}

void sb_append_byte(void *h, int byte) {
    if (!h) return;
    StrBuf *b = (StrBuf *)h;
    if (!sb_ensure(b, 1)) return;
    b->buf[b->len++] = (char)(byte & 0xFF);
    b->buf[b->len] = 0;
}

long sb_len(void *h) { return h ? (long)((StrBuf *)h)->len : 0; }

const char *sb_cstr(void *h) { return h ? ((StrBuf *)h)->buf : ""; }

void sb_clear(void *h) {
    if (!h) return;
    StrBuf *b = (StrBuf *)h;
    b->len = 0;
    b->buf[0] = 0;
}

void sb_free(void *h) {
    if (!h) return;
    StrBuf *b = (StrBuf *)h;
    free(b->buf);
    free(b);
}
