/* vyto/media/image native backing — PNG/JPEG/BMP/TGA encoding.

   First-party. The upstream half is src/stb/stb_image_write.h, vendored
   byte-for-byte; see ../refresh-stb.sh.

   Two return conventions, both already used elsewhere in this tree:

     * Encode-to-file writes and returns 1/0. Nothing to own.
     * Encode-to-memory is opaque handle + borrowed pointer + explicit free,
       the vyto/net fetch convention (net.vt:26-32): vimg_encode_* returns a
       handle or NULL, vimg_buf_data/vimg_buf_len read it, vimg_buf_free
       releases it, and the pointer is invalid after that call.

   Pixels cross as (void *, w, h) of 32-bit 0xAARRGGBB in NATIVE byte order,
   which is what Vyto's i32[] holds. stb wants component order R,G,B,A in
   ascending memory, so the shim repacks rather than making callers care about
   endianness — see vimg_pack_rgba in the .c.

   Every entry point has a VT_NO_LIBC arm returning its failure sentinel, so a
   --freestanding build of any program that merely imports vyto/media/image
   still links. stb needs stdlib/stdio and cannot be compiled there. */

#ifndef VYTO_IMAGE_SHIM_H
#define VYTO_IMAGE_SHIM_H

/* ---- encode to a file: 1 on success, 0 on failure ---------------------- */

int vimg_write_png(const char *path, const void *px, int w, int h);
int vimg_write_bmp(const char *path, const void *px, int w, int h);
int vimg_write_tga(const char *path, const void *px, int w, int h);
/* quality is 1..100; stb clamps. JPEG has no alpha, so it is dropped. */
int vimg_write_jpg(const char *path, const void *px, int w, int h, int quality);

/* ---- encode to memory: handle, or NULL on failure --------------------- */

void *vimg_encode_png(const void *px, int w, int h);
void *vimg_encode_bmp(const void *px, int w, int h);
void *vimg_encode_tga(const void *px, int w, int h);
void *vimg_encode_jpg(const void *px, int w, int h, int quality);

/* Borrowed — invalid once vimg_buf_free is called. */
const void *vimg_buf_data(void *h);
int         vimg_buf_len(void *h);
void        vimg_buf_free(void *h);

/* Copy the encoded bytes into a caller-owned buffer of at least vimg_buf_len
   bytes, then free the handle. The Vyto side allocates that buffer as a byte[]
   and passes it as a cstring, because this shim has no -I to runtime/ and so
   cannot build a VtArray itself. Returns the number of bytes copied. */
int vimg_buf_take(void *h, void *dst, int cap);

/* ---- knobs ------------------------------------------------------------- */

/* stb's deflate level, 0..9, default 8. Set once; it is a global in stb, which
   is why this is a call and not a per-encode argument. */
void vimg_set_png_compression(int level);

/* 1 when this was built with a real libc, 0 on the freestanding arm. */
int vimg_available(void);

#endif /* VYTO_IMAGE_SHIM_H */
