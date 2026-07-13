# Vyto Benchmark Issues Report

Generated from benchmarks run on 2026-07-06.
Tests: Fibonacci, Primes, Matrix Multiply, Ackermann, Array Ops, Log Parsing (1M lines), 15-Queens, SQLite (1M rows write + read).

**Update 2026-07-07: issues 1–8 and 11 (partially) are fixed in v0.2.**
Each issue below carries a status; resolved ones note the fix. New numbers
in the Performance Summary.

---

## 1. Missing Bitwise Operators — **FIXED (v0.2)**

**Severity: High**

Vyto now has `& | ^ ~ << >>` and `&= |= ^= <<= >>=`, with Go-style
precedence (`& << >>` bind like `*`; `| ^` like `+`), so `a & mask == 0`
parses sanely. Shifts keep the left operand's type.

Result: the bitmask N-Queens (`07_nqueens_bitmask.vt`) runs at **1.02x C**
(1.24s vs 1.21s, n=15) — the array-based workaround needed 87.8s.

```vyto
let avail = (~(cols | d1 | d2)) & mask;   // now valid Vyto
```

---

## 2. No Built-in File I/O — **FIXED (v0.2)**

**Severity: High**

New builtins: `readfile(path): string`, `readlines(path): string[]`,
`writefile(path, data): bool`, `appendfile(path, data): bool`. No FFI, no
buffers, no `str()` copies per line. `06_logparse.vt` dropped from 0.509s
to 0.25s (3.49x → 1.9x C). Note: `readlines` holds the whole file in
memory; use FFI streaming when peak RSS matters.

---

## 3. No Address-Of Operator (`&`) — **FIXED (v0.2)**

**Severity: High**

Unary `&` on a value-type local/parameter yields `rawptr`:

```vyto
let db: rawptr = null;
sqlite3_open("logs.db".cstr(), &db);   // ppDb out-param, no C wrapper
```

The `volt_sqlite3_*` wrapper C file is gone; `sqlite3.vt` now binds
`sqlite3_open`/`sqlite3_prepare_v2` directly. Taking the address of a
ref-counted value is a compile error.

---

## 4. Cannot Cast `byte[]` to `cstring` — **FIXED (v0.2)**

**Severity: Medium**

`byte[]`/`i8[]` casts to `cstring`/`rawptr` as a borrowed view of the
buffer, and the new `bytes(n)` builtin allocates a zeroed n-byte array:

```vyto
let buf = bytes(4096);
fgets(buf as cstring, 4096, f);   // no malloc/free via FFI
```

---

## 5. Extern Function Type Conflicts with Runtime Headers — **FIXED (v0.2)**

**Severity: Medium**

Extern fns are now emitted under private identifiers aliased to the real
symbol via `__asm__` (`extern void* vx_strstr(...) __asm__("strstr")`), so
declarations can never collide with system headers pulled in by
`vyto_rt.h`. Declaring `strstr(hay: cstring, needle: cstring): rawptr`
just works.

---

## 6. `str(cstring)` Allocates on Every Call — **MITIGATED (v0.2)**

**Severity: Medium**

The dominant case (reading files line by line) no longer goes through
`str(cstring)` at all — `readlines()` builds the strings once in C.
A true zero-copy span type over foreign buffers is still future work.

---

## 7. `const` Cannot Be Initialized with Non-Literal Expressions — **PARTIALLY FIXED (v0.2)**

**Severity: Medium**

`null` is now a valid const initializer (`const SQLITE_STATIC: rawptr =
null;`). General constant expressions (arithmetic, references to other
consts) remain future work.

---

## 8. `int` and `i32` Are Not Interchangeable — **FIXED (v0.2)**

**Severity: Medium**

Value-preserving integer widening is implicit: signed → wider signed,
unsigned → wider unsigned, unsigned → strictly wider signed. Applies to
assignment, arguments, returns, and mixed-width arithmetic/comparisons
(result is the wider type):

```vyto
if (sqlite3_exec(db, sql, null, null, null) != SQLITE_OK) { ... }  // no cast
```

Narrowing and same-width sign changes still need `as`.

---

## 9. Bounds-Checked Array Indexing — open (by design)

**Severity: Low**

Every `arr[i]` / `text[i]` is bounds-checked; costs 2-3x vs C in tight
index loops. Deliberate safety trade-off; an unchecked escape hatch or
iterator-based optimization is future work.

---

## 10. Reference Counting Overhead on Dynamic Arrays — open (by design)

**Severity: Low**

Arrays are ref-counted; hot recursive paths that pass arrays touch the
counter. The bitmask N-Queens rewrite (issue 1) shows the practical fix:
integer state beats array state where it matters.

---

## 11. No Zero-Copy Buffer Access — **MITIGATED (v0.2)**

**Severity: Medium**

`bytes(n)` + `buf as cstring` gives C a Vyto-owned buffer with no copies,
and `readlines()` removed the per-line copy tax. Reading foreign C buffers
without copying (a span/slice type) is still future work.

---

## 12. `let` Variable Reassignment Confusion — open

**Severity: Low**

`let` still allows reassignment. Needs a language decision (`var` vs
enforced immutability); deferred.

---

## Performance Summary (re-run 2026-07-07, v0.2 compiler)

### Compute Benchmarks

| Benchmark | Vyto (release) | C (-O2) | Ratio | v0.1 ratio |
|-----------|---------------|---------|-------|------------|
| Fibonacci(40) | 0.21s | 0.245s | ~1.0x | 1.03x |
| Primes (10M) | 0.19s | 0.103s | ~1.9x | 2.50x |
| Matrix 300x300 | 0.10s | 0.041s | ~2.4x | 3.24x |
| Ackermann(3,10) | 0.03s | 0.049s | ~1.0x | 1.35x |
| Array Ops (10M) | 0.11s | 0.062s | ~1.8x | 1.97x |
| Log Parse (1M lines) | 0.25s | 0.13s | 1.9x | 3.49x |
| **15-Queens (bitmask)** | **1.24s** | **1.21s** | **1.02x** | 2.25x (array), 350x vs bitmask C |

### Real-World: SQLite (1M rows write + read), no C wrappers

| Language | Time | Peak Memory | vs C |
|----------|------|-------------|------|
| **C (-O2)** | 2.68s | 4,480 KB | 1.0x |
| **Vyto (release)** | 2.81s | 4,480 KB | 1.05x |
| **Python (streaming)** | 9.78s (v0.1 run) | 12,672 KB | ~3.6x |

The two headline gaps from v0.1 — bitwise algorithms (350x) and FFI-heavy
file handling — are closed. Remaining overhead is bounds checking and RC,
both deliberate safety trade-offs (issues 9/10).
