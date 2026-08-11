# db_test — vyto/db with a hundred million rows in it

`examples/103_db.vt` proves `vyto/db` is *correct*. It runs against `:memory:`
with a handful of rows and says nothing about what the package costs. This app
is the other half: ten tables, ten million rows each, loaded and read back and
served over HTTP, with every phase timed and resident memory sampled beside it.

Two programs over one store:

```sh
./vytoc run apps/db_test/db_test.vt              # load and measure
./vytoc run apps/db_test/db_server.vt -- 8080    # then serve it
```

| | |
|---|---|
| `db_test.vt` | the harness — schema, bulk load, index build, three scans, lookups, durable writes |
| `db_server.vt` | the server — paginated browse, a full-table dump, an insert form |
| `tables.vt` | the schema both must agree on, plus generators and the `/proc` readers |
| `view.vt` | every byte of HTML |
| `data/` | `store.db`, its WAL, and the generated dumps. Gitignored |

## Running it

**Build `--release` before quoting any number.** A debug build measures the
compiler's bounds checks, not the database.

```sh
./vytoc build apps/db_test/db_test.vt --release -o /tmp/db_test
/tmp/db_test --rows 10000000
```

| flag | default | |
|---|---|---|
| `--rows N` | 1000000 | rows per table |
| `--tables N` | 10 | how many tables |
| `--resume` | off | skip tables already at the target count |
| `--skip-load` | off | measure reads against what is already there |
| `--durable-rows N` | 200 | rows for the `synchronous=FULL` phase |
| `--lookups N` | 10000 | point lookups per variant |
| `--no-server-dump` | off | skip phase 7, which pre-writes the HTML the server sends |

The default is 1M × 10 — about 450 MB and a couple of minutes. `--rows
10000000` is the full 100M: roughly 4.5 GB, and on a spinning disk a long
while. Smoke-test at `--rows 10000 --tables 2` first; nobody should debug a
half-hour run by re-running it.

The harness prints, before it starts, whether the dataset will fit in available
RAM. A read throughput figure from a warm page cache and one from a cold disk
differ by two orders of magnitude, and a number quoted without which one it was
is not a measurement.

## What each phase is actually asking

**Phase 2 — bulk write.** One `prepare` for the whole table, four `Value`
objects reused by mutating their public fields, `bind`/`step`/`reset` per row,
a commit every 100k. This is the floor: the same insert with every one of the
library's conveniences removed. Batched commits are not a tuning choice — a
single ten-million-row transaction cannot checkpoint, so the WAL would grow to
the size of the entire dirty set before the first fsync.

**Phase 3 — what `t.id()` costs.** `schema.vt`'s `id()` is the obvious spelling
and every app will reach for it. On SQLite it renders `INTEGER PRIMARY KEY
AUTOINCREMENT` (`dialect.vt:147`), which keeps a `sqlite_sequence` row updated
on every insert so a rowid is never reused after a delete. Two scratch tables,
identical but for that column, same rows, same loop. **It is not free**, and
the number is large enough to be worth knowing before you type `id()`.

**Phase 5 — three scans of one column.** Identical query, three ways:

| | |
|---|---|
| raw `Stmt.step()` + `columnValue(0)` | the floor |
| `Cursor` + `row().at(0)` | adds a `Row` and a `Value` per step |
| `Cursor` + `row().getInt("uid")` | adds a linear scan of the column names |

Subtracting them apart prices `Cursor` and `Row` in the package's own terms.
All three sum the same column and the harness aborts the phase if the sums
disagree — three timings of three different answers would be worthless.

**Phase 6 — durable write.** One row per transaction at `synchronous=FULL`:
what a form POST costs when it must be on disk before the response goes out. On
a 5400 RPM disk this is bounded by rotational latency and lands orders of
magnitude below phase 2. Both numbers print together, because a bulk-load
figure quoted without its pragma is not a measurement of anything.

**Phase 7 — pre-generate the dump.** Writes the HTML that `GET /t/:name/all`
serves, and reports RSS on both sides of an 892 MB write. Not decoration — see
the second mistake below.

## The server, and the one constraint that shapes it

`vyto/net/server` has **no chunked or streaming response**. `write_response`
(`server.vt:1631`) composes the status line, the headers, an auto-computed
`Content-Length` and the whole of `res.body_str` after the handler returns; the
handler never sees the socket. Ten million rows of HTML is ~1.5 GB, and a
`StringBuilder` doubling its way there peaks at twice that.

So `/t/:name/all` does not build a response. It scans the table straight into a
file through a `BufWriter` — 1 MB flush threshold, so resident memory is
bounded by the buffer rather than by the row count — and hands the path to
**`res.sendFile()`**, which `lseek`s for the length and lets `sendfile(2)` move
the bytes. They never enter the process.

> The dump is real HTML and the server stays flat producing it, but **no
> browser will lay out a 10M-row `<table>`** — the tab will hang or be killed.
> `curl -o dump.html` it, or use `?limit=N`. That is a property of browsers,
> not of the server, and it is why the paginated route exists beside it.

| route | |
|---|---|
| `GET /` | tables, counts, database size, RSS, and the last load's numbers |
| `GET /t/:name` | keyset pagination — `?after=<id>&n=<100>` |
| `GET /t/:name/all` | the full dump, `?limit=N`, `?force=1`. Cached against the store's mtime |
| `GET /new` / `POST /new` | insert a row |
| `GET /stats` | the same numbers as JSON |

**Pagination is keyset, never `OFFSET`.** `LIMIT 100 OFFSET 9999900` walks ten
million rows to discard all but the last hundred; `WHERE id > ? ORDER BY id
LIMIT 100` is one seek. Page one and page one hundred thousand cost the same,
which is the point.

**`POST /new` uses `db.exec(insert(...))` deliberately** — the ergonomic path,
re-rendering the SQL and re-binding through the statement cache, which is what
an application actually writes. The loader deliberately does not, so the two
numbers bracket the real cost.

`runSingle()`, not `run()`: a `sqlite3` handle must not cross a `fork()`, and
pre-fork workers share nothing, so per-worker row counts and RSS would be
incoherent.

## Measured

100,000,000 rows — ten tables of ten million — on a 5400 RPM `WDC WD10SPZX`
with 7.6 GB RAM (2.5 GB available), `--release`. Whole run: **3 m 55 s**.

| | |
|---|---|
| **bulk insert** | **100,000,000 rows in 49.6 s — 2,017,642/s, 0.50 µs/row** |
| bytes written | 3.43 GB at 70.9 MB/s |
| **RSS across the entire load** | **36.1 MB, identical after every one of the ten tables** |
| WAL during the load | held at 10 MB — the batched commits doing their job |
| `t.id()` AUTOINCREMENT tax | **53%** — 2,148,411/s → 1,404,196/s |
| index build, 10M rows | 16.8 s. RSS 36 MB → 371 MB — SQLite's sorter, the one phase whose memory scales |
| scan, raw `Stmt` | 0.70 s — 14,290,568 rows/s |
| scan, `Cursor` + `at(0)` | 1.36 s — 7,352,949/s (**+96%**) |
| scan, `Cursor` + `getInt(name)` | 1.37 s — 7,302,539/s (**+101%**) |
| → `Row`+`Value` allocation | 726 ms over 10M rows = **73 ns/row** |
| → column name lookup | 37 ms over 10M rows = **3.7 ns/row** |
| **RSS across all three scans** | **371.3 MB → 371.3 MB. Zero.** |
| `COUNT(*)` × 10 tables | 100M rows in 35.5 s — it is a full scan, see below |
| lookup by `id`, random, cold | 8.8 ms — a seek into a 3.7 GB file |
| lookup by `uid`, covering index | **5.1 µs** |
| lookup by `uid`, no index | 376 ms per probe |
| durable insert, `synchronous=FULL` | 7 rows/s, 141 ms/row |
| dump 10M rows to HTML | 891.8 MB in 22.4 s, **RSS 371.3 → 371.3 MB** |
| serve that 935 MB over HTTP | 81.6 MB/s, **server RSS 3.96 MB → 3.96 MB** |
| paginated page at row 9,999,000 | 1.8 ms |

### What those say

**The write path does not accumulate.** 36.1 MB, unchanged after each of ten
tables. Reference counting frees a `Row` and its cells when the last reference
drops, and at a hundred million rows that is visible as a flat line.

**Neither does the read path.** Three full scans of ten million rows — roughly
30M `Row` allocations and 150M `Value` allocations — moved resident memory by
**zero**. The only phase that grows is `CREATE INDEX`, and that memory is
SQLite's in-memory sorter, not ours.

**`Cursor` costs about 2×, and it is the allocation, not the name lookup.** 73
ns/row for the `Row`+`Value` pair against 3.7 ns/row for the linear scan over
column names. Anyone tempted to optimise `Row.get` by hashing the names would
be attacking the 5% and leaving the 95%. Use the raw `Stmt` when a scan is the
hot path; the ergonomic path is fine everywhere else, and 7M rows/s is not slow.

**`t.id()` is not free.** 53% on this workload, for a rowid-uniqueness
guarantee across deletes that most applications never rely on. Worth knowing
before you type it.

**A covering index beats the primary key.** 5.1 µs against 8.8 ms — three
orders of magnitude — because `SELECT id … WHERE uid = ?` is answered entirely
from the index and never touches the table, while a random rowid seek must go
fetch the row from a cold 3.7 GB file. Both probe the same randomly chosen
rows; an earlier version of this benchmark did not, and reported a number that
was really about page-cache locality.

## Two mistakes this app made first, so you don't have to

**`COUNT(*)` is a full scan.** SQLite keeps no cached row count. The index page
called it for all ten tables and took **27.7 seconds** to render a grid of
cards. Switched to `MAX(id)` — one seek to the edge of the rowid btree,
constant time — it renders in **0.17 s**, a 163× difference. The page says "≈"
and offers an exact count on request. Any page that displays "N rows" for a
large table has this bug until it doesn't.

**A slow handler looks exactly like a slow client.** The first
`GET /t/t00/all` died after delivering 133 MB of 935 MB. Not a `sendfile` bug:
the handler spent ~20 s scanning and writing the dump before it could send a
byte, and the idle sweep — the Slowloris defence, a 15 s deadline set at
`server.vt:631` — reaped the connection. It cannot distinguish a peer that
isn't sending from a server that isn't finished. So the loader pre-generates
the dump (phase 7) and the request path only ever sends a file that already
exists; the same request then delivered all 935 MB at 81.6 MB/s.

This is the `vyto/db` README's "a synchronous query parks the whole worker"
warning with a sharper edge on it: it is not only the *other* connections that
suffer — the requesting connection can be dropped for the server's own
slowness.

## Things that were easy to get wrong

**The schema uses `integer("id").primaryKey()`, not `t.id()`** — a plain rowid
alias, for the reason phase 3 measures.

**`label` comes from a fixed set of 64 strings**, indexed by `i & 63`. Building
a fresh string per row would put a string allocation inside the measured loop
and report it as database time. SQLite stores a full copy per row either way,
so the bytes on disk are unaffected.

**`db_bytes()` counts the WAL**, which mid-load holds real data not yet
checkpointed back. Reporting `store.db` alone understates it by gigabytes.

**Every `:name` is validated against the known table list** before it reaches
`quoteIdent`, which *panics* on anything that is not a plain identifier. Without
that check a stray URL would abort the process.

**Every value is bound, never interpolated.** A label containing
`'); DROP TABLE t00; --` stores and reads back verbatim with the table intact,
and renders escaped in both the paginated page and the sendfile'd dump.

## Not in `make test`

The suite is `examples/NN_*.vt` compared against golden stdout. This app's
output is timings over a multi-gigabyte data directory — neither reproducible
nor cheap. `make test` is untouched.
