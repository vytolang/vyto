/* Wrapper translation unit for the vendored SQLite amalgamation.
 *
 * The compiler globs native/src/*.c FLAT and non-recursively with a fixed
 * command line, so the upstream tree sits one directory down where the glob
 * cannot see it, and this file is what actually compiles it — carrying the
 * defines that no command line can supply. Same shape as
 * vyto/crypto/ecc's vecc_uecc.c and vyto/regex's vregex_*.c.
 *
 * Do not edit sqlite3/sqlite3.c. Local changes belong in db_shim.c or in
 * sqlite_config.h, so that refresh-sqlite.sh can replace the vendored tree
 * wholesale. `refresh-sqlite.sh --verify` fails if it has been touched.
 */

#include "sqlite_config.h"

/* --freestanding splices -DVT_NO_LIBC into every package shim's compile line.
 * SQLite is built on stdio, malloc and the filesystem, so there is nothing to
 * degrade to — the whole library drops out and db_shim.c's VT_NO_LIBC arm
 * returns failure sentinels instead. Without this guard, merely importing
 * vyto/db/sqlite would break any freestanding build. */
#ifndef VT_NO_LIBC
#include "sqlite3/sqlite3.c"
#endif
