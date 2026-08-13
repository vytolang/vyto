/* ossl_shim.h — the sizes and status codes vyto/crypto/openssl mirrors.
 *
 * Every constant here is OURS. Not one of them is an OpenSSL macro forwarded
 * under a new name, which is the point: a libssl upgrade can renumber anything
 * it likes and no number the Vyto side compares against moves. Same contract as
 * ecc_shim.h (VECC_*) and regex_shim.h (RX_*).
 */
#ifndef OSSL_SHIM_H
#define OSSL_SHIM_H

/* ---- buffer sizes, mirrored as OSSL_* consts in openssl.vt --------------- */

/* Every (out, cap) pair in this file follows the vyto/intl convention: the
   call writes a NUL-terminated string and returns the full length it WANTED,
   so a caller that sees `needed >= cap` grows to needed+1 and calls again.
   These are first guesses generous enough that the retry almost never runs. */
#define VOSSL_ERR_CAP   512     /* an OpenSSL error string, in practice < 200 */
#define VOSSL_NAME_CAP  1024    /* an RFC 2253 subject/issuer DN             */
#define VOSSL_FP_SIZE   32      /* SHA-256 certificate fingerprint, exact    */

/* ---- client verification level, the argument to vossl_ctx_client ---------- */

#define VOSSL_VERIFY_NONE   0   /* nothing is checked                        */
#define VOSSL_VERIFY_CHAIN  1   /* signatures, dates, basic constraints      */
#define VOSSL_VERIFY_FULL   2   /* the chain, and the hostname against SAN   */

/* ---- status codes -------------------------------------------------------
 *
 * vossl_handshake:  1 done, -1 want-read, -2 want-write, 0 failed
 * vossl_read/write: >0 count, 0 clean EOF, -1 want-read, -2 want-write, -3 failed
 *
 * The want-read/want-write SPLIT is the load-bearing part and the reason these
 * are not simply "would block". TLS is not a byte pipe: SSL_read can need the
 * socket to become WRITABLE (a TLS 1.3 key update, a renegotiation) and
 * SSL_write can need it READABLE. A caller that always polls for readability
 * after a blocked read will hang, and only on a long-lived connection under
 * load. vyto/db/wire/tls.vt consumes exactly this distinction.
 */
#define VOSSL_WANT_READ   (-1)
#define VOSSL_WANT_WRITE  (-2)
#define VOSSL_FAILED      (-3)

#ifdef __cplusplus
extern "C" {
#endif

/* ---- library ------------------------------------------------------------ */
int  vossl_version(char *out, int cap);
int  vossl_available(void);

/* ---- contexts ----------------------------------------------------------- */
void *vossl_ctx_client(int verify);
void *vossl_ctx_server(const char *chain_pem, const char *key_pem, char *err, int cap);
int   vossl_ctx_min_version(void *ctx, const char *v);   /* "TLS1.2" | "TLS1.3" */
int   vossl_ctx_alpn(void *ctx, const char *csv);
int   vossl_ctx_ca_file(void *ctx, const char *path);
void  vossl_ctx_free(void *ctx);

/* ---- connections -------------------------------------------------------- */
void *vossl_conn_new(void *ctx, int fd, const char *host);
int   vossl_handshake(void *c, char *err, int cap);
int   vossl_read(void *c, void *dst, int n);
int   vossl_write(void *c, const void *src, int off, int n);
int   vossl_proto_version(void *c, char *out, int cap);
int   vossl_alpn_selected(void *c, char *out, int cap);
void *vossl_peer_cert(void *c);
int   vossl_last_error(void *c, char *out, int cap);
int   vossl_conn_shutdown(void *c);
void  vossl_conn_free(void *c);

/* ---- certificates ------------------------------------------------------- */
void *vossl_cert_from_pem(const char *pem);
void *vossl_cert_from_der(const void *der, int n);
int   vossl_cert_subject(void *x, char *out, int cap);
int   vossl_cert_issuer(void *x, char *out, int cap);
int   vossl_cert_serial(void *x, char *out, int cap);
long long vossl_cert_not_before(void *x);
long long vossl_cert_not_after(void *x);
int   vossl_cert_san_count(void *x);
int   vossl_cert_san_at(void *x, int i, char *out, int cap);
int   vossl_cert_matches_host(void *x, const char *host);
int   vossl_cert_fingerprint(void *x, void *out32);
void  vossl_cert_free(void *x);

/* ---- chain verification -------------------------------------------------
 *
 * A builder rather than two array parameters. Passing STACK_OF(X509) in from
 * Vyto would mean marshalling an array of rawptr, which the FFI has no shape
 * for; adding one certificate per call has none of that and costs one extra
 * entry point.
 */
void *vossl_store_new(void);
int   vossl_store_add(void *store, void *x);
void  vossl_store_free(void *store);
void *vossl_chain_new(void);
int   vossl_chain_add(void *chain, void *x);
void  vossl_chain_free(void *chain);
int   vossl_verify_chain(void *store, void *untrusted, void *leaf, char *err, int cap);

#ifdef __cplusplus
}
#endif

#endif /* OSSL_SHIM_H */
