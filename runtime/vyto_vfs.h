/* Vyto embedded-asset registry.
 *
 * Populated at startup by the generated vyto_assets.c when a build passes
 * --with-assets; empty otherwise, so every lookup misses and callers fall
 * through to disk with one branch. Keys are logical paths relative to the app
 * root, e.g. "assets/logo.png". Amalgamated into vyto_rt (see vyto_rt.c), so
 * the symbols live in the runtime object for the generated asset TU to link. */
#ifndef VYTO_VFS_H
#define VYTO_VFS_H

/* Register one embedded blob. `logical`/`data` are expected to be static (owned
 * by the generated TU / process lifetime); the registry does not copy them. */
void vt_vfs_register(const char *logical, const unsigned char *data, long len);

/* Look up a blob. Matches `key` exactly, or an absolute/longer path whose tail
 * is "/"+key (so "assets/x" and "/app/assets/x" both resolve). Returns 1 and
 * fills *out/*out_len on hit, 0 on miss. out/out_len may be NULL. */
int vt_vfs_get(const char *key, const unsigned char **out, long *out_len);
int vt_vfs_has(const char *key);

/* Volt-friendly accessors: a bare data pointer (NULL on miss) and byte length
   (-1 on miss), so .vt code can query the registry via simple extern decls. */
const unsigned char *vt_vfs_ptr(const char *key);
long vt_vfs_size(const char *key);

/* Copy up to `cap` bytes of the asset into `buf`; returns bytes copied, or -1
   on miss. Lets .vt build a byte[] without exposing the raw pointer. */
long vt_vfs_read(const char *key, unsigned char *buf, long cap);

/* Registry iteration (for asset listing). vt_vfs_key returns NULL out of range.
   Keys are the logical paths as registered, e.g. "assets/logo.png". */
int vt_vfs_count(void);
const char *vt_vfs_key(int i);

#endif
