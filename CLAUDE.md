# Vyto

A systems language that compiles to C99. `vytoc` emits C and shells out to a
host C compiler; there is no VM, no GC (reference counting instead), and no
LLVM. Anywhere a C compiler exists, Vyto runs.

Source files are `.vt`. The runtime prefix is `vt_*`. The install root env var
is `VYTO_HOME`.

## Goal

The language is designed for aggressive memory safety, small efficient
binaries, low memory consumption, low latency and be as fast as C while making
the code stupidly easy to read and write.

Every piece of builtin library code must adhere to this goal.

## Build and test

```sh
make                  # builds vytoc + vytobind
make test             # full suite: every examples/NN_*.vt vs its .expected
make test-win         # cross-build the windows-x64 slice into tests/tmp/win-x64
make clean-cache      # remove every .vyto-cache in the tree
```

`make test` runs all 87 examples and is slow. While iterating, build and run
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

Concrete trap: **`readlines()` allocates a string per line.** On a large file
call `readfile()` once and slice the single buffer instead.

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
silently skip rather than fail. `vyto/regex` (pcre2) and `vyto/crypto/ecc`
(micro-ecc) *are* vendored and need nothing.

