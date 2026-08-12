# vyto/db — SQL databases, with the statement handle owned by the language

Every app that needed to persist structured state used to hand-roll a file
format, because the tree had no database at all. This is that gap closed:
a fluent query and schema builder, a `Value`/`Row` model, and a SQLite driver
with the amalgamation vendored, so it builds on a fresh clone with no `-dev`
package and nothing to provision.

The binding is the easy half. The value is the layer on top: `Db`, `Stmt` and
`Row` are reference-counted, so a statement is finalized and a connection closed
the moment the last reference drops — on an early return, on a `panic`-free
bail-out, on any path at all. There is no finalizer queue and no `defer` to
forget.

```vyto
import { Db, select, insert, table, dbText, dbInt } from "vyto/db";
import { sqlite } from "vyto/db/sqlite";

let db = new Db(sqlite("app.db").wal(true).busyTimeout(5000));

let t = table("users").ifNotExists();
t.id();
t.text("email").notNull().unique();
db.execAll(t.statements(db.dialect()));

db.exec(insert("users").set("email", dbText("ada@example.com")));

let r = db.all(select("users").where("id", dbInt(1)).orderBy("email"));
for (let row in r) { print(row.getStr("email")); }     // ada@example.com

let c = db.cursor(select("events").orderBy("id"));      // stream, don't collect
while (c.next()) { total += c.row().getInt("n"); }
```

## Modules

| Module | What it gives you |
|---|---|
| `vyto/db` | the barrel: `Db`, `Result`, `Cursor`, `Tx`, and everything below |
| `vyto/db/value` | `Value` (null/int/float/text/blob) and `Row` |
| `vyto/db/dialect` | `Dialect` and the SQLite / Postgres / MySQL flavours |
| `vyto/db/driver` | `Driver`, `Conn`, `Stmt` — what a driver implements |
| `vyto/db/query` | `select` / `insert` / `update` / `deleteFrom` |
| `vyto/db/schema` | `table` / `dropTable` / `alterTable` — DDL, no migration runner |
| `vyto/db/introspect` | reading a schema back out: `TableInfo`, `ColumnInfo`, `IndexInfo`, `ForeignKey` |
| `vyto/db/sqlite` ⚙ | the SQLite driver. **Vendored**, nothing to install |
| `vyto/db/wire` | byte-level floor shared by the network drivers: frame building, endian conversion, tolerant parsing, DSN parsing |
| `vyto/db/wire/io` | the framed transport — `WireIo`, `SockIo`, and `MemIo` for testing without a server |
| `vyto/db/pgsql` | the PostgreSQL driver. **Pure Vyto**, speaks the v3 wire protocol; no libpq, nothing to install |
| `vyto/db/mysql` | declared, every entry panics |

`import { … } from "vyto/db"` re-exports all of it except a driver. Importing an
individual module pulls in less.

## Three rules the package is built on

**The app names the driver; no library ever does.** Native sources compile per
package *directory* for every module in the transitive import closure, so a
`vyto/db` that imported the drivers would build the 9.5 MB SQLite amalgamation —
and the socket shim behind the Postgres driver — into every program that merely
wanted to render a query string. Hence two imports at the top of an app, and hence:

1. `db.vt` never imports a driver, not even as a default.
2. There is never a `drivers.vt` barrel importing all three.
3. **No `db_open("sqlite://app.db")` scheme dispatcher.** It would require
   `db.vt` to construct a `SqliteConn`. A scheme is a runtime string; the link
   line is a compile-time fact. The one-line cost of naming the driver at the
   import site is the price, and it is the same price `vyto/crypto/ecc` and
   `vyto/util/uuid` already charge.

This is verifiable, not aspirational, and `tests/run_tests.sh` checks it both
ways: `db_no_driver_contagion` builds a driverless program and fails if any
SQLite *or* socket object was produced for it, and `pgsql_no_sqlite` builds a
Postgres-only program and fails if any SQLite object was.

**A row owns its cells.** `sqlite3_column_text` returns a pointer invalidated by
the next `step()`, so every cell is copied into Vyto storage before control
returns. A `Row` may therefore outlive its statement, its connection, and the
whole `Db` — `examples/103_db.vt` closes the database and then reads a saved row
to prove it. The `Cursor` is the only object with a lifetime worth thinking
about, and it holds both handles itself.

**Values are bound, never interpolated.** There is no `quoteValue` anywhere.
Every value reaches the engine as a parameter, which is what makes injection
structurally impossible rather than a matter of remembering to escape.
`quoteIdent` exists for table and column names — which come from your source —
and it *panics* on anything that is not a plain identifier, because a caller
passing user input as a column name has written the bug this package prevents.

## Things worth knowing before you use these

> **A synchronous query parks the whole worker.** Vyto has no threads, so a
> query blocks the process — and behind `vyto/net/server`'s pre-fork reactor,
> that worker's other connections wait too. It is the main reason the Postgres
> and MySQL drivers are stubs rather than half-built.
>
> SQLite escapes this **only while the working set stays in page cache.** Once
> the database is larger than RAM the assumption fails and the numbers are not
> close: a cold random row fetch measured 8.8 ms, a `COUNT(*)` over ten tables
> 35 s. Past ~15 s the requesting connection is dropped as well — see *A slow
> query can cost you the connection* below.

> **There is no connection pool, and there cannot be one.** Pre-fork workers
> share nothing, so it is one connection per worker for the worker's life.

> **An `sqlite3` handle must not cross a `fork()`.** Open the connection in the
> worker, after the pre-fork — not in the parent.

> **`Db.begin()` panics if a transaction is already open.** SQLite has no
> nested `BEGIN`, and silently making it a no-op would let an inner rollback
> discard the outer transaction's work. Savepoints are the real answer and are
> deliberately not in v1.

> **`Tx` rolls back in `deinit`.** An early return that never commits is undone
> when the Tx goes out of scope. That is the feature, but it means a `Tx` you
> stash in a long-lived field will hold the transaction open for as long as you
> hold the reference.

> **The statement cache hands out a statement only when it is not in use.** Two
> live cursors over the same SQL get two statements rather than trampling one.
> Cap it with `.stmtCache(n)`; `0` disables. **It has no eviction** — once full
> it stops caching rather than making room, which is silent. See *The statement
> cache has no eviction* below.

> **Wrong-kind reads return a typed zero, not an error.** `getInt` on a text
> column is `0`, an unknown column is NULL, an out-of-range index is NULL. Text
> is never parsed into a number — a silent parse is how `"12abc"` becomes `12`.

## Introspection — what is actually in the database

Everything above builds SQL against a schema you already know. `db.describe()`
answers the other question.

```vyto
for (let name in db.tables()) { print(name); }

let t = db.describe("users");
if (t.exists()) {
    print(t.describe());                  // a readable dump
    for (let c in t) {                    // iterates the columns
        print(c.name + " " + c.declType + " pk=" + c.pkPos);
    }
    for (let k in t.primaryKey()) { print("key: " + k.name); }
}

let s = db.schema();                      // every table, view and trigger
```

| | |
|---|---|
| `db.supportsIntrospection()` | SQLite and Postgres yes; MySQL panics if asked |
| `db.tables()` / `db.views()` | names, engine internals filtered, never null |
| `db.hasTable(name)` | existence, without quoting the name |
| `db.describe(name)` | `TableInfo` — columns, PK, indexes, foreign keys, DDL text |
| `db.triggers()` / `db.objectSql(name)` | |
| `db.schema()` | everything at once |

**Every call binds the object name as a parameter.** None of them splices it
into SQL, which is what makes it safe to pass a name that came from a user.
That is possible only because these are `SELECT`s over SQLite's `pragma_*`
table-valued functions rather than bare `PRAGMA` statements — **`PRAGMA` takes
no bind parameters**, its grammar admits a name or a number and never an
expression, so `PRAGMA table_info(?)` is a syntax error. The bare form would
force interpolating the table name, through the `quoteIdent` that panics.

> **This is the answer to the `quoteIdent` trap.** `quoteIdent` aborts the
> process on anything outside `[A-Za-z_][A-Za-z0-9_$]*` — but a database will
> happily contain a table called `odd-name` and `db.tables()` will hand it to
> you. **`isIdent(name)`** is now exported for exactly this: test a name before
> you build a query from it.
>
> ```vyto
> for (let t in db.tables()) {
>     if (!isIdent(t)) { continue; }        // cannot go through select()
>     let r = db.all(select(t).limit(10));
> }
> ```

> **A missing table is an answer, not an error.** `describe("nope")` returns a
> `TableInfo` whose `exists()` is false — never null, never a panic. The
> pragmas cannot tell an absent table from an empty one (both are zero rows),
> so existence is established first.

> **`ColumnInfo.declType` is the truth; `kindGuess()` is a guess.** The declared
> type is kept verbatim — `VARCHAR(255)`, `NUMERIC(10,2)`, or `""` for an
> untyped column. `kindGuess()` applies SQLite's affinity rules to map it onto a
> `COL_*`, and `kindName()` gives a stable string for tests. Neither is
> reversible; keep `declType` for anything that matters.

> **`pkPos` is a position, not a flag.** 1-based within the primary key, 0 when
> not a member. `primaryKey()` returns the columns *in key order*, which is not
> column order — `PRIMARY KEY (tenant_id, user_id)` on a table declaring
> `user_id` first is the case that catches a reader assuming otherwise.

> **An `INTEGER PRIMARY KEY` has no index at all.** It is the rowid alias, so
> `indexes` is empty for it and the key is visible only through `pkPos`. An
> index whose `origin` is not `IDX_APPDEF` was created by the engine to enforce
> a `UNIQUE` constraint or a primary key — `isImplicit()` says so.

**`TableInfo.toTable()` bridges to the DDL builder and is lossy on purpose.** It
exists for "make something like this elsewhere", not for round-tripping. It
drops the exact type string (`NUMERIC(10,2)` re-renders as `REAL`), collations,
generated expressions, CHECK constraints, partial-index predicates, index sort
order, and **composite primary keys** — `Column.pkFlag` is a bool with no
ordinal, so two key columns would emit two `PRIMARY KEY` clauses and produce
invalid DDL. It drops them rather than rendering them wrong. `TableInfo.sql`
holds the original text when fidelity matters.

Two limits worth stating plainly. **A generated column's expression is not
recoverable** from any pragma — SQLite forces its `dflt_value` to NULL — so it
lives only in `objectSql()`. And **Postgres and MySQL panic**: their
introspection SQL would be untestable without a server, and the package would
rather have a clear abort than ship a large query nobody has run. Ask
`supportsIntrospection()` first.

## What things cost

Measured by `apps/db_test`, which loads a hundred million rows through this
package and serves them: ten tables of ten million, `--release`, on a 5400 RPM
laptop disk with 7.6 GB of RAM. **Read the ratios, not the absolutes** — the
throughput numbers are this machine's, the multiples are the package's.

| | |
|---|---|
| prepared statement, rebound in a loop | 2,017,642 inserts/s — 0.50 µs/row |
| raw `Stmt.step()` + `columnValue(i)` scan | 14,290,568 rows/s |
| `Cursor` + `row().at(0)` | 7,352,949 rows/s — **+96%** |
| `Cursor` + `row().getInt("uid")` | 7,302,539 rows/s — **+101%** |
| one row per transaction, `synchronous=FULL` | 7 rows/s |

### `Cursor` costs about 2×, and it is the allocation

`Cursor.next()` builds a fresh `Row` and one `Value` per column on every step
(`db.vt:213`). Splitting the two overheads apart over ten million rows:

- `Row` + `Value` allocation — **73 ns/row**
- the linear scan of column names in `Row.get` (`value.vt:185`) — **3.7 ns/row**

So the name lookup is ~5% of the gap and the allocation is ~95%. Two things
follow. Reaching for `row.at(i)` over `row.getStr("name")` to "avoid the string
compare" buys almost nothing — keep the readable one. And optimising `Row.get`
into a hash would be attacking the wrong 5%.

**When a scan is genuinely the hot path, drop to the `Stmt`:**

```vyto
let cn = db.conn();
let st = cn.prepare("SELECT uid FROM events");
while (st.step() == STEP_ROW) { total += st.columnValue(0).asInt(); }
cn.release(st);                      // hand it back; do not finalize it yourself
```

That is roughly twice the throughput and no per-row garbage. Everywhere else,
7M rows/s is not slow, and `Cursor` reads better.

### `db.exec(insert(...))` re-renders the SQL on every call

The ergonomic path is not free per row (`db.vt:272`): the builder allocates,
`render` runs `quoteIdent` over every column into a fresh `StringBuilder`,
`toString` copies it out, and the statement cache then finds it by comparing
that string against its entries one at a time (`sqlite/sqlite.vt:257`). The
`sqlite3_prepare_v2` is avoided from the second call onward; nothing else is.

Fine for a request handler inserting one row. For a bulk load, render once and
rebind — `insert(...).render(db.dialect())` outside the loop, `cn.prepare(sql)`
once, then `bind`/`step`/`reset`. Reuse the `Value` objects too: their fields
are public, so `v.i = n` replaces an allocation per column per row.

> **`bind` is 1-based; `columnValue` and `columnName` are 0-based.** They are
> SQLite's own conventions, kept rather than papered over.

### Transaction granularity is the single biggest write lever

Five orders of magnitude separate the two ends of it:

| | |
|---|---|
| `db.begin()` per row, `synchronous=FULL` | 7 rows/s |
| commit every 100k rows, `synchronous=OFF` | 2,017,642 rows/s |

Most of that is the fsync, which on a spinning disk is one rotation per commit;
an SSD narrows it by around two orders of magnitude but does not close it.
**Batch bulk writes.** A commit every 50k–100k rows is the useful shape.

Do not go to the other extreme and wrap the whole load in one transaction: an
open transaction cannot checkpoint, so the WAL grows to hold the entire dirty
set. Batched at 100k, the WAL stayed at **10 MB across all hundred million
rows**. `CREATE INDEX` — one implicit transaction, not batchable — pushed it to
132 MB on its own.

### `table().id()` costs ~53% on SQLite

`id()` renders `INTEGER PRIMARY KEY AUTOINCREMENT` (`dialect.vt:147`), which
keeps a `sqlite_sequence` row updated on every insert so a rowid is never
reused after a delete. Measured against the same table declared
`integer("id").primaryKey()` — a plain rowid alias — it was **2,148,411/s vs
1,404,196/s**.

If you do not specifically need monotonicity across deletes, spell it
`t.integer("id").primaryKey()`. `id()` remains the right default for a table
where a reused id would be a correctness bug.

### Memory is flat, with one exception

This is the part that held. Across a hundred million inserts, resident memory
was **36.1 MB and identical after every one of the ten tables**. Three full
scans of a ten-million-row table — on the order of 30M `Row` and 150M `Value`
allocations — moved it by **zero bytes**. Reference counting frees each one at
the point the last reference drops; there is no high-water creep to watch.

The exception is `CREATE INDEX`, whose in-memory sorter took RSS from 36 MB to
371 MB on ten million rows. That is SQLite's allocation inside one implicit
transaction, and it is the only phase here whose memory scales with the table.

> **`Db.all()` collects every row; `Db.cursor()` does not.** `Result` holds a
> `Row[]`, so `db.all(select("events"))` on a ten-million-row table materialises
> ten million `Row` objects. Use `all` when you know the result is bounded —
> a `limit`, a primary-key match — and `cursor` whenever it is not. This is the
> one way to make this package use unbounded memory, and it is easy to write by
> accident.

### The statement cache has no eviction

It is capped at 64 by default and **when full, `release` finalizes the
statement instead of storing it** (`sqlite/sqlite.vt:286-292`). So the first 64
distinct SQL strings own the cache for the life of the connection, and the 65th
re-prepares on every single call — silently, with no warning and no counter.

The key is the exact rendered SQL, compared with `==`. `insert("t").set("a",…)
.set("b",…)` and `insert("t").set("b",…).set("a",…)` render differently and are
two entries. An application with many distinct statement shapes should raise
`.stmtCache(n)` past its actual count.

> **`Db.rawCursor` does not release its statement — only the `Cursor` does**,
> on `close()`, on running to completion, or in `deinit`. A cursor abandoned
> mid-scan holds a statement out of the pool until it drops.

### `COUNT(*)` is a full table scan

SQLite keeps no cached row count, so `SELECT COUNT(*)` reads every page: **3.5 s
for one ten-million-row table, 35 s for ten of them** on a cold disk. Nothing
in this package can make that cheaper.

If an estimate will do, `SELECT MAX(id)` is one seek to the edge of the rowid
btree and is constant time. `apps/db_test` renders its table list in 0.17 s that
way, down from 27.7 s counting properly — and says "≈" rather than pretending.

### A slow query can cost you the connection, not just the latency

The caveat above about parking the worker has a sharper edge than it reads.
Behind `vyto/net/server`, the idle sweep — the Slowloris defence, a 15 s
deadline at `server.vt:631` — **cannot distinguish a client that is not sending
from a handler that is not finished.** A request handler that spent ~20 s
scanning and writing before it could send a byte had its connection reaped
mid-response, at 133 MB of a 935 MB reply.

So a long query does not merely make one request slow while blocking the
worker's other connections; past the deadline the requesting connection is
dropped too, and the work is thrown away. Move anything that long off the
request path.

## SQLite is vendored, and why

`native/src/sqlite3/` holds the upstream amalgamation, committed. The tree's
other native dependencies — blend2d, ICU, curl — are fetched or built out of
band, and their tests **skip silently** on a fresh clone. For a database that
trade is wrong: a green run that proved nothing is worse than a red one. SQLite
is a single self-contained C file with no build system of its own, which is
exactly the case vendoring is for. Same argument `vyto/crypto` makes for
micro-ecc.

`native/refresh-sqlite.sh` updates or checks it:

```sh
sh lib/vyto/db/sqlite/native/refresh-sqlite.sh --verify    # tree unmodified?
sh lib/vyto/db/sqlite/native/refresh-sqlite.sh 3.54.0      # update and re-pin
```

Note it verifies **SHA3-256**, which is what sqlite.org publishes — checking
SHA-256 against that field reports a mismatch on a perfectly good download.

Local changes never go inside `sqlite3/`; they go in `db_shim.c` (our entry
points) or `sqlite_config.h` (the build options), both of which survive a
refresh. `--verify` fails if the vendored tree has been touched.

Build options worth knowing, all in `sqlite_config.h`: `SQLITE_THREADSAFE=0`
(no threads in the language, so every mutex is dead weight),
`SQLITE_OMIT_LOAD_EXTENSION` (no `dlopen`, no `-ldl`), and **`SQLITE_DQS=0`** —
double-quoted strings are identifiers, never string literals, so a typo'd column
name is an error instead of quietly comparing against itself.

## PostgreSQL, and what it costs

```vyto
import { Db, select, dbText } from "vyto/db";
import { pgsql } from "vyto/db/pgsql";

let db = new Db(pgsql("postgres://ada@localhost/app"));
if (!db.ok()) { print(db.error()); return; }
```

The driver speaks the v3 wire protocol directly. There is **no `native/`
directory and no `#link`** in the package: it reaches the server through
`vyto/net/socket`, so nothing has to be provisioned, the tests cannot silently
skip on a fresh clone the way the blend2d and ICU ones do, and there is no
GPL/LGPL question about a shipped client library. It also means the socket is
ours, which is the only route to the reactor integration described below.

Authentication: trust, cleartext, **md5**, and **SCRAM-SHA-256** (the default
since PostgreSQL 14). Not GSSAPI/SSPI, and not SCRAM-SHA-256-PLUS, which binds
the exchange to the TLS channel.

### Six things to know before you rely on it

**There is no TLS.** `lib/vyto/crypto/openssl` is a stub in which every entry
point panics, so this connects in the clear. `sslmode=require` is *refused*
rather than quietly downgraded — connecting unencrypted when encryption was
asked for is the one failure a caller must never get without being told. That
rules out most managed providers (RDS, Neon, Supabase) for now. The place TLS
plugs in is `WireIo.requestSsl()`, which is written and returns false.

**`lastInsertId()` is always 0.** Not a gap in this driver: PostgreSQL 12
removed OIDs from ordinary tables, and the `INSERT` tag's oid field is the only
thing the protocol offers. Ask for the key instead —

```vyto
let r = db.exec(insert("users").set("name", dbText("ada")).returning("id"));
let id = r.first().getInt("id");
```

— which is exact, works for non-integer and composite keys, and does not depend
on the insert being the last statement on the connection.

**An error aborts the whole transaction block.** After any failure inside a
transaction the server rejects every further statement with `25P02` until
`ROLLBACK`. SQLite lets you carry on after a constraint violation, so code that
catches a duplicate key and continues inside the same `Tx` works on SQLite and
fails here. `25P02` maps to `DBERR_MISUSE` to make it read as the programming
error it is; `Tx`'s rollback recovers. **Portable code has to assume the
stricter rule.**

**`numeric` and arrays come back as text.** `Value` has five kinds and neither
an exact decimal nor a list is among them. `NUMERIC(19,4)` is used precisely
because a double cannot represent it, and quietly routing money through one
produces the bug that surfaces in a reconciliation months later — so it stays
text. Note `asFloat()` returns `0.0` on a text `Value` by design, so cast in SQL
(`col::float8`) if you want a number. Arrays arrive as the server's literal,
`{1,2,3}`; use `unnest()` in the query, or `array_to_json(col)` with
`vyto/json`.

**One cursor at a time, really.** The protocol cannot interleave two portals on
one connection. Opening a second statement while a `Cursor` is live drains that
cursor's remaining rows into memory so it keeps working — correct, and bounded
by `.drainLimit(n)`, but it has stopped streaming.

**Every query parks the worker, and there is no pool.** No threads, and
`vyto/net/server` is a pre-fork reactor, so a query holds up that worker and
every connection on it; shared-nothing workers mean one connection per worker
for its life. Sixteen workers across four app servers is 64 connections before
any traffic, against a default `max_connections` of 100. The transport
underneath is already non-blocking — `WireIo` drives a non-blocking socket
through a `PollSet` and `PgConn.io()` hands out the fd — but the `Stmt` seam is
inherently blocking: `step()` returns row/done/error with no would-block, and
both `Cursor.next()` and `Db.raw()`'s loop treat anything that is not a row as
terminal. A non-blocking client is an *additive* API on `PgConn`, not a
reinterpretation of this one.

### Why float8 is the one type read as binary

Everything else is requested as text, because the default branch hands back
whatever the server printed and so an unknown OID always degrades gracefully —
which matters, since between the built-ins and every extension type (PostGIS,
`hstore`, `citext`, enums, composites, domains) that set is never closed.

Floats are the exception: reading one as decimal text needs a correctly-rounded
decimal-to-binary conversion, which needs arbitrary-precision arithmetic to do
properly, and getting it merely nearly right loses the last bit. Asking for the
IEEE bits sidesteps the question entirely. Bound float parameters go the same
way when the server has inferred `float8`, and otherwise as the *exact* decimal
expansion — never `str(v)`, which is `%g` and would send `1.23457e+06` for
`1234567.89`.

## Deliberately out of scope

**A migration runner.** Ordered files, a `schema_migrations` table, up and down
— all policy, and policy belongs above the stdlib. `schema.vt` builds the DDL;
running it in some order is yours.

**An ORM or an object mapper.** Vyto has no reflection, so mapping rows to
structs would be code generation, which is a different project.

**`Value` as a generic.** `Value<T>` cannot work: v1 generics have no bounds,
and one instantiated with a type from another module is unsupported when the
parameter crosses by value. The tagged-kind class is the same shape `JsonValue`
and `vyto/cli`'s `Flag` already use, for the same reason.

## Tests

`examples/104_introspect.vt` — 88 assertions, in the golden suite. Its fixture
schema is built entirely out of the cases that are easy to read wrongly: a
rowid-alias primary key with no index behind it, a composite key declared in the
reverse of column order, a stored generated column sitting *between* two
ordinary ones, an implicit foreign-key target, a partial index, a `DESC` index
column, `WITHOUT ROWID` and `STRICT` tables, and a table called `odd-name` that
`quoteIdent` would abort on.

`examples/103_db.vt` — 55 assertions, in the golden suite. It runs entirely
against `:memory:`, so it touches no filesystem and leaves nothing behind. The
load-bearing ones: a value containing `'); DROP TABLE users; --` stored and read
back verbatim with the table intact; a blob containing `0x00` and `0xff`; a
transaction rolled back by `deinit` alone; and a row read *after* the database
is closed.

The dialect and schema sections render Postgres and MySQL SQL while linking only
SQLite — which is why all four dialects live in `vyto/db/dialect` rather than in
the driver packages.

`examples/105_pgwire.vt` — 326 assertions, in the golden suite, and **it needs no
PostgreSQL**. Because the driver is pure Vyto, its codec is a set of pure
functions over `byte[]`, so every message it builds is asserted against a
hand-computed byte string from the protocol specification, and every message it
parses against a hand-built body. The authentication section is two known-answer
tests — RFC 7677 §3 for SCRAM-SHA-256 and RFC 1321 §A.5 for MD5 — which between
them pin PBKDF2, HMAC, SHA-256, MD5, base64 and the exact `AuthMessage`
assembly.

The last third is the part worth copying elsewhere: `MemIo` is a `WireIo` whose
"server" is a recorded transcript in a `byte[]`, so an entire session — the
handshake, the statement cache, prepare, bind, streaming, error recovery, and
`vyto/db`'s own `Db`/`Result`/`Cursor` machinery above it — replays with no
server, no fd and no network. It captures what the *client* said too, which is
the half a live test cannot pin down. Every transcript is also replayed
byte-at-a-time, because a driver that only works when messages arrive whole is
one that will desynchronize against a real socket. Same call `vyto/ui` makes
with `VS_HEADLESS`, and the reason it has 55 tests.

Both goldens were fault-injected — a wrong length prefix, a dropped
`AuthMessage` separator, `numeric` quietly becoming a float, a float formatted
with `%g`, a stale binding resent, an error path that skips resynchronization,
`indoption` read as a 1-based array — to confirm each one fails when it should.
A test of this shape passes trivially once it stops finding anything, so it is
worth redoing if it is ever refactored.

**Against a real server**, `tests/run_tests_pgsql.sh` runs
`tests/fixtures/pgsql_live.vt` and `tests/fixtures/pgsql_introspect_live.vt`. It
is not in `make test` and skips cleanly when `VYTO_PG_DSN` is unset, so CI never
stands up a database:

```sh
docker run -d -p 5432:5432 -e POSTGRES_PASSWORD=pw postgres:16
VYTO_PG_DSN='postgres://postgres:pw@localhost/postgres' sh tests/run_tests_pgsql.sh
```

It covers what a recorded transcript structurally cannot: a real handshake
against a real `pg_hba.conf` (set `VYTO_PG_DSN_TRUST` / `_MD5` / `_SCRAM` to
exercise all three), values round-tripping through actual storage, the aborted
transaction block, the cached-plan retry after a mid-session `ALTER TABLE`, a
200k-row cursor's memory profile, and — the one thing nothing offline can settle
— whether `indoption` and `indcollation` really are 0-subscripted, which decides
whether `DESC` index columns are reported against the right column.

`apps/db_test` is the load half, and not in the suite: it loads a hundred
million rows, times every phase and samples resident memory beside each one,
then serves the result over HTTP. Every figure in *What things cost* above came
from it. It is a multi-gigabyte data directory and minutes of wall clock, so it
is run deliberately rather than by `make test`:

```sh
./vytoc run apps/db_test/db_test.vt -- --rows 10000 --tables 2   # smoke, seconds
./vytoc build apps/db_test/db_test.vt --release -o /tmp/db_test
/tmp/db_test --rows 10000000                                     # the full 100M
```
