/* Build options for the vendored SQLite amalgamation.
 *
 * Included by BOTH vsqlite3.c (which compiles the amalgamation) and db_shim.c
 * (which calls it). That is not belt-and-braces: SQLITE_OMIT_* changes which
 * prototypes sqlite3.h declares, so a shim compiled without these defines and a
 * library compiled with them disagree about the API. The same reason
 * vyto/crypto/ecc's uecc_config.h is included by both of its translation units.
 *
 * There is no #cflags pragma in Vyto — #link is the only one — so every -D has
 * to arrive from a C file. That is why this header exists at all rather than
 * being a compiler flag.
 */

#ifndef VYTO_SQLITE_CONFIG_H
#define VYTO_SQLITE_CONFIG_H

/* Vyto has no threads and reference counting is not atomic, so a connection
 * cannot be shared across one anyway. Turning this off removes every mutex
 * operation from the hot path — the single most valuable option here. */
#define SQLITE_THREADSAFE 0

/* No dlopen, and therefore no -ldl on the link line. Loading an extension is
 * also an obvious attack surface for a library that may sit behind a server. */
#define SQLITE_OMIT_LOAD_EXTENSION 1

/* Double-quoted strings are IDENTIFIERS, never string literals. SQLite's legacy
 * misfeature silently turns a typo'd column name into a string constant, so
 * `WHERE "nmae" = 'x'` compares the literal "nmae" to 'x' and quietly matches
 * nothing. vyto/db quotes every identifier with double quotes, so this must be
 * off for that to be unambiguous. */
#define SQLITE_DQS 0

/* Nothing calls sqlite3_status(), and the counters cost an add on every
 * allocation. */
#define SQLITE_DEFAULT_MEMSTATUS 0

/* Nothing in the shim uses the pre-2007 API. */
#define SQLITE_OMIT_DEPRECATED 1

/* No sqlite3_trace/profile hooks are exposed by the driver. */
#define SQLITE_OMIT_TRACE 1

/* Keep the shared-cache misfeature off; it is deprecated upstream and
 * interacts badly with the one-connection-per-worker model. */
#define SQLITE_OMIT_SHARED_CACHE 1

/* WAL is the whole reason a server-side SQLite is usable under concurrency, so
 * it stays in. Left explicit so nobody "optimises" it out with the others. */
/* (SQLITE_OMIT_WAL is deliberately NOT defined.) */

#endif /* VYTO_SQLITE_CONFIG_H */
