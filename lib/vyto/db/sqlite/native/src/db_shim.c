/* vyto/db/sqlite — the C side.
 *
 * Opaque handle + explicit free, the file_shim.c convention: vdb_open and
 * vdb_prepare are the constructors, vdb_close and vdb_finalize are the frees,
 * and the Vyto side never learns what a sqlite3* is.
 *
 * THREE CONTRACTS, all of which exist because getting them wrong is a
 * use-after-free rather than a wrong answer:
 *
 *  1. ERROR CODES ARE OURS, NOT SQLITE'S. Every code crossing the boundary is
 *     one of the VDB_ERR_* below, mapped here. A SQLite version bump can then
 *     never change a value the Vyto side branches on or a golden test asserts.
 *     Same rule as regex_shim.c and intl_shim.c.
 *
 *  2. BUFFER-FILLING ENTRIES RETURN THE FULL LENGTH NEEDED. vdb_column_copy and
 *     vdb_errmsg write at most `cap` bytes and return how many the result
 *     actually needs. A return >= cap means truncated, and the Vyto caller
 *     retries once with a bigger bytes(n). Two-pass sizing, as regex_shim.c
 *     documents.
 *
 *  3. BINDING IS ALWAYS SQLITE_TRANSIENT. A Vyto .cstr() borrow is valid only
 *     to the end of the enclosing statement, and a bound value must outlive the
 *     step() that reads it — so SQLite copies. SQLITE_STATIC here would be a
 *     use-after-free with excellent performance.
 */

#include "sqlite_config.h"

/* Error classes, mirroring lib/vyto/db/driver.vt's DBERR_* exactly. */
#define VDB_ERR_NONE        0
#define VDB_ERR_OPEN        1
#define VDB_ERR_SQL         2
#define VDB_ERR_CONSTRAINT  3
#define VDB_ERR_BUSY        4
#define VDB_ERR_CLOSED      5
#define VDB_ERR_UNSUPPORTED 6
#define VDB_ERR_MISUSE      7
#define VDB_ERR_CANCELLED   8

/* Step outcomes, mirroring STEP_* in driver.vt. */
#define VDB_STEP_ROW  0
#define VDB_STEP_DONE 1
#define VDB_STEP_ERR  2

#ifdef VT_NO_LIBC
/* Freestanding: SQLite needs stdio, malloc and a filesystem, so there is
 * nothing to degrade to. Every entry point keeps its symbol — so a program that
 * imports the package still links — and returns its documented failure
 * sentinel. Deliberately no #include at all, so this arm cannot regress into
 * depending on a hosted header. The reference arm is
 * lib/vyto/mmap/native/src/mmap_shim.c. */

void      *vdb_open(const char *p, int ro, int create) { (void)p; (void)ro; (void)create; return 0; }
int        vdb_close(void *db)                      { (void)db; return -1; }
void      *vdb_prepare(void *db, const char *s, long long n) { (void)db; (void)s; (void)n; return 0; }
int        vdb_step(void *st)                       { (void)st; return VDB_STEP_ERR; }
int        vdb_reset(void *st)                      { (void)st; return -1; }
int        vdb_finalize(void *st)                   { (void)st; return -1; }
int        vdb_bind_null(void *st, int i)           { (void)st; (void)i; return -1; }
int        vdb_bind_int(void *st, int i, long long v) { (void)st; (void)i; (void)v; return -1; }
int        vdb_bind_double(void *st, int i, double v) { (void)st; (void)i; (void)v; return -1; }
int        vdb_bind_text(void *st, int i, const char *p, long long n) { (void)st; (void)i; (void)p; (void)n; return -1; }
int        vdb_bind_blob(void *st, int i, const char *p, long long n) { (void)st; (void)i; (void)p; (void)n; return -1; }
int        vdb_column_count(void *st)               { (void)st; return 0; }
int        vdb_column_type(void *st, int i)         { (void)st; (void)i; return 0; }
long long  vdb_column_int(void *st, int i)          { (void)st; (void)i; return 0; }
double     vdb_column_double(void *st, int i)       { (void)st; (void)i; return 0.0; }
long long  vdb_column_bytes(void *st, int i)        { (void)st; (void)i; return 0; }
long long  vdb_column_copy(void *st, int i, char *o, long long c) { (void)st; (void)i; (void)o; (void)c; return -1; }
long long  vdb_column_name(void *st, int i, char *o, long long c) { (void)st; (void)i; (void)o; (void)c; return -1; }
int        vdb_errcode(void *db)                    { (void)db; return VDB_ERR_UNSUPPORTED; }
long long  vdb_errmsg(void *db, char *o, long long c) { (void)db; (void)o; (void)c; return 0; }
int        vdb_stmt_errcode(void *st)               { (void)st; return VDB_ERR_UNSUPPORTED; }
long long  vdb_stmt_errmsg(void *st, char *o, long long c) { (void)st; (void)o; (void)c; return 0; }
long long  vdb_last_insert_rowid(void *db)          { (void)db; return 0; }
int        vdb_changes(void *db)                    { (void)db; return 0; }
int        vdb_exec(void *db, const char *sql)      { (void)db; (void)sql; return -1; }
int        vdb_busy_timeout(void *db, int ms)       { (void)db; (void)ms; return -1; }
void       vdb_interrupt(void *db)                  { (void)db; }
int        vdb_set_cancel_fd(void *db, int fd)      { (void)db; (void)fd; return -1; }
int        vdb_cancel_pending(void *db)             { (void)db; return 0; }
void       vdb_clear_cancel(void *db)               { (void)db; }

#else

#include "sqlite3/sqlite3.h"
#include <string.h>

#include <stdint.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#endif

/* Defined at the bottom with the rest of the cancellation machinery; vdb_close
 * needs it early to release the connection's slot. */
int vdb_set_cancel_fd(void *db, int fd);

/* SQLite result code -> our class. Extended codes carry the base in the low
 * byte, so mask before comparing: SQLITE_CONSTRAINT_UNIQUE is 2067, and a
 * driver that only tested == SQLITE_CONSTRAINT would miss every real one. */
static int vdb_map_err(int rc) {
    int base = rc & 0xff;
    switch (base) {
        case SQLITE_OK:
        case SQLITE_ROW:
        case SQLITE_DONE:       return VDB_ERR_NONE;
        case SQLITE_CONSTRAINT: return VDB_ERR_CONSTRAINT;
        /* Both routes to a stopped statement land here — sqlite3_interrupt and a
         * progress handler returning non-zero raise the same code — so one
         * mapping covers both. NOT busy: nobody should retry this. */
        case SQLITE_INTERRUPT:  return VDB_ERR_CANCELLED;
        case SQLITE_BUSY:
        case SQLITE_LOCKED:     return VDB_ERR_BUSY;
        case SQLITE_CANTOPEN:
        case SQLITE_NOTADB:
        case SQLITE_PERM:
        case SQLITE_READONLY:
        case SQLITE_AUTH:       return VDB_ERR_OPEN;
        case SQLITE_MISUSE:     return VDB_ERR_MISUSE;
        default:                return VDB_ERR_SQL;
    }
}

/* Copy `n` bytes into the caller's buffer, NUL-terminating when it fits, and
 * return the length the result actually needs. Contract 2 above. */
static long long vdb_fill(char *out, long long cap, const void *src, long long n) {
    if (out && cap > 0) {
        long long k = n < (cap - 1) ? n : (cap - 1);
        if (k > 0 && src) memcpy(out, src, (size_t)k);
        out[k] = 0;
    }
    return n;
}

void *vdb_open(const char *path, int readonly, int create) {
    sqlite3 *db = 0;
    int flags = readonly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE;
    if (!readonly && create) flags |= SQLITE_OPEN_CREATE;
    /* On failure sqlite3_open_v2 still hands back a handle carrying the error,
     * which is what lets the Vyto side report WHY rather than just "null". Only
     * an allocation failure yields NULL. */
    if (sqlite3_open_v2(path, &db, flags, 0) != SQLITE_OK) return db;
    return db;
}

/* close_v2, not close. A Cursor may still hold a live statement when the app
 * closes early; close_v2 marks the connection a zombie and frees it when the
 * last statement finalizes, where close would return SQLITE_BUSY and leak the
 * handle outright. */
int vdb_close(void *db) {
    if (!db) return 0;
    /* Release the cancel slot first: the table is keyed by handle address, and
     * a later connection allocated at the same address would otherwise inherit
     * this one's latch and abort its first statement. */
    vdb_set_cancel_fd(db, -1);
    return sqlite3_close_v2((sqlite3 *)db) == SQLITE_OK ? 0 : -1;
}

void *vdb_prepare(void *db, const char *sql, long long n) {
    sqlite3_stmt *st = 0;
    if (!db) return 0;
    /* The tail pointer is dropped deliberately: one statement per prepare. A
     * caller who passes two gets the first executed and the rest ignored, which
     * is the same thing every other binding does and is why execAll takes a
     * list rather than a blob. */
    if (sqlite3_prepare_v2((sqlite3 *)db, sql, (int)n, &st, 0) != SQLITE_OK) return 0;
    return st;
}

int vdb_step(void *st) {
    int rc;
    if (!st) return VDB_STEP_ERR;
    rc = sqlite3_step((sqlite3_stmt *)st);
    if (rc == SQLITE_ROW) return VDB_STEP_ROW;
    if (rc == SQLITE_DONE) return VDB_STEP_DONE;
    return VDB_STEP_ERR;
}

/* Clears bindings as well as rewinding. A statement returned to a cache with a
 * stale binding is a data-corruption bug waiting for the first call site that
 * binds fewer parameters than the last one did. */
int vdb_reset(void *st) {
    if (!st) return -1;
    sqlite3_clear_bindings((sqlite3_stmt *)st);
    return sqlite3_reset((sqlite3_stmt *)st) == SQLITE_OK ? 0 : -1;
}

int vdb_finalize(void *st) {
    if (!st) return 0;
    return sqlite3_finalize((sqlite3_stmt *)st) == SQLITE_OK ? 0 : -1;
}

int vdb_bind_null(void *st, int i) {
    if (!st) return -1;
    return sqlite3_bind_null((sqlite3_stmt *)st, i) == SQLITE_OK ? 0 : -1;
}

int vdb_bind_int(void *st, int i, long long v) {
    if (!st) return -1;
    return sqlite3_bind_int64((sqlite3_stmt *)st, i, (sqlite3_int64)v) == SQLITE_OK ? 0 : -1;
}

int vdb_bind_double(void *st, int i, double v) {
    if (!st) return -1;
    return sqlite3_bind_double((sqlite3_stmt *)st, i, v) == SQLITE_OK ? 0 : -1;
}

/* SQLITE_TRANSIENT on both: contract 3. */
int vdb_bind_text(void *st, int i, const char *p, long long n) {
    if (!st) return -1;
    return sqlite3_bind_text((sqlite3_stmt *)st, i, p, (int)n, SQLITE_TRANSIENT) == SQLITE_OK ? 0 : -1;
}

int vdb_bind_blob(void *st, int i, const char *p, long long n) {
    if (!st) return -1;
    /* A zero-length blob must still be a blob, not NULL, and sqlite3_bind_blob
     * with a null pointer binds NULL — so route the empty case explicitly. */
    if (n == 0) return sqlite3_bind_zeroblob((sqlite3_stmt *)st, i, 0) == SQLITE_OK ? 0 : -1;
    return sqlite3_bind_blob((sqlite3_stmt *)st, i, p, (int)n, SQLITE_TRANSIENT) == SQLITE_OK ? 0 : -1;
}

int vdb_column_count(void *st) {
    if (!st) return 0;
    return sqlite3_column_count((sqlite3_stmt *)st);
}

/* SQLite's type codes are 1..5; ours are 0..4 with NULL at 0. Mapped here so
 * the Vyto side never sees a SQLITE_* constant. */
int vdb_column_type(void *st, int i) {
    int t;
    if (!st) return 0;
    t = sqlite3_column_type((sqlite3_stmt *)st, i);
    switch (t) {
        case SQLITE_INTEGER: return 1;
        case SQLITE_FLOAT:   return 2;
        case SQLITE_TEXT:    return 3;
        case SQLITE_BLOB:    return 4;
        default:             return 0;   /* SQLITE_NULL */
    }
}

long long vdb_column_int(void *st, int i) {
    if (!st) return 0;
    return (long long)sqlite3_column_int64((sqlite3_stmt *)st, i);
}

double vdb_column_double(void *st, int i) {
    if (!st) return 0.0;
    return sqlite3_column_double((sqlite3_stmt *)st, i);
}

/* Byte length of a TEXT or BLOB column.
 *
 * The order matters and is not interchangeable: calling sqlite3_column_bytes
 * before the matching _text/_blob can report the length of a different
 * representation, because SQLite converts in place and the conversion is what
 * sets the length. So each branch fetches the pointer first, then the size. */
long long vdb_column_bytes(void *st, int i) {
    sqlite3_stmt *s = (sqlite3_stmt *)st;
    if (!s) return 0;
    if (sqlite3_column_type(s, i) == SQLITE_BLOB) {
        (void)sqlite3_column_blob(s, i);
        return (long long)sqlite3_column_bytes(s, i);
    }
    (void)sqlite3_column_text(s, i);
    return (long long)sqlite3_column_bytes(s, i);
}

/* Copy a TEXT or BLOB column's bytes out.
 *
 * This is the function that makes "a Row owns its cells" true. The pointer
 * SQLite returns here is invalidated by the next step/reset/finalize on this
 * statement, so it must never reach Vyto — only these copied bytes do. */
long long vdb_column_copy(void *st, int i, char *out, long long cap) {
    sqlite3_stmt *s = (sqlite3_stmt *)st;
    const void *p;
    long long n;
    if (!s) return -1;
    if (sqlite3_column_type(s, i) == SQLITE_BLOB) {
        p = sqlite3_column_blob(s, i);
        n = (long long)sqlite3_column_bytes(s, i);
    } else {
        p = (const void *)sqlite3_column_text(s, i);
        n = (long long)sqlite3_column_bytes(s, i);
    }
    return vdb_fill(out, cap, p, n);
}

long long vdb_column_name(void *st, int i, char *out, long long cap) {
    const char *nm;
    if (!st) return -1;
    nm = sqlite3_column_name((sqlite3_stmt *)st, i);
    if (!nm) return vdb_fill(out, cap, "", 0);
    return vdb_fill(out, cap, nm, (long long)strlen(nm));
}

int vdb_errcode(void *db) {
    if (!db) return VDB_ERR_CLOSED;
    return vdb_map_err(sqlite3_extended_errcode((sqlite3 *)db));
}

long long vdb_errmsg(void *db, char *out, long long cap) {
    const char *m;
    if (!db) return vdb_fill(out, cap, "no connection", 13);
    m = sqlite3_errmsg((sqlite3 *)db);
    if (!m) return vdb_fill(out, cap, "", 0);
    return vdb_fill(out, cap, m, (long long)strlen(m));
}

/* A statement's error is its connection's error — sqlite3_db_handle is what
 * saves the Vyto side from carrying a back-pointer it would otherwise need,
 * which is what would have made Stmt->Conn a reference cycle. */
int vdb_stmt_errcode(void *st) {
    if (!st) return VDB_ERR_CLOSED;
    return vdb_errcode(sqlite3_db_handle((sqlite3_stmt *)st));
}

long long vdb_stmt_errmsg(void *st, char *out, long long cap) {
    if (!st) return vdb_fill(out, cap, "no statement", 12);
    return vdb_errmsg(sqlite3_db_handle((sqlite3_stmt *)st), out, cap);
}

long long vdb_last_insert_rowid(void *db) {
    if (!db) return 0;
    return (long long)sqlite3_last_insert_rowid((sqlite3 *)db);
}

int vdb_changes(void *db) {
    if (!db) return 0;
    return sqlite3_changes((sqlite3 *)db);
}

int vdb_exec(void *db, const char *sql) {
    if (!db) return -1;
    /* No callback and no errmsg out-param: the caller reads the error through
     * vdb_errmsg, so there is no sqlite3_free obligation crossing the FFI. */
    return sqlite3_exec((sqlite3 *)db, sql, 0, 0, 0) == SQLITE_OK ? 0 : -1;
}

int vdb_busy_timeout(void *db, int ms) {
    if (!db) return -1;
    return sqlite3_busy_timeout((sqlite3 *)db, ms) == SQLITE_OK ? 0 : -1;
}

/* ---- cancellation --------------------------------------------------------
 *
 * SQLite has no timeout and no "stop this query" message: a long scan runs to
 * completion inside sqlite3_step, and the calling thread is inside that call.
 * There are exactly two ways out, and this exposes both.
 *
 * sqlite3_interrupt is the direct one, but it has to be called from ANOTHER
 * thread while step() is running — which a language with no threads cannot do.
 * It is still exported here for the case where it can be reached (a signal
 * handler, a second entry point into the same process).
 *
 * The usable one is a progress handler: SQLite calls it every N virtual-machine
 * instructions, and a non-zero return aborts the statement with SQLITE_INTERRUPT.
 * That gives a running query somewhere to check for "stop" — and since the
 * checker lives here in C, it can watch a file descriptor without the query ever
 * returning to Vyto. Point that fd at a pipe from whoever wants to cancel (the
 * parent process, over a worker's control channel) and a query becomes
 * interruptible from outside the process running it.
 *
 * The signal is LATCHED, and this is not incidental. The byte on the fd is
 * consumed on the first sighting; without that it stays readable forever and
 * every subsequent statement on the connection would abort the moment it began.
 * Clearing the latch is the caller's explicit act, so a cancel ends exactly one
 * statement and the connection stays usable — which is the whole reason to
 * cancel a query rather than kill the process running it. */

/* Roughly every 20k VM instructions: sub-millisecond in practice, while keeping
 * the poll syscall far off the hot path. */
#define VDB_PROGRESS_OPS 20000
#define VDB_CANCEL_SLOTS 16

typedef struct {
    void *db;       /* which connection, or NULL for a free slot */
    int   fd;       /* watched for readability; -1 disables */
    int   flagged;  /* latch: seen, consumed, still in force */
} vdb_cancel;

static vdb_cancel g_cancel[VDB_CANCEL_SLOTS];

static vdb_cancel *vdb_cancel_find(void *db) {
    for (int i = 0; i < VDB_CANCEL_SLOTS; i++) {
        if (g_cancel[i].db == db) return &g_cancel[i];
    }
    return 0;
}

/* 1 = readable, 0 = not, and any error reads as "not" so a broken pipe cannot
 * masquerade as a cancel and abort every query on the connection. */
static int vdb_readable(int fd) {
    if (fd < 0) return 0;
#ifdef _WIN32
    fd_set rf;
    struct timeval tv;
    FD_ZERO(&rf);
    FD_SET((SOCKET)(intptr_t)fd, &rf);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    return select(0, &rf, 0, 0, &tv) > 0 ? 1 : 0;
#else
    struct pollfd p;
    int rc;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    do { rc = poll(&p, 1, 0); } while (rc < 0 && errno == EINTR);
    return rc > 0 ? 1 : 0;
#endif
}

static void vdb_drain(int fd) {
    char sink[64];
#ifdef _WIN32
    recv((SOCKET)(intptr_t)fd, sink, (int)sizeof sink, 0);
#else
    ssize_t r;
    do { r = read(fd, sink, sizeof sink); } while (r < 0 && errno == EINTR);
#endif
}

static int vdb_progress(void *p) {
    vdb_cancel *c = (vdb_cancel *)p;
    if (!c) return 0;
    if (c->flagged) return 1;
    if (vdb_readable(c->fd)) {
        vdb_drain(c->fd);
        c->flagged = 1;
        return 1;
    }
    return 0;
}

/* Ask SQLite to abandon whatever `db` is running. Only does anything when
 * called while another thread is inside step(); see the note above. */
void vdb_interrupt(void *db) {
    if (db) sqlite3_interrupt((sqlite3 *)db);
}

/* Watch `fd` for a cancel signal on this connection. fd < 0 removes the handler.
 * 0 on success, -1 if the table is full (or db is null). */
int vdb_set_cancel_fd(void *db, int fd) {
    if (!db) return -1;
    vdb_cancel *c = vdb_cancel_find(db);
    if (fd < 0) {
        if (c) {
            sqlite3_progress_handler((sqlite3 *)db, 0, 0, 0);
            c->db = 0; c->fd = -1; c->flagged = 0;
        }
        return 0;
    }
    if (!c) c = vdb_cancel_find(0);
    if (!c) return -1;
    c->db = db;
    c->fd = fd;
    c->flagged = 0;
    sqlite3_progress_handler((sqlite3 *)db, VDB_PROGRESS_OPS, vdb_progress, c);
    return 0;
}

/* Whether the latch is set — i.e. whether the last statement stopped because
 * someone asked, rather than because it failed. */
int vdb_cancel_pending(void *db) {
    vdb_cancel *c = vdb_cancel_find(db);
    return (c && c->flagged) ? 1 : 0;
}

/* Release the latch so the connection accepts statements again. */
void vdb_clear_cancel(void *db) {
    vdb_cancel *c = vdb_cancel_find(db);
    if (c) c->flagged = 0;
}

#endif /* VT_NO_LIBC */
