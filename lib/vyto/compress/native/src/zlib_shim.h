/* zlib_shim.h — the C side of vyto/compress.
 *
 * First-party and hand-maintained: a refresh of the vendored tree does not
 * touch this file. Local changes belong here, never in native/src/zlib/.
 *
 * ---- the two conventions this ABI uses ----
 *
 * 1. OUTPUT COMES BACK AS A HANDLE, NOT AN ARRAY. This shim has no -I to
 *    runtime/ and so cannot build a VtArray. Compressed and decompressed sizes
 *    are also not knowable in advance, so a caller-sized buffer would be a
 *    guess. Instead an entry point returns an opaque handle; the Vyto side asks
 *    its length, allocates a byte[] of that size, and copies the bytes out.
 *    Same arrangement as lib/vyto/media/image/native/src/image_shim.h:34-50.
 *
 * 2. vzl_buf_take FREES THE HANDLE, even when the copy is short. There is
 *    therefore no path through the Vyto wrapper that leaks, and no second call
 *    is needed on the success path.
 *
 * ---- errors ----
 *
 * An entry point that fails returns NULL. The reason is fetched separately with
 * vzl_last_error(), which returns a borrowed static string that is valid until
 * the next call on the same thread. Decompression failure is an ordinary
 * outcome here, not a bug: the input is untrusted bytes.
 *
 * ---- freestanding ----
 *
 * Every entry point has a VT_NO_LIBC arm returning its documented failure
 * sentinel, so a --freestanding program that merely imports vyto/compress still
 * links. vzl_available() reports which arm was compiled, and is what lets
 * compress.vt stay free of platform conditionals.
 */
#ifndef VYTO_ZLIB_SHIM_H
#define VYTO_ZLIB_SHIM_H

/* Stream framings. The numbering is this shim's own and is mirrored by the
 * FMT_* constants in compress.vt — keep the two in step. */
#define VZL_RAW  0   /* bare DEFLATE, no header or checksum (RFC 1951) */
#define VZL_ZLIB 1   /* zlib wrapper, Adler-32                (RFC 1950) */
#define VZL_GZIP 2   /* gzip wrapper, CRC-32                  (RFC 1952) */

/* ---- one-shot: whole buffer in, handle out (NULL on failure) ---- */

/* level is 0-9; 6 is zlib's default. */
void *vzl_deflate(const void *src, int n, int format, int level);

/* limit caps the output size in bytes and refuses anything larger, so a
 * decompression bomb fails instead of exhausting memory. 0 means no limit. */
void *vzl_inflate(const void *src, int n, int format, int limit);

/* ---- streaming: a z_stream driven a chunk at a time ---- */

void *vzl_stream_new(int compressing, int format, int level);

/* Feed one chunk. Returns 0 on success, negative on error. Output accumulates
 * inside the stream object; collect it with vzl_stream_take. */
int   vzl_stream_push(void *s, const void *src, int n);

/* Finish the stream: flush, and for decompression verify the trailer. Returns
 * 0 on success, negative on error. */
int   vzl_stream_finish(void *s);

/* Bytes produced but not yet collected. */
int   vzl_stream_pending(void *s);

/* Copy out at most cap pending bytes; returns the number copied. Unlike
 * vzl_buf_take this does NOT free the stream — a stream is collected many
 * times over its life. */
int   vzl_stream_take(void *s, void *dst, int cap);

/* Idempotent; safe on NULL. */
void  vzl_stream_free(void *s);

/* ---- output buffer handles ---- */

int   vzl_buf_len(void *h);

/* Copy up to cap bytes into a caller-owned buffer, then free the handle.
 * Returns the number of bytes copied. The Vyto side allocates that buffer as a
 * byte[] and passes it here, because this shim cannot build a VtArray itself. */
int   vzl_buf_take(void *h, void *dst, int cap);

/* Only for a handle that is being abandoned without a copy. */
void  vzl_buf_free(void *h);

/* ---- diagnostics ---- */

/* Borrowed; valid until the next shim call on this thread. Never NULL. */
const char *vzl_last_error(void);

/* 0 on a --freestanding build, where the whole library is compiled out. */
int   vzl_available(void);

/* The vendored zlib's version string, e.g. "1.3.1". Never NULL. */
const char *vzl_version(void);

#endif /* VYTO_ZLIB_SHIM_H */
