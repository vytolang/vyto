# vyto/regex — Perl-compatible regular expressions

Pattern matching backed by [PCRE2](https://github.com/PCRE2Project/pcre2),
compiled from source that is vendored in this repository. Nothing to install,
nothing to provision: clone, `make`, and it works on every target.

Patterns and subjects are UTF-8 by default — `\w`, `\b` and `\p{...}` follow
Unicode rules, not ASCII. Matching is JIT-compiled unless the platform refuses
executable memory, in which case it silently falls back to PCRE2's interpreter.
A hostile pattern fails with an error in bounded time instead of hanging.

```vyto
import { Regex, rx_find, RX_CASELESS } from "vyto/regex";

let re = new Regex("(?<user>\\w+)@(?<host>[\\w.]+)", 0);
re.isValid();                        // true — check this, compiling is soft
let m = re.find("mail bob@example.com now");
m.text();                            // "bob@example.com"
m.named("user");                     // "bob"
m.start();                           // 5
m.groups();                          // ["bob@example.com", "bob", "example.com"]
re.findAll("a@b c@d").len;           // 2
re.replaceAll("a@b", "${host}!");    // "b!"        — $1, ${name}, $$ — never \1
re.split("x a@b y").len;             // 2

new Regex("abc", RX_CASELESS).test("ABC");   // true
rx_find("\\d+", "x42").text();               // "42" — cached, no Regex to hold

let bad = new Regex("(unclosed", 0);
bad.isValid();                       // false
bad.errorOffset();                   // 9
```

## Modules

| Module | What it gives you |
|---|---|
| `regex.vt` ⚙ | Everything: `Regex`, `Match`, the `RX_*` constants, and the cached `rx_*` free functions |

⚙ marks a module backed by a native shim. `vyto/regex` is deliberately a single
module — see [Layout](#layout).

## The convenience layer

The core API (`find`, `findAll`, `replace*`, `split*`) is what everything else
is built from. These are the shortcuts for the jobs people actually do.

**Every `rx_*` function takes a trailing `flags` that defaults to `0`**, so the
cached path honours `RX_CASELESS`, `RX_MULTILINE`, `RX_BYTES` and the rest. The
flags are part of the cache key, so one pattern under two flag sets occupies two
slots rather than colliding.

### Getting text out

| Call | Gives you |
|---|---|
| `re.findAllText(s)` / `rx_find_all(pat, s)` | every match's text — no `Match` built |
| `re.findAllGroup(s, i)` / `rx_find_all_group(pat, s, i)` | one capture across every match |
| `re.count(s)` / `rx_count(pat, s)` | how many, counted in C with zero allocation |
| `re.findOr(s, dflt)` / `rx_find_or(pat, s, dflt)` | first match, or a default |
| `rx_group(pat, s, i)` / `rx_named(pat, s, name)` | one capture from the first match |
| `rx_grep(pat, s)` / `rx_grep_v(pat, s)` | the lines that do / don't match |

```vyto
rx_find_all("\\d+", "a1 b22 c333");        // ["1", "22", "333"]
rx_find_all_group("(\\w)=(\\d)", cfg, 2);  // every value
rx_count("ERROR", log);                     // no array allocated
rx_grep("^ERROR", log, RX_MULTILINE);       // matching lines
rx_find_or("v(\\d+)", s, "0");
```

### Whole-string matching

`fullMatch` is not the same as checking `find`'s offsets. Matching is
leftmost-first, so `a|ab` against `"ab"` finds `"a"`, and an offset test would
wrongly report no full match. It compiles `\A(?:pattern)\z` once, on demand.

`\A` and `\z` rather than `^` and `$` is deliberate: they are absolute, so the
answer does not shift under `RX_MULTILINE` and does not fall for `$`'s "also
matches before a final newline" rule — which is the exact bug this exists to
prevent.

```vyto
rx_test("^abc$", "abc\n");          // true  — the classic surprise
rx_full_match("abc", "abc\n");      // false — what you meant
```

`(?:...)` is non-capturing, so group numbering is untouched.

### Escaping user input

**`rx_quote` is not optional when any part of a pattern came from outside the
program.** Without it, someone typing `a.*b` into a search box gets a wildcard,
and someone typing `(a+)+$` gets a denial of service.

```vyto
rx_count(rx_quote(userInput), haystack);
```

It escapes ``\ . ^ $ | ? * + ( ) [ ] { }`` and encodes NUL as `\x00`.
Over-escaping is harmless; under-escaping is a bug, so the set is deliberately
wide.

### Editing

| Call | Does |
|---|---|
| `re.replaceFn(s, f)` | replace each match with what `f` returns for it |
| `re.replaceN(s, repl, n)` | replace at most the first `n` |
| `rx_replace_first(pat, s, repl)` | first only — twin of `text.replace_first` |
| `re.expand(m, template)` | apply `$1`/`${name}`/`$$` to a `Match` you already have |
| `re.each(s, f)` | call `f` per match without building the array |

```vyto
let redact: fn(Match): string = (m) => m.named("user") + "@***";
mail.replaceFn(log, redact);
```

Two v0.1 language limits bite here, and they are the language's, not this
module's: **the arrow must be assigned to a typed target** (an inline one at the
call site cannot be inferred), and **it cannot capture `this`**, so it cannot be
written inside a method that needs instance state. Accumulating across calls
needs the 1-element-array idiom, since captures cannot be assigned to.

### Slicing up a subject

| Call | Gives |
|---|---|
| `re.partition(s)` | `[before, match, after]` — always 3 elements, always rejoins to `s` |
| `re.splitKeep(s)` | split keeping the delimiters; also rejoins exactly |
| `re.trim(s)` / `rx_trim(pat, s)` | strip one leading and one trailing match |
| `rx_strip_prefix` / `rx_strip_suffix` | drop a leading / trailing match if present |
| `rx_index_of` / `rx_last_index_of` | byte offset of the first / last match, or `-1` |

### Validators

Only two, and the omissions are the point. UUID, URL, ISO-date and numbers all
already have hand-written validators that beat a regex on accuracy *and* on
error reporting — use `uuid_is_valid` (`vyto/util/uuid`), `url_is_valid`
(`vyto/util/url`), `date_parse` + `date_is_valid` (`vyto/util/date`), and
`parse_int` / `parse_float` (`vyto/cli`).

| Call | |
|---|---|
| `rx_is_email_loose(s)` / `RX_P_EMAIL_LOOSE` | a shape check, see below |
| `rx_is_ipv4(s)` / `RX_P_IPV4` | dotted quad, range-checked |
| `rx_test_any(pats, s)` | true if any pattern matches |

**`rx_is_email_loose` — the `_LOOSE` is not decoration.**

It accepts `a@b.co`, `first.last@sub.example.co.uk`, `user+tag@x.io`,
internationalised domains, and a local part containing almost anything —
consecutive dots, a leading dot, `!#$%`. It rejects a missing `@`, whitespace
anywhere, a domain with no dot, a trailing dot, consecutive dots in the domain,
and an empty local part or domain label.

It deliberately does **not** accept three things that are legal RFC 5322 and
vanishingly rare: quoted local parts with spaces (`"a b"@x.com`), IP-literal
domains (`a@[10.0.0.1]`), and comments.

**What it is not:** an RFC 5322 validator, a deliverability check, or evidence
that mail sent there arrives. The only way to know an address works is to send
to it and have someone click the link. Use this to catch a typo in a form field,
not to make a decision.

It is ReDoS-safe by construction: dot is excluded from the domain character
classes, so no two parts of the pattern can match the same byte and there is
nothing for the engine to backtrack through. The classic `([a-z0-9.]+)+@`
catastrophe cannot occur. The fixture asserts a 5000-character non-matching
subject returns rather than burning the match limit.

`RX_P_IPV4` range-checks each octet, so `999.1.1.1` is rejected where the usual
`\d{1,3}` version accepts it, and it rejects leading zeros — `01.2.3.4` is read
as octal by some resolvers and is a spoofing vector.

Both constants are exported as plain strings, so they compose:
`rx_test("<" + RX_P_EMAIL_LOOSE + ">", "<a@b.co>")`.

## Errors: where the line falls

The library-wide rule is **panic hard, sentinel soft**
(`lib/vyto/util/README.md`). Applied to regex it lands in a place worth stating
outright:

- **Compiling a bad pattern is soft.** `new Regex(...)` never panics. A regex is
  usually *data* — a config value, a `--filter` argument, the contents of a
  search box — so it behaves like `url_parse` and `toml_parse`: check
  `isValid()`, read `error()` and `errorOffset()`.
- **Matching with a failed `Regex` panics.** Ignoring the sentinel is the
  programmer error. Silently answering "no match" would be a wrong answer rather
  than a loud one.
- **A limit hit is soft where there is somewhere to put it.** `find` returns a
  `Match` with `errorCode() == RX_ERR_LIMIT`. `test`, `findAll`, `replace*` and
  `split*` return plain values with no error channel, so they **panic** — a
  truncated result would be indistinguishable from a real one.
- **Invalid UTF-8 in the subject is soft**: `find` returns `RX_ERR_UTF`. It is
  data from elsewhere, not a bug in your program. `RX_BYTES` turns Unicode off
  if you are matching binary.

`errorCode()` is one of `RX_OK`, `RX_NO_MATCH`, `RX_ERR_LIMIT`, `RX_ERR_UTF`,
`RX_ERR_INTERNAL`. PCRE2's own numbering never reaches Vyto, so a PCRE2 upgrade
cannot change a value your code branches on.

## Limits and ReDoS

Every `Regex` is created with a backtracking budget:

| | default | what it bounds |
|---|---|---|
| `RX_DEFAULT_MATCH_LIMIT` | 1 000 000 | total backtracks |
| `RX_DEFAULT_DEPTH_LIMIT` | 10 000 | recursion depth |
| `RX_DEFAULT_HEAP_KB` | 16 384 | heap the matcher may claim |

These are far below PCRE2's own compile-time ceilings, which are "don't hang
forever" backstops rather than safety limits. `setLimits(match, depth, heapKB)`
raises or lowers them per pattern; pass `0` to leave one alone.

```vyto
let evil = new Regex("(a+)+$", 0);
let m = evil.find("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa!");
m.matched();      // false
m.errorCode();    // RX_ERR_LIMIT — milliseconds, not until the heat death
```

One caveat worth knowing: **`depth_limit` has no effect while the JIT is
active**, because the JIT uses its own stack rather than the interpreter's
recursion. `match_limit` binds either way, which is why the guarantee above
holds with JIT on and off. If you want the depth limit to bite, run with
`VYTO_REGEX_JIT=0`.

## Performance notes

- **JIT is on by default.** `pcre2_jit_compile` runs at `Regex` construction and
  attaches a process-global JIT stack (32 KB growing to 512 KB). If it fails —
  a hardened macOS runtime without `allow-jit`, a W^X kiosk, an architecture
  sljit has no backend for — nothing breaks: `pcre2_match` falls through to the
  interpreter. `jitEnabled()` reports which one you got, and
  `rx_jit_available()` reports whether this build has JIT at all.
- **`VYTO_REGEX_JIT=0`** forces the interpreter at runtime. There is no
  build-time switch, because Vyto has no `#cflags` pragma to hang one on.
- **Compile once, match many.** Compiling is the expensive half. Hoist the
  `Regex` out of your loop, or use the `rx_*` functions, which compile through a
  64-entry process-local cache keyed on (pattern, flags).
- **UTF validation is cached per scan.** With Unicode on, PCRE2 revalidates the
  whole subject on every call, which would make `findAll` over a large log
  quadratic. The shim validates once and passes `PCRE2_NO_UTF_CHECK` for the
  rest of the scan. This is the single biggest performance trap in the module
  and it is already handled.
- **Match data is allocated per `Regex`, not per call**, so `findAll` over
  10 000 matches allocates nothing in C. The cost is that a `Regex` is **not
  reentrant** — do not interleave two scans over the same object. Vyto has no
  threads and `vyto/os/worker` is fork-based, so this is only ever a
  single-flow concern.

## Why PCRE2 is vendored

This repository's standing rule is *fetch, don't vendor*, and it is written down
in at least five places — `.gitignore:25-27` (which rejects exactly this for the
SQLite amalgamation), `.gitignore:33-40`,
`lib/vyto/gfx/native/build-blend2d.sh:3-5`,
`lib/vyto/net/native/provision-curl.sh`, and `benchmarks/README.md:30-33`. Until
this module there was no third-party C in the tree at all.

`vyto/regex` is a deliberate exception, for two reasons:

1. **A fetched dependency makes tests silently skip.** `tests/run_tests.sh:505`
   and `:518` skip the whole gfx block when blend2d has not been built. That is
   tolerable for a canvas backend. Regex is not optional — it is the substrate
   for text search, log filtering, validation and bulk rename — and a
   "must-have" module that quietly is not there on a fresh clone is worse than
   the disk cost.
2. **PCRE2 is cheap to vendor.** It is pure C with no build system of its own:
   no cmake, no autoconf, no C++ toolchain, no two-pass cross build. The other
   three dependencies are none of those things.

The cost, measured rather than guessed: the git pack went from **816 KiB to
3.72 MiB**. About 4 MB on disk across 78 files. The bulk is unavoidable for what
was asked of it — `pcre2_ucd.c` (350 K of Unicode tables), `pcre2_jit_compile.c`
(423 K), `pcre2_compile.c` (361 K), and sljit's x86 and ARM backends. Dropping
sljit's unused architectures (MIPS, PPC, S390X, RISC-V, LOONGARCH) would recover
only ~700 K raw, roughly 5% of the pack, at the price of breaking any future
target — so they stay.

## Layout

```
lib/vyto/regex/
  regex.vt                  the entire public API
  native/
    pcre2.version           the pinned release
    pcre2.sha256            sha256 of that release's tarball
    pcre2.files.sha256      sha256 of every vendored file
    refresh-pcre2.sh        re-vendoring tool — never run by the build
    src/
      config.h              first-party: autoconf's job, done by hand
      pcre2.h               first-party: generated from upstream pcre2.h.in
      regex_shim.c          first-party: the C half of this module
      vregex_*.c            29 generated wrapper TUs
      pcre2/                PRISTINE upstream, byte-identical to the release
```

The wrapper TUs look pointless and are not. Three compiler facts force them:

- `vytoc` compiles `native/src/*.c` **flat and non-recursively**
  (`src/main.c:616-635`), so a subdirectory is invisible to it — which is
  exactly how the vendored tree avoids being compiled twice or wrongly.
- The only include path is `-Inative/src` (same place), which still *reaches
  into* that subdirectory, so `#include "pcre2/src/pcre2_compile.c"` resolves.
- **There is no `#cflags` pragma.** `src/parse.c:860-878`: `#link` is the only
  one, and anything else after `#` is a hard error. PCRE2 needs
  `-DPCRE2_CODE_UNIT_WIDTH=8` and `-DHAVE_CONFIG_H`, so they are `#define`d in
  each wrapper instead.

**Do not merge the wrappers into one unity build.** Upstream has many same-named
file statics; collapsing the translation units makes them collide. One wrapper
per upstream source preserves the boundaries that keep them apart.

`config.h` and `pcre2.h` sit *outside* `native/src/pcre2/` on purpose, so that
directory stays byte-identical to the release and `refresh-pcre2.sh --verify`
can prove it. `pcre2_internal.h` includes both by unqualified name, misses
`pcre2/src/`, and falls through to `-Inative/src`.

`native/` has no `<triple>/` directory, which is what tells `vytoc` there are no
prebuilt libraries to find (`src/main.c:637-670`) and why this package needs no
`#link` and no provisioning script.

## Refreshing PCRE2

```sh
sh lib/vyto/regex/native/refresh-pcre2.sh --verify   # tree matches the release?
sh lib/vyto/regex/native/refresh-pcre2.sh --dry-run 10.46
sh lib/vyto/regex/native/refresh-pcre2.sh 10.46
```

The script derives the compiled-source list mechanically from upstream's
`Makefile.am` rather than trusting a hand-written list, and fails loudly if it
disagrees with the wrappers on disk. That check is not decoration: upstream
added `pcre2_chkdint.c` in 10.43 and `pcre2_compile_class.c` in 10.45, and a new
source with no wrapper is a link error while a stale wrapper is a duplicate
symbol.

Before committing a version bump:

1. Build the **untrimmed** tarball and run PCRE2's own suite —
   `./configure --enable-jit --enable-unicode && make && make check`. The
   vendored tree drops `testdata/`, so this is the only place upstream's
   conformance tests ever run. (10.45 passes 4/4, including `pcre2_jit_test`.)
2. Confirm the script's TU-drift report is empty.
3. `make clean-cache && ./tests/run_tests.sh`.

## Build cost

Vendoring means every program importing `vyto/regex` compiles 30 native
translation units. Measured here:

| | time |
|---|---|
| cold debug build (`vytoc run`, `-O0`) | ~4 s |
| warm rebuild | ~0.07 s |
| cold release build (`--release`, `-O2`) | ~20 s |

Two consequences worth knowing before you file them as bugs:

- `vytoc` has no header dependency scanner (`src/main.c:576-592`): touching
  *any* file under `native/src` rebuilds all 30 objects.
- Objects are cached per entry-file directory, so `examples/`,
  `tests/fixtures/` and each `apps/*` keep their own copy, once per
  profile and target. `make clean-cache` is the escape hatch.

A release binary using `vyto/regex` is around 620 KB on linux-x64 and 1.6 MB on
windows-x64.

## Portability

- **linux-x64, linux-arm64, macos-x64, macos-arm64** — full support, JIT active.
- **windows-x64** — full support, cross-built with mingw and part of
  `make test-win`. Unlike `vyto/gfx`, `vyto/intl` and `vyto/net`, this module
  needs nothing installed on the target, which is a direct consequence of
  vendoring.
- **TCC** — `vytoc run` picks `tcc` over `cc` for non-release builds when it is
  installed (`src/main.c:485`). All 29 PCRE2 sources compile under it; JIT is
  disabled automatically (`__TINYC__` guard in `config.h`) and the interpreter
  takes over, producing identical results.
- **`--freestanding` / `VT_NO_LIBC`** — not usable. PCRE2 needs libc.

## Design notes

- The shim is pure C over PCRE2's public API. `pcre2_code`, `pcre2_match_data`,
  `PCRE2_SPTR` and PCRE2's option and error numbering never cross into Vyto.
- Strings cross as an explicit `(pointer, byte length)` pair, never as a bare
  NUL-terminated `cstring`. So patterns and subjects containing NUL bytes work,
  and `vyto/regex` does not inherit the "truncates at the first NUL" caveat that
  `lib/vyto/util/text.vt:12-15` documents.
- Group text is sliced out of the Vyto subject string using the byte offsets,
  never copied back from C. That keeps embedded NULs intact and avoids a
  `strlen` per group.
- `findAll` and `split` handle the zero-width-match trap the way upstream
  documents: retry anchored and non-empty at the same offset, then advance by a
  whole character — and over a `\r\n` pair as one unit — rather than by a byte.
  `new Regex("", 0).findAll("héllo")` returns 6 matches (5 characters plus the
  end), not 7.
- The pattern cache is a C `static` because Vyto has no module-level mutable
  state. Entries are owned by the cache and never handed out as a `Regex`; if
  one were, `Regex.deinit` would free a `pcre2_code` the cache still points at.
  That is why `rx_*` are free functions rather than a memoising constructor.

## Tests

- `tests/fixtures/regex_match.vt` — exhaustive, including every soft-failure
  path: compile errors and their offsets, unset groups, named groups both
  directions, zero-width `findAll`, per-character advance over UTF-8, embedded
  NULs in both pattern and subject, invalid UTF-8 and the `RX_BYTES` escape,
  `replace`'s grow-and-retry, `$1`/`${name}`/`$$`, `split`/`splitN`, the ReDoS
  limit, and cache fill/evict/clear.
- The suite runs that fixture **twice**, the second time with
  `VYTO_REGEX_JIT=0`, against the same golden. The output is JIT-invariant by
  construction, which is what makes the module safe on targets without a JIT.
- `examples/83_regex.vt` is the runnable tour.
- Two things the fixture deliberately never prints: PCRE2's own error message
  strings (upstream rewords them between releases) and anything that varies with
  whether the JIT is active.
- The panic paths — `test`, `findAll`, `replace*` and `split*` on a limit hit,
  and any match on an invalid `Regex` — are not in the golden, because
  `run_tests.sh` has no `contains` mode on Linux.
