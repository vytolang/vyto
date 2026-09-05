/* zlib_config.h — the build options for the vendored zlib.
 *
 * First-party. A refresh of native/src/zlib/ does not touch this file, and
 * local build choices belong here rather than in the vendored tree.
 *
 * INCLUDED BY BOTH the wrapper TUs and zlib_shim.c, and that is load-bearing:
 * Z_SOLO changes which prototypes zlib.h declares, so a shim compiled without
 * it and a library compiled with it would disagree about the API. The same trap
 * lib/vyto/db/sqlite/native/src/sqlite_config.h:1-12 records for SQLITE_OMIT_*.
 *
 * There is no #cflags pragma in Vyto — #link is the only directive
 * (src/parse.c:1030) — so every -D zlib needs has to come from a C file.
 *
 * Every value here is identical on every triple. A host-derived option would
 * let the same input compress differently per platform and break the goldens,
 * which is the rule lib/vyto/regex/native/src/config.h:9-12 states.
 */
#ifndef VYTO_ZLIB_CONFIG_H
#define VYTO_ZLIB_CONFIG_H

/* Z_SOLO drops the gzopen/gzread stdio file API. This package works on byte
 * buffers and never opens a file, so the four gz*.c are not vendored at all —
 * which also keeps libc file I/O out of the dependency surface.
 *
 * It does NOT drop gzip FRAMING: deflateInit2 with windowBits +16 still writes
 * a gzip header and CRC-32 trailer, because that lives in deflate.c behind
 * GZIP (deflate.h:22-23), not behind Z_SOLO. compress()/uncompress()/
 * compressBound() also survive. */
#define Z_SOLO

#endif /* VYTO_ZLIB_CONFIG_H */
