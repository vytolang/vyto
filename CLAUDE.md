# Vyto

A systems language that compiles to C99. `vytoc` emits C and shells out to a
host C compiler; there is no VM, no GC (reference counting instead), and no
LLVM. Anywhere a C compiler exists, Vyto runs.

Source files are `.vt`. The runtime prefix is `vt_*`. The install root env var
is `VYTO_HOME`; `VYTO_PATH` (or `--modpath`) adds package roots to the import
search — see "Module resolution" below.

## Goal

The language is designed for aggressive memory safety, small efficient
binaries, low memory consumption, low latency and be as fast as C while making
the code stupidly easy to read and write.

Every piece of builtin library code must adhere to this goal.

## Build and test

```sh
make                  # builds vytoc + vytobind + vytopack
make test             # full suite: every examples/NN_*.vt vs its .expected
make test-pack        # vytopack: install, manifest, audit, shell-injection
make test-charts      # lib/vyto/ui/chart.vt
make test-mobile      # lib/vyto/mobile/android/ui.vt
make test-win         # cross-build the windows-x64 slice into tests/tmp/win-x64
make clean-cache      # remove every .vyto-cache in the tree
```

**`make test` is not the whole suite.** Four targets are split out of it, so a
green `make test` says nothing about what they cover — run the matching one
after touching its area, and all of them before a release:

| Target | Covers | Split out because |
|---|---|---|
| `test-pack` | 36 checks over `src/vytopack` — install, manifest parsing, `audit`, and the URL/rev/dir rejection cases | shells out to `git` repeatedly. No network: it clones a throwaway repo it creates under `tests/tmp` |
| `test-charts` | 21 golden cases over `lib/vyto/ui/chart.vt` | leaf package; every entry file compiles its own copy of the ui stack |
| `test-mobile` | 18 golden cases over `lib/vyto/mobile/android/ui.vt` | same, and the widgets need the headless surface |
| `test-win` | the windows-x64 portable slice | only *stages* into `tests/tmp/win-x64`; runs nothing. Copy it to a Windows box to execute |

`test-pack`'s rejection cases are the ones worth knowing about: they assert the
refusal *reason*, not merely that the command failed, because fault injection
showed an exit-status-only check passed with validation stubbed out entirely.
Anything touching `safety.vt` needs this target, not `make test`.

`make test` runs all 108 examples and is slow. While iterating, build and run
the single example you care about directly:

```sh
./vytoc run examples/42_whatever.vt
```

Save the full suite for the end of a task.

### `.vyto-cache` serves stale objects on lib-only edits

Emitted C and objects are cached per entry-file directory. Editing only a
library `.vt` does not always invalidate it, so a build can validate the
*previous* code. **Before regenerating any golden `.expected`, run
`make clean-cache`** — otherwise the golden records the stale build.

## Layout

| Path | What |
|---|---|
| `src/` | the compiler: lex, parse, check, emit. `emit.c` writes C; `main.c` drives the toolchain |
| `runtime/` | ~1200 lines. Depends only on `malloc`, `stdio`, `dirent`, `stat` |
| `lib/vyto/` | the stdlib, one directory per package |
| `lib/vyto/ui` | the Vyto UI Toolkit |
| `examples/` | `NN_name.vt` + `NN_name.expected` — these *are* the test suite |
| `docs/` | user-facing docs |
| `local/docs/` | design docs, gitignored (see CLAUDE.local.md) |
| `vytobind` | C header → Vyto FFI binding generator |

## Module resolution

An import probes, in order: the **importing file's own directory** (not the
entry file's — this is what lets a package's internal imports work wherever the
package is dropped), then each registered **package root**, then
`$VYTO_HOME/lib` last so nothing shadows the stdlib. Roots come from `--modpath`
(repeatable), `$VYTO_PATH` (`:`-separated, ignored entirely if any `--modpath`
was passed), and a `vyto_modules/` found by walking up from the entry file.

**A root contains packages; it is not itself a package** — same shape as `lib/`
holding `vyto/`. For a checkout at `/src/vytoweb`, pass `--modpath /src`.

**A module name is global.** It prefixes every emitted C symbol (`v_<mod>_<n>`),
the header guard, and the flat cache filenames `mod_<mod>.c/.h/.o`, so two
modules sharing one is a fatal error, not a warning. A file **under a root** is
named by its root-relative path (`vyto_modules/vytoweb/router/router.vt` →
`vytoweb_router`, `lib/vyto/ui/ui.vt` → `vyto_ui`); a file under **no** root
keeps its bare stem. That is what lets two packages each ship a `config.vt`.

The duplicate-leaf collapse only fires when the leaf equals its parent
(`store/store.vt` → `..._store`), so `vyto-kv/src/store.vt` is
`vyto_kv_src_store`. Stripping a `src/` root is a manifest concept and is
deliberately not hardcoded — see `local/docs/PACKAGING.md` §3.2.

Roots are canonicalized and de-duplicated at registration. That is load-bearing,
not hygiene: `load_module` realpath's the module path before its prefix test, so
a non-canonical root silently falls through to bare-stem naming.

## Memory model

Reference counting, no GC, no tracing. Frees are deterministic — which is what
makes the low-latency and low-memory goals reachable — but **a strong reference
cycle leaks**. Nothing detects it.

Break cycles with `weak`, a real keyword (`src/lex.c:9`). It compiles to
`vt_weak_set` on assignment and `vt_weak_drop` in the destructor
(`src/emit.c:1349`, `src/emit.c:1734`), so a weak slot reads as null once the
target is gone rather than dangling.

**Reading a `weak` field gives an optional, and the checker enforces it.**
`this.win.skin` is a compile error; test the path first (`if (this.win != null)`,
an early `if (this.win == null) { return; }`, a `&&` short-circuit, or bind a
local and check it). Writing to the path drops the narrowing. The rule exists
because a weak slot is *designed* to read null, so faulting on it is the wrong
failure mode — see `docs/memory.md` §3 and `tests/errors/weak_deref.vt`.

**Any back-reference — child to parent, part to owner — must be `weak`.** The
toolkit does this consistently and it is the pattern to copy:

```vyto
parent: weak Widget;        // ui/core.vt:1102
win: weak Window;           // ui/core.vt:1143
last_click_w: weak Widget;  // ui/core.vt:1868
```

Owning references (a parent's `children` array) stay strong. Getting this
backwards produces a leak that no test will catch, so it is worth checking
deliberately whenever a type gains a field pointing "up" or "sideways".

### Bulk data goes in an arena, not one allocation per element

The runtime pools small allocations by size class (`runtime/vyto_rt.c:26-53`),
so a single object is cheap. What is not cheap is *count*: a million-element
structure built one object per element pays a million headers, a million
refcount lifetimes, and scattered cache lines. That is the difference between
hitting the low-memory and low-latency goals and missing them.

**For lists, tables, and text at scale, allocate one contiguous backing buffer
and hand out offsets into it.** Elements become indices, not objects.

The reference implementation is `lib/vyto/data/native/src/coltable.c` — read it
before writing anything similar. Its shape:

- struct-of-arrays: each column is one typed buffer, so a cell is one indexed
  load with zero per-cell boxing
- strings are `(offset, len)` into a **shared string arena**, not a `string`
  per cell
- sort and filter never move column data; they permute an `idx` array that
  names the visible rows, so a 1M-row sort is a permutation of one `i64` array
- growth doubles, and `ct_reserve` pre-sizes so a bulk load is realloc-free

`StringBuilder` (`lib/vyto/util/text.vt`) is the same idea in the small: one
growing buffer instead of a fresh string per concatenation, and `clear()`
deliberately keeps the capacity so the buffer is reused across iterations
rather than reallocated.

Concrete trap: **`readlines()` allocates a string per line — a memory cost, not
a speed one.** Measured on a 55 MB / 1M-line file: **157 MB peak RSS vs 110 MB**
for `readfile()` plus one pass over the single buffer (+43%), while wall time is
a wash (0.13 s vs 0.14 s — `readlines()` is marginally *faster*). The size-class
pool makes each small string a bump in a pre-allocated chunk rather than a
`malloc`, and `vt_file_lines` reads the file once and `memchr`s through it
(`runtime/vyto_rt.c:869`), so there is no per-line I/O or re-copy to avoid.

So reach for `readfile()` when the file is large relative to available memory —
not for throughput. Rewriting a working `readlines()` loop to chase latency is
wasted effort and costs readability.

This is a rule for bulk and unbounded data. Do not arena a handful of items —
it costs readability, and "stupidly easy to read" is in the goal too.

## Calling C (FFI)

Declare foreign functions in an `extern "C"` block, then `#link` what they need:

```vyto
#link "curl" if "linux"

extern "C" {
    fn gfx_fill_rect(c: rawptr, x: f64, y: f64, w: f64, h: f64, rgb: i32);
    fn vt_vfs_ptr(key: cstring): rawptr;
}
```

FFI parameter types are the C-shaped ones — `i32`, `i64`, `f64`, `cstring`,
`rawptr`, `clong`, `culong` — not Vyto's `int`/`float`/`string`.

**Conversions are explicit at every call site. This is not stylistic; the
checker requires it.**

| Direction | How |
|---|---|
| `int` → `i32` | `color as i32` — Vyto `int` is 64-bit, so this narrows |
| `float` → `f64` | nothing; already exact |
| `string` → `cstring` | `path.cstr()` |
| `cstring` → `string` | `str(p)` — a builtin, not an import |
| `T[]` → `rawptr` | `xs as rawptr`, **plus** `xs.len as i32` as a separate argument |

Arrays have no `.ptr()` method. Casting the array itself is the idiom:

```vyto
fn fillPolygon(xs: float[], ys: float[], rgb: int) {
    gfx_fill_polygon(this.handle, xs as rawptr, ys as rawptr,
                     xs.len as i32, rgb as i32);   // gfx/gfx.vt:156
}
```

A `float[]` reaches C as `double*`, since Vyto `float` is `f64`. A shim that
wants `float32` must narrow on the C side.

`vytobind` generates these blocks from a C header — `./vytobind foo.h --lib
name@platform --filter 'foo_*'` prints a `.vt` module on stdout. Checked-in
examples: `lib/vyto/surface/vsurf.vt`, `examples/greeter/greeter.vt`.

## Things that will bite you

**Binary operands evaluate right to left.** Documented at `docs/strings.md` §2.
Never chain side effects in one expression. The one exception is a **template
literal**, whose holes evaluate left to right — the emitted join fills a parts
array through comma operators, which are sequence points, so `` `{a()}{b()}` ``
is ordered where `a() + b()` is not.

**`native/src/*.c` is globbed flat and every file compiles on every target.**
There is no per-file platform filter. A platform-specific source must wrap its
whole body in `#ifdef`, e.g. `lib/vyto/surface/native/src/vsurf_android.c`.

**A shim that reaches past the six `vt_host_*` hooks needs a `VT_NO_LIBC` arm.**
`--freestanding` splices `-ffreestanding -DVT_NO_LIBC -fno-builtin` into every
*package shim's* compile line, not just the runtime's (`src/main.c`), so an
unguarded `#include <sys/mman.h>` breaks the freestanding build of any program
that merely imports the package. The stub arm returns each entry point's
documented failure sentinel, which keeps the `.vt` free of platform
conditionals. First instance: `lib/vyto/mmap/native/src/mmap_shim.c`. Nothing
catches this by accident — the freestanding test builds `01_hello.vt`, which
imports nothing, so `tests/fixtures/mmap_freestanding.vt` exists to exercise it.

**`#link` conditions are OS-*prefix* matches on the target triple.**
`#link "X11" if "linux"` matches `linux-x64` and `linux-arm64` — and does *not*
match `android-arm64`. A non-matching condition silently drops the library;
nothing warns. Adding a triple means auditing all `#link` sites.

**Cross-compiling needs a triple, not just `--cc`.** Object filenames encode
release/debug but not the compiler or triple, so `--cc <other-toolchain>`
without `--target` silently reuses host objects and produces a bad link. The
known triples are a closed table in `src/main.c` (`cross_cc_table`).

**Generics are monomorphized**, non-generic base classes only, no bounds.

## Architectural seams worth not breaking

**`vyto/ui` must not depend on `vyto/gfx`.** The invariant is stated at
`lib/vyto/ui/core.vt:709` and currently holds — the only mentions of `vyto/gfx`
in `lib/vyto/ui` are comments. `Painter` (`ui/core.vt:497`) is the rendering
seam: ~45 virtuals where every rich operation degrades to a lean one on its
own. Renderers are swapped at runtime with `win.use_painter(p)`, and no library
constructs one — only app code does.

`Path` comes from `vyto/geom/path`, deliberately: it is the one structured type
in the `Painter` API and it must stay renderer-neutral.

**`Window.repaint()` does partial *drawing*, not just partial presenting.**
`ui/core.vt:2111-2146` clips to the dirty rect and redraws only intersecting
widgets, assuming everything outside is still in the backbuffer. Any renderer
without a persistent backbuffer will produce garbage, not degraded output.

**A looping `Tween` repaints every frame.** Never use one as a passive timer,
and never re-arm one from `render()`.

## Native dependencies

blend2d (`vyto/gfx`) and ICU (`vyto/intl`) binaries are **not in git** — they
are built by `lib/vyto/*/native/build-*.sh`. On a fresh clone the gfx tests
silently skip rather than fail. `vyto/regex` (pcre2), `vyto/crypto/ecc`
(micro-ecc) and `vyto/media/image` (stb_image_write) *are* vendored and need
nothing; each carries a `native/refresh-*.sh --verify` that checks the committed
tree against its sha256 manifest.

That silent skip is exactly why those three are vendored — a must-have module
whose tests quietly do not run on a fresh clone is worse than one that is
missing. It is also why `vyto/media/image` splits encode from decode: encode is
vendored and always works, and `vyto/media/image/decode` is a separate module
because it reaches blend2d.

