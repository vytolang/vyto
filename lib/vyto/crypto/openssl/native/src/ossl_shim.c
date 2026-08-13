/* ossl_shim.c — the C half of vyto/crypto/openssl.
 *
 * OpenSSL lives behind this file. Not one of its types, constants or error
 * codes crosses into Vyto: handles are opaque pointers to structs declared
 * here, statuses are the VOSSL_* numbers in ossl_shim.h, and every string is
 * written into a caller-owned (out, cap) buffer. A libssl upgrade therefore
 * cannot change a number the Vyto side compares against. Same arrangement as
 * lib/vyto/regex/native/src/regex_shim.c and lib/vyto/crypto/ecc's ecc_shim.c.
 *
 * Contract, uniform across this file:
 *   - Predicates and "did it work" calls return 1 on success, 0 on failure.
 *   - Handshake/read/write return the VOSSL_* status codes, which split
 *     would-block into want-read and want-write. See ossl_shim.h on why.
 *   - String getters write a NUL-terminated string into (out, cap) and return
 *     the length they WANTED, so the caller grows and retries. The vyto/intl
 *     convention (intl_shim.c:7-11).
 *   - Every entry point that can fail clears the error queue on the way in and
 *     drains it on the way out. OpenSSL's queue is per-thread and sticky: an
 *     unread error from one call is reported as the cause of the next one, and
 *     a handshake that "failed for no reason" is nearly always that.
 *
 * ---- the include is unconditional, deliberately ----
 *
 * The OpenSSL headers are included with no #ifdef, exactly as vyto/net's net_shim.c
 * includes <curl/curl.h>. native/src is globbed flat and compiled for every
 * target (CLAUDE.md), so a triple with no provisioned headers fails to compile
 * this package — loudly, at build time, naming the missing header. That is the
 * intended outcome: a TLS package that silently compiles to nothing on a
 * platform is how a program ships without encryption. Linux uses the system
 * libssl-dev; macOS and Windows use native/<triple>/include from
 * native/provision-openssl.sh.
 */

#include "ossl_shim.h"

#ifdef VT_NO_LIBC

/* ---- freestanding ------------------------------------------------------
 *
 * --freestanding splices -DVT_NO_LIBC into every package shim's compile line,
 * not just the runtime's (src/main.c), so an unguarded <openssl/ssl.h> would
 * break the freestanding build of anything that merely imports this package.
 * There is no OpenSSL without libc, so the whole library drops out and every
 * entry point returns its documented failure sentinel. The .vt side needs no
 * platform conditionals: vossl_available() answers 0 and every constructor
 * returns null, which the Vyto classes already treat as "not available".
 */

int  vossl_version(char *out, int cap)    { if (out && cap > 0) out[0] = 0; return 0; }
int  vossl_available(void)                { return 0; }

void *vossl_ctx_client(int verify)        { (void)verify; return 0; }
void *vossl_ctx_server(const char *c, const char *k, char *err, int cap) {
    (void)c; (void)k; if (err && cap > 0) err[0] = 0; return 0;
}
int   vossl_ctx_min_version(void *x, const char *v) { (void)x; (void)v; return 0; }
int   vossl_ctx_alpn(void *x, const char *s)        { (void)x; (void)s; return 0; }
int   vossl_ctx_ca_file(void *x, const char *p)     { (void)x; (void)p; return 0; }
void  vossl_ctx_free(void *x)                       { (void)x; }

void *vossl_conn_new(void *x, int fd, const char *h) { (void)x; (void)fd; (void)h; return 0; }
int   vossl_handshake(void *c, char *e, int cap) { (void)c; if (e && cap > 0) e[0] = 0; return 0; }
int   vossl_read(void *c, void *d, int n)        { (void)c; (void)d; (void)n; return VOSSL_FAILED; }
int   vossl_write(void *c, const void *s, int o, int n) {
    (void)c; (void)s; (void)o; (void)n; return VOSSL_FAILED;
}
int   vossl_proto_version(void *c, char *o, int cap) { (void)c; if (o && cap > 0) o[0] = 0; return 0; }
int   vossl_alpn_selected(void *c, char *o, int cap) { (void)c; if (o && cap > 0) o[0] = 0; return 0; }
void *vossl_peer_cert(void *c)                       { (void)c; return 0; }
int   vossl_last_error(void *c, char *o, int cap)    { (void)c; if (o && cap > 0) o[0] = 0; return 0; }
int   vossl_conn_shutdown(void *c)                   { (void)c; return 0; }
void  vossl_conn_free(void *c)                       { (void)c; }

void *vossl_cert_from_pem(const char *p)             { (void)p; return 0; }
void *vossl_cert_from_der(const void *d, int n)      { (void)d; (void)n; return 0; }
int   vossl_cert_subject(void *x, char *o, int cap)  { (void)x; if (o && cap > 0) o[0] = 0; return 0; }
int   vossl_cert_issuer(void *x, char *o, int cap)   { (void)x; if (o && cap > 0) o[0] = 0; return 0; }
int   vossl_cert_serial(void *x, char *o, int cap)   { (void)x; if (o && cap > 0) o[0] = 0; return 0; }
long long vossl_cert_not_before(void *x)             { (void)x; return 0; }
long long vossl_cert_not_after(void *x)              { (void)x; return 0; }
int   vossl_cert_san_count(void *x)                  { (void)x; return 0; }
int   vossl_cert_san_at(void *x, int i, char *o, int cap) {
    (void)x; (void)i; if (o && cap > 0) o[0] = 0; return 0;
}
int   vossl_cert_matches_host(void *x, const char *h) { (void)x; (void)h; return 0; }
int   vossl_cert_fingerprint(void *x, void *o)        { (void)x; (void)o; return 0; }
void  vossl_cert_free(void *x)                        { (void)x; }

void *vossl_store_new(void)                           { return 0; }
int   vossl_store_add(void *s, void *x)               { (void)s; (void)x; return 0; }
void  vossl_store_free(void *s)                       { (void)s; }
void *vossl_chain_new(void)                           { return 0; }
int   vossl_chain_add(void *c, void *x)               { (void)c; (void)x; return 0; }
void  vossl_chain_free(void *c)                       { (void)c; }
int   vossl_verify_chain(void *s, void *u, void *l, char *e, int cap) {
    (void)s; (void)u; (void)l; if (e && cap > 0) e[0] = 0; return 0;
}

#else /* !VT_NO_LIBC — the real implementation */

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/bn.h>
#include <openssl/opensslv.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The 1.1 -> 3.x soname change is not ABI-compatible and 3.x deprecated the
   low-level APIs this file would otherwise need two paths for. Refusing at
   compile time beats discovering it at link time on someone else's machine. */
#if OPENSSL_VERSION_NUMBER < 0x30000000L
#error "vyto/crypto/openssl requires OpenSSL 3.0 or newer"
#endif

/* ---- handles ------------------------------------------------------------
 *
 * Both handles are structs of ours rather than a bare OpenSSL pointer, because
 * both need to own something OpenSSL will not own for them: the context owns
 * its ALPN wire list (SSL_CTX_set_alpn_protos copies, but the server-side
 * select callback needs the list to outlive the call), and the connection
 * remembers the last SSL_get_error so vossl_last_error can describe a read or
 * write failure that had no room for an error buffer.
 */

typedef struct VosslCtx {
    SSL_CTX       *ctx;
    unsigned char *alpn;        /* length-prefixed wire format, owned here */
    unsigned       alpn_len;
    int            check_host;  /* VOSSL_VERIFY_FULL asked for a hostname check */
    int            is_server;
} VosslCtx;

typedef struct VosslConn {
    SSL *ssl;
    int  last;                  /* the last SSL_get_error() result */
} VosslConn;

/* ---- init ---------------------------------------------------------------
 *
 * 3.x initialises itself on first use, so this exists only to load the error
 * strings — without them every diagnostic in this file degrades to a number.
 */
static int g_inited = 0;

static int ensure_init(void) {
    if (g_inited) return 1;
    if (OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS
                       | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL) != 1) return 0;
    g_inited = 1;
    return 1;
}

/* ---- error handling -----------------------------------------------------
 *
 * drain() is called on the way out of every failure path. Leaving an entry in
 * the queue is not cosmetic: the next call to reach ERR_get_error() reports it
 * as its own cause, so one real failure produces a trail of false ones.
 */
static void drain(void) { while (ERR_get_error() != 0) { } }

/* Defined with the certificate helpers; needed by ossl_conn_new above them. */
static int is_ip_literal(const char *h);

static int put(char *out, int cap, const char *s) {
    int need = (int)strlen(s);
    if (out && cap > 0) {
        int n = need < cap - 1 ? need : cap - 1;
        memcpy(out, s, (size_t)n);
        out[n] = 0;
    }
    return need;
}

/* The first queued reason, or "" when the queue is empty. */
static int first_reason(char *out, int cap) {
    unsigned long e = ERR_get_error();
    if (e == 0) return put(out, cap, "");
    char b[256];
    ERR_error_string_n(e, b, sizeof b);
    drain();
    return put(out, cap, b);
}

/* Why an SSL operation failed, in the order that produces the most useful
   answer. A verification failure has a specific, nameable cause and OpenSSL
   files it away from the error queue, so it is checked first — "unable to get
   local issuer certificate" is a fixable message and "tlsv1 alert unknown ca"
   is not. */
static int describe(VosslConn *c, char *out, int cap) {
    long v = SSL_get_verify_result(c->ssl);
    if (v != X509_V_OK) {
        char b[VOSSL_ERR_CAP];
        snprintf(b, sizeof b, "certificate verification failed: %s",
                 X509_verify_cert_error_string(v));
        drain();
        return put(out, cap, b);
    }
    if (ERR_peek_error() != 0) return first_reason(out, cap);
    if (c->last == SSL_ERROR_SYSCALL) {
        return put(out, cap, "the connection was closed before TLS finished");
    }
    if (c->last == SSL_ERROR_ZERO_RETURN) {
        return put(out, cap, "the peer closed the TLS connection");
    }
    return put(out, cap, "the TLS operation failed");
}

/* ---- library ------------------------------------------------------------ */

int vossl_version(char *out, int cap) {
    if (!ensure_init()) return put(out, cap, "");
    const char *v = OpenSSL_version(OPENSSL_VERSION);
    return put(out, cap, v ? v : "");
}

int vossl_available(void) { return ensure_init(); }

/* ---- contexts ----------------------------------------------------------- */

/* Every context gets these. ENABLE_PARTIAL_WRITE is what lets ossl_write
   report a short count instead of being all-or-nothing, which is the contract
   WireIo.rawWrite already documents; ACCEPT_MOVING_WRITE_BUFFER makes a retry
   from a different address defined behaviour rather than undefined. */
static void common_mode(SSL_CTX *c) {
    SSL_CTX_set_mode(c, SSL_MODE_ENABLE_PARTIAL_WRITE
                      | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
    /* Compression is a CRIME oracle and renegotiation is a DoS lever; neither
       buys anything a database or an HTTP client wants. */
    SSL_CTX_set_options(c, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
}

static VosslCtx *ctx_alloc(SSL_CTX *inner) {
    VosslCtx *w = (VosslCtx *)calloc(1, sizeof *w);
    if (!w) { SSL_CTX_free(inner); return NULL; }
    w->ctx = inner;
    return w;
}

void *vossl_ctx_client(int verify) {
    if (!ensure_init()) return NULL;
    ERR_clear_error();

    SSL_CTX *c = SSL_CTX_new(TLS_client_method());
    if (!c) { drain(); return NULL; }
    common_mode(c);

    if (verify == VOSSL_VERIFY_NONE) {
        SSL_CTX_set_verify(c, SSL_VERIFY_NONE, NULL);
    } else {
        SSL_CTX_set_verify(c, SSL_VERIFY_PEER, NULL);
        /* Without a trust store, SSL_VERIFY_PEER fails everything. A missing
           store is a broken installation, not a connection this should make
           unverified. */
        if (SSL_CTX_set_default_verify_paths(c) != 1) {
            SSL_CTX_free(c);
            drain();
            return NULL;
        }
    }

    VosslCtx *w = ctx_alloc(c);
    if (!w) { drain(); return NULL; }
    w->check_host = (verify == VOSSL_VERIFY_FULL);
    drain();
    return w;
}

/* PEM text, not a path: tls_server() takes the certificate and key as strings
   so an app can hold them anywhere — a file, a secret store, an embedded
   asset — without this layer growing a filesystem opinion. */
static int load_chain(SSL_CTX *c, const char *pem) {
    BIO *b = BIO_new_mem_buf(pem, -1);
    if (!b) return 0;

    X509 *leaf = PEM_read_bio_X509(b, NULL, NULL, NULL);
    if (!leaf) { BIO_free(b); return 0; }
    int ok = (SSL_CTX_use_certificate(c, leaf) == 1);
    X509_free(leaf);

    while (ok) {
        X509 *ca = PEM_read_bio_X509(b, NULL, NULL, NULL);
        if (!ca) break;                     /* end of the file, not an error */
        if (SSL_CTX_add0_chain_cert(c, ca) != 1) { X509_free(ca); ok = 0; }
        /* add0 took ownership on success — do not free ca here. */
    }
    /* The loop always ends on a PEM_R_NO_START_LINE that is not a failure. */
    drain();
    BIO_free(b);
    return ok;
}

void *vossl_ctx_server(const char *chain_pem, const char *key_pem, char *err, int cap) {
    if (!ensure_init()) { put(err, cap, "OpenSSL could not be initialised"); return NULL; }
    ERR_clear_error();

    SSL_CTX *c = SSL_CTX_new(TLS_server_method());
    if (!c) { first_reason(err, cap); return NULL; }
    common_mode(c);

    if (!load_chain(c, chain_pem)) {
        SSL_CTX_free(c);
        if (first_reason(err, cap) == 0) put(err, cap, "the certificate chain could not be read");
        return NULL;
    }

    BIO *kb = BIO_new_mem_buf(key_pem, -1);
    EVP_PKEY *pk = kb ? PEM_read_bio_PrivateKey(kb, NULL, NULL, NULL) : NULL;
    if (kb) BIO_free(kb);
    if (!pk) {
        SSL_CTX_free(c);
        if (first_reason(err, cap) == 0) put(err, cap, "the private key could not be read");
        return NULL;
    }
    int ok = (SSL_CTX_use_PrivateKey(c, pk) == 1);
    EVP_PKEY_free(pk);
    if (!ok || SSL_CTX_check_private_key(c) != 1) {
        SSL_CTX_free(c);
        if (first_reason(err, cap) == 0) put(err, cap, "the private key does not match the certificate");
        return NULL;
    }

    VosslCtx *w = ctx_alloc(c);
    if (!w) { put(err, cap, "out of memory"); drain(); return NULL; }
    w->is_server = 1;
    drain();
    put(err, cap, "");
    return w;
}

int vossl_ctx_min_version(void *ctx, const char *v) {
    VosslCtx *w = (VosslCtx *)ctx;
    if (!w || !v) return 0;
    int ver;
    if      (strcmp(v, "TLS1.2") == 0) ver = TLS1_2_VERSION;
    else if (strcmp(v, "TLS1.3") == 0) ver = TLS1_3_VERSION;
    else return 0;
    ERR_clear_error();
    int r = (SSL_CTX_set_min_proto_version(w->ctx, ver) == 1);
    drain();
    return r;
}

/* "h2,http/1.1" -> the length-prefixed wire form ALPN actually uses. */
static unsigned char *alpn_encode(const char *csv, unsigned *out_len) {
    size_t total = strlen(csv);
    if (total == 0) return NULL;
    unsigned char *w = (unsigned char *)malloc(total + 8);
    if (!w) return NULL;

    unsigned n = 0;
    const char *p = csv;
    while (*p) {
        const char *q = strchr(p, ',');
        size_t len = q ? (size_t)(q - p) : strlen(p);
        if (len == 0 || len > 255) { free(w); return NULL; }
        w[n++] = (unsigned char)len;
        memcpy(w + n, p, len);
        n += (unsigned)len;
        if (!q) break;
        p = q + 1;
    }
    *out_len = n;
    return w;
}

/* Server-side selection. This is a callback, but it is C calling C — the thing
   openssl.vt puts out of scope is a callback that has to re-enter Vyto, which
   this is not. Server preference wins: the server's list is the ordered one. */
static int alpn_select(SSL *ssl, const unsigned char **out, unsigned char *outlen,
                       const unsigned char *in, unsigned int inlen, void *arg) {
    (void)ssl;
    VosslCtx *w = (VosslCtx *)arg;
    if (!w || !w->alpn) return SSL_TLSEXT_ERR_NOACK;
    unsigned i = 0;
    while (i < w->alpn_len) {
        unsigned char len = w->alpn[i];
        const unsigned char *want = w->alpn + i + 1;
        unsigned j = 0;
        while (j < inlen) {
            unsigned char have = in[j];
            if (have == len && memcmp(in + j + 1, want, len) == 0) {
                *out = in + j + 1;
                *outlen = have;
                return SSL_TLSEXT_ERR_OK;
            }
            j += 1u + have;
        }
        i += 1u + len;
    }
    return SSL_TLSEXT_ERR_NOACK;
}

int vossl_ctx_alpn(void *ctx, const char *csv) {
    VosslCtx *w = (VosslCtx *)ctx;
    if (!w || !csv) return 0;
    unsigned len = 0;
    unsigned char *wire = alpn_encode(csv, &len);
    if (!wire) return 0;

    free(w->alpn);
    w->alpn = wire;
    w->alpn_len = len;

    ERR_clear_error();
    /* Inverted return: this one answers 0 for success. */
    int ok = (SSL_CTX_set_alpn_protos(w->ctx, wire, len) == 0);
    SSL_CTX_set_alpn_select_cb(w->ctx, alpn_select, w);
    drain();
    return ok;
}

int vossl_ctx_ca_file(void *ctx, const char *path) {
    VosslCtx *w = (VosslCtx *)ctx;
    if (!w || !path || !*path) return 0;
    ERR_clear_error();
    int r = (SSL_CTX_load_verify_locations(w->ctx, path, NULL) == 1);
    drain();
    return r;
}

void vossl_ctx_free(void *ctx) {
    VosslCtx *w = (VosslCtx *)ctx;
    if (!w) return;
    if (w->ctx) SSL_CTX_free(w->ctx);
    free(w->alpn);
    free(w);
}

/* ---- connections -------------------------------------------------------- */

void *vossl_conn_new(void *ctx, int fd, const char *host) {
    VosslCtx *w = (VosslCtx *)ctx;
    if (!w || fd < 0) return NULL;
    ERR_clear_error();

    SSL *s = SSL_new(w->ctx);
    if (!s) { drain(); return NULL; }

    if (SSL_set_fd(s, fd) != 1) { SSL_free(s); drain(); return NULL; }

    /* SSL_new already infers this from the context's method. Saying it anyway
       makes a context built the wrong way round fail at the handshake with a
       protocol error instead of hanging waiting for a peer that is also
       waiting. */
    if (w->is_server) SSL_set_accept_state(s); else SSL_set_connect_state(s);

    if (host && *host) {
        /* SNI. An IP literal is not a legal SNI value and servers reject it,
           so it is set only for a name. */
        if (!is_ip_literal(host)) SSL_set_tlsext_host_name(s, host);

        if (w->check_host) {
            /* Hostname verification inside the handshake, so a mismatch fails
               there rather than after the caller has already sent something.
               NO_PARTIAL_WILDCARDS rejects "f*.example.com": a wildcard matches
               one whole label or nothing. */
            SSL_set_hostflags(s, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
            /* set1_host takes an IP literal too and matches it against the
               iPAddress SANs, so there is one call rather than two paths. */
            if (SSL_set1_host(s, host) != 1) { SSL_free(s); drain(); return NULL; }
        }
    }

    VosslConn *c = (VosslConn *)calloc(1, sizeof *c);
    if (!c) { SSL_free(s); drain(); return NULL; }
    c->ssl = s;
    drain();
    return c;
}

int vossl_handshake(void *h, char *err, int cap) {
    VosslConn *c = (VosslConn *)h;
    if (!c || !c->ssl) { put(err, cap, "the TLS connection is closed"); return 0; }
    ERR_clear_error();

    int r = SSL_do_handshake(c->ssl);
    if (r == 1) { put(err, cap, ""); return 1; }

    int e = SSL_get_error(c->ssl, r);
    c->last = e;
    if (e == SSL_ERROR_WANT_READ)  { drain(); return VOSSL_WANT_READ; }
    if (e == SSL_ERROR_WANT_WRITE) { drain(); return VOSSL_WANT_WRITE; }
    describe(c, err, cap);
    drain();
    return 0;
}

int vossl_read(void *h, void *dst, int n) {
    VosslConn *c = (VosslConn *)h;
    if (!c || !c->ssl || !dst || n <= 0) return VOSSL_FAILED;
    ERR_clear_error();

    int r = SSL_read(c->ssl, dst, n);
    if (r > 0) return r;

    int e = SSL_get_error(c->ssl, r);
    c->last = e;
    if (e == SSL_ERROR_WANT_READ)  { drain(); return VOSSL_WANT_READ; }
    if (e == SSL_ERROR_WANT_WRITE) { drain(); return VOSSL_WANT_WRITE; }
    /* ZERO_RETURN is close_notify: the peer said goodbye properly. SYSCALL with
       r == 0 is the socket vanishing without one, which is a truncation and
       stays distinguishable as an error rather than being laundered into EOF. */
    if (e == SSL_ERROR_ZERO_RETURN) { drain(); return 0; }
    return VOSSL_FAILED;
}

int vossl_write(void *h, const void *src, int off, int n) {
    VosslConn *c = (VosslConn *)h;
    if (!c || !c->ssl || !src || n <= 0 || off < 0) return VOSSL_FAILED;
    ERR_clear_error();

    int r = SSL_write(c->ssl, (const unsigned char *)src + off, n);
    if (r > 0) return r;

    int e = SSL_get_error(c->ssl, r);
    c->last = e;
    if (e == SSL_ERROR_WANT_READ)  { drain(); return VOSSL_WANT_READ; }
    if (e == SSL_ERROR_WANT_WRITE) { drain(); return VOSSL_WANT_WRITE; }
    if (e == SSL_ERROR_ZERO_RETURN) { drain(); return 0; }
    return VOSSL_FAILED;
}

int vossl_proto_version(void *h, char *out, int cap) {
    VosslConn *c = (VosslConn *)h;
    if (!c || !c->ssl) return put(out, cap, "");
    const char *v = SSL_get_version(c->ssl);
    return put(out, cap, v ? v : "");
}

int vossl_alpn_selected(void *h, char *out, int cap) {
    VosslConn *c = (VosslConn *)h;
    if (!c || !c->ssl) return put(out, cap, "");
    const unsigned char *p = NULL;
    unsigned len = 0;
    SSL_get0_alpn_selected(c->ssl, &p, &len);
    if (!p || len == 0) return put(out, cap, "");
    if (out && cap > 0) {
        unsigned n = len < (unsigned)(cap - 1) ? len : (unsigned)(cap - 1);
        memcpy(out, p, n);
        out[n] = 0;
    }
    return (int)len;
}

void *vossl_peer_cert(void *h) {
    VosslConn *c = (VosslConn *)h;
    if (!c || !c->ssl) return NULL;
    ERR_clear_error();
    X509 *x = SSL_get1_peer_certificate(c->ssl);   /* takes a reference */
    drain();
    return x;
}

int vossl_last_error(void *h, char *out, int cap) {
    VosslConn *c = (VosslConn *)h;
    if (!c || !c->ssl) return put(out, cap, "the TLS connection is closed");
    return describe(c, out, cap);
}

int vossl_conn_shutdown(void *h) {
    VosslConn *c = (VosslConn *)h;
    if (!c || !c->ssl) return 0;
    ERR_clear_error();
    /* One close_notify is what the peer needs to tell a clean close from a
       truncation. Waiting for theirs would mean blocking on a socket the
       caller is about to drop, so the second SSL_shutdown is not attempted. */
    int r = SSL_shutdown(c->ssl);
    drain();
    return r >= 0;
}

void vossl_conn_free(void *h) {
    VosslConn *c = (VosslConn *)h;
    if (!c) return;
    if (c->ssl) SSL_free(c->ssl);
    free(c);
}

/* ---- certificates ------------------------------------------------------- */

void *vossl_cert_from_pem(const char *pem) {
    if (!ensure_init() || !pem) return NULL;
    ERR_clear_error();
    BIO *b = BIO_new_mem_buf(pem, -1);
    if (!b) { drain(); return NULL; }
    X509 *x = PEM_read_bio_X509(b, NULL, NULL, NULL);
    BIO_free(b);
    drain();
    return x;
}

void *vossl_cert_from_der(const void *der, int n) {
    if (!ensure_init() || !der || n <= 0) return NULL;
    ERR_clear_error();
    const unsigned char *p = (const unsigned char *)der;
    X509 *x = d2i_X509(NULL, &p, (long)n);
    drain();
    return x;
}

static int name_to(X509_NAME *nm, char *out, int cap) {
    if (!nm) return put(out, cap, "");
    BIO *b = BIO_new(BIO_s_mem());
    if (!b) return put(out, cap, "");
    /* RFC 2253 rather than the legacy oneline format: it is the spelling every
       other tool prints, and it escapes embedded commas instead of producing a
       DN that cannot be parsed back. */
    X509_NAME_print_ex(b, nm, 0, XN_FLAG_RFC2253);
    char *p = NULL;
    long len = BIO_get_mem_data(b, &p);
    int need = (int)len;
    if (out && cap > 0) {
        int k = need < cap - 1 ? need : cap - 1;
        if (k > 0) memcpy(out, p, (size_t)k);
        out[k > 0 ? k : 0] = 0;
    }
    BIO_free(b);
    return need;
}

int vossl_cert_subject(void *x, char *out, int cap) {
    if (!x) return put(out, cap, "");
    return name_to(X509_get_subject_name((X509 *)x), out, cap);
}

int vossl_cert_issuer(void *x, char *out, int cap) {
    if (!x) return put(out, cap, "");
    return name_to(X509_get_issuer_name((X509 *)x), out, cap);
}

int vossl_cert_serial(void *x, char *out, int cap) {
    if (!x) return put(out, cap, "");
    ERR_clear_error();
    ASN1_INTEGER *ai = X509_get_serialNumber((X509 *)x);
    BIGNUM *bn = ai ? ASN1_INTEGER_to_BN(ai, NULL) : NULL;
    if (!bn) { drain(); return put(out, cap, ""); }
    char *hex = BN_bn2hex(bn);
    BN_free(bn);
    int need = put(out, cap, hex ? hex : "");
    OPENSSL_free(hex);
    drain();
    return need;
}

/* ASN.1 time -> unix seconds. OpenSSL hands back a struct tm in UTC, and
   timegm is the inverse that does not consult the local timezone; mktime here
   would shift every certificate by the machine's offset. */
static long long asn1_time_to_unix(const ASN1_TIME *t) {
    if (!t) return 0;
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    if (ASN1_TIME_to_tm(t, &tm) != 1) return 0;
#if defined(_WIN32)
    return (long long)_mkgmtime(&tm);
#else
    return (long long)timegm(&tm);
#endif
}

long long vossl_cert_not_before(void *x) {
    if (!x) return 0;
    return asn1_time_to_unix(X509_get0_notBefore((X509 *)x));
}

long long vossl_cert_not_after(void *x) {
    if (!x) return 0;
    return asn1_time_to_unix(X509_get0_notAfter((X509 *)x));
}

/* The SAN list is re-parsed per call rather than cached on the handle. A
   certificate has a handful of names, the Vyto side reads them once at
   construction, and a cache here would be a lifetime to get wrong for nothing. */
static GENERAL_NAMES *sans_of(X509 *x) {
    return (GENERAL_NAMES *)X509_get_ext_d2i(x, NID_subject_alt_name, NULL, NULL);
}

int vossl_cert_san_count(void *x) {
    if (!x) return 0;
    GENERAL_NAMES *g = sans_of((X509 *)x);
    if (!g) { drain(); return 0; }
    int n = sk_GENERAL_NAME_num(g);
    GENERAL_NAMES_free(g);
    return n < 0 ? 0 : n;
}

int vossl_cert_san_at(void *x, int i, char *out, int cap) {
    if (!x || i < 0) return put(out, cap, "");
    GENERAL_NAMES *g = sans_of((X509 *)x);
    if (!g) { drain(); return put(out, cap, ""); }

    int need = 0;
    if (i < sk_GENERAL_NAME_num(g)) {
        const GENERAL_NAME *gn = sk_GENERAL_NAME_value(g, i);
        if (gn->type == GEN_DNS) {
            const unsigned char *s = ASN1_STRING_get0_data(gn->d.dNSName);
            need = put(out, cap, s ? (const char *)s : "");
        } else if (gn->type == GEN_IPADD) {
            /* Rendered rather than skipped: a certificate for an IP is how a
               database behind a load balancer is usually reached, and a caller
               listing SANs to explain a mismatch needs to see it. */
            const unsigned char *ip = ASN1_STRING_get0_data(gn->d.iPAddress);
            int len = ASN1_STRING_length(gn->d.iPAddress);
            char b[64];
            if (len == 4) {
                snprintf(b, sizeof b, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
            } else if (len == 16) {
                snprintf(b, sizeof b,
                         "%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
                         "%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                         ip[0], ip[1], ip[2],  ip[3],  ip[4],  ip[5],  ip[6],  ip[7],
                         ip[8], ip[9], ip[10], ip[11], ip[12], ip[13], ip[14], ip[15]);
            } else {
                b[0] = 0;
            }
            need = put(out, cap, b);
        } else {
            need = put(out, cap, "");
        }
    } else {
        need = put(out, cap, "");
    }
    GENERAL_NAMES_free(g);
    return need;
}

/* Cheap syntactic test, not a parser: it only has to decide whether a string
   should be offered as SNI and whether to match it as an address. */
static int is_ip_literal(const char *h) {
    if (!h || !*h) return 0;
    if (strchr(h, ':')) return 1;                /* any IPv6 form */
    const char *p = h;
    int digits = 0, dots = 0;
    while (*p) {
        if (*p == '.') { dots++; }
        else if (*p >= '0' && *p <= '9') { digits++; }
        else return 0;
        p++;
    }
    return dots == 3 && digits > 0;
}

int vossl_cert_matches_host(void *x, const char *host) {
    if (!x || !host || !*host) return 0;
    ERR_clear_error();
    int r;
    if (is_ip_literal(host)) {
        r = X509_check_ip_asc((X509 *)x, host, 0);
    } else {
        /* RFC 6125 as OpenSSL implements it: SAN when present, CN only when
           there is no SAN, and a wildcard that matches exactly one label.
           NO_PARTIAL_WILDCARDS additionally rejects "f*.example.com". */
        r = X509_check_host((X509 *)x, host, 0,
                            X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS, NULL);
    }
    drain();
    return r == 1;
}

int vossl_cert_fingerprint(void *x, void *out32) {
    if (!x || !out32) return 0;
    ERR_clear_error();
    unsigned len = 0;
    int r = X509_digest((X509 *)x, EVP_sha256(), (unsigned char *)out32, &len);
    drain();
    return (r == 1 && len == VOSSL_FP_SIZE);
}

void vossl_cert_free(void *x) { if (x) X509_free((X509 *)x); }

/* ---- chain verification ------------------------------------------------- */

void *vossl_store_new(void) {
    if (!ensure_init()) return NULL;
    return X509_STORE_new();
}

int vossl_store_add(void *store, void *x) {
    if (!store || !x) return 0;
    ERR_clear_error();
    int r = (X509_STORE_add_cert((X509_STORE *)store, (X509 *)x) == 1);
    drain();
    return r;
}

void vossl_store_free(void *store) { if (store) X509_STORE_free((X509_STORE *)store); }

void *vossl_chain_new(void) {
    if (!ensure_init()) return NULL;
    return sk_X509_new_null();
}

int vossl_chain_add(void *chain, void *x) {
    if (!chain || !x) return 0;
    /* up_ref rather than transferring ownership: the Cert object on the Vyto
       side keeps its handle and frees it in deinit, so the stack must hold its
       own reference or the free order decides whether this crashes. */
    if (X509_up_ref((X509 *)x) != 1) return 0;
    if (sk_X509_push((STACK_OF(X509) *)chain, (X509 *)x) <= 0) {
        X509_free((X509 *)x);
        return 0;
    }
    return 1;
}

void vossl_chain_free(void *chain) {
    if (chain) sk_X509_pop_free((STACK_OF(X509) *)chain, X509_free);
}

int vossl_verify_chain(void *store, void *untrusted, void *leaf, char *err, int cap) {
    if (!store || !leaf) { put(err, cap, "nothing to verify"); return 0; }
    ERR_clear_error();

    X509_STORE_CTX *ctx = X509_STORE_CTX_new();
    if (!ctx) { first_reason(err, cap); return 0; }

    if (X509_STORE_CTX_init(ctx, (X509_STORE *)store, (X509 *)leaf,
                            (STACK_OF(X509) *)untrusted) != 1) {
        X509_STORE_CTX_free(ctx);
        if (first_reason(err, cap) == 0) put(err, cap, "the verification context failed to start");
        return 0;
    }

    int r = X509_verify_cert(ctx);
    if (r != 1) {
        /* The store's error is the specific one — expired, self-signed, no
           issuer. The error queue's version of the same event is generic. */
        int e = X509_STORE_CTX_get_error(ctx);
        put(err, cap, X509_verify_cert_error_string(e));
    } else {
        put(err, cap, "");
    }
    X509_STORE_CTX_free(ctx);
    drain();
    return r == 1;
}

#endif /* VT_NO_LIBC */
