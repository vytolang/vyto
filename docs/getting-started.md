# Getting started with Vyto

> **Status: experimental.** Vyto is a young language — the compiler, runtime,
> and standard library are all evolving, and breaking changes happen. Great for
> learning, prototyping, and open-source apps; not yet for mission-critical or
> production business systems.

This guide takes you from a clean checkout to your first running program and a
small GUI app, in about five minutes.

## 1. Prerequisites

- A **C99 compiler** — `cc`/`gcc`/`clang`. The compiler itself is plain C99 with
  zero dependencies.
- **make** and **git**.
- For **GUI apps**: the platform's windowing libraries (on Linux, `libX11`).
- Optional: **tcc** for faster debug builds (Vyto uses it automatically when
  present; falls back to `cc -O0`).

## 2. Get the source and build the compiler [Linux Only]

Clone the repo and build:

```sh
git clone https://github.com/vytolang/vyto.git
cd vyto
make                 # builds ./vytoc (and ./vytobind)
```

That produces `./vytoc`, the Vyto compiler, in the repo root. Verify it:

```sh
./vytoc run examples/01_hello.vt
```

You should see `hello, vyto` and a few counter lines.

## 3. Your first program

Create `hello.vt`:

```js
fn main() {
    print("hello, vyto");

    let n = 3;
    for (let i in 0..n) {
        print("tick " + i);
    }

    let x: float = 2.5;
    print("x*2 = " + x * 2.0);
}
```

Run it:

```sh
./vytoc run hello.vt
```

## 4. The two commands

- **`vytoc run <file.vt>`** — compile and run in one step. Use this while
  developing.
- **`vytoc build <file.vt>`** — compile to a native executable. Add `-o <path>`
  to choose the output, `--release` for the optimized (`cc -O2`) build:

  ```sh
  ./vytoc build hello.vt --release -o hello
  ./hello
  ```

The generated C is human-readable — look inside the `.vyto-cache/` directory
created next to your source.

### The two caches

Builds use two caches, and knowing which is which saves confusion:

| | Where | Holds |
|---|---|---|
| **Local** | `.vyto-cache/` next to the file you ran | the generated C, its objects, and the executable |
| **Shared** | `.vyto-cache/obj/` at the root of the Vyto tree | objects compiled from the runtime and from native packages' C |

The shared one exists because a native package's C does not depend on the
program being built. `vyto/regex` vendors 30 translation units of PCRE2; without
sharing, every directory you ran a program from compiled its own copy. Entries
are keyed on a hash of the source contents **and** the full compile command, so
switching branches or touching a file cannot serve a stale object — and reverting
a change makes the previous objects valid again instantly.

Generated C stays local on purpose: a generic instantiated with your types is
emitted into the module that *declared* the generic, so a stdlib module's C
depends on your program.

Delete either at any time — they only cost a rebuild. `make clean-cache` removes
every one in the tree, including the shared one. `--clean` clears only the local
cache for that file. `VYTO_OBJ_CACHE=<dir>` moves the shared cache;
`VYTO_OBJ_CACHE=off` disables sharing entirely.

## 5. A small GUI app

Vyto ships a native UI toolkit, `vyto/ui`. It renders on two tiers: a lean X11
surface, and a rich anti-aliased tier over blend2d — the *same* widget code runs
on both.

Create `app.vt`:

```js
import { Window, Column, Row, Label, Button, TextField } from "vyto/ui";

fn main() {
    let win = new Window("My first Vyto app", 360, 220);
    win.root = new Column([
        new Label("Hello from vyto/ui"),
        new Row([new TextField("type here"), new Button("Add")]),
        new Button("OK"),
    ]);
    win.run();
}
```

Run it:

```sh
./vytoc run app.vt
```

> **Rich (blend2d) tier.** The example above runs on the lean X11 tier, which
> needs no extra setup. The anti-aliased rich tier (`vyto/gfx` / `GfxPainter`)
> is backed by **blend2d**, a large C++ library that is *not* vendored in git —
> build it once before using that tier:
>
> ```sh
> lib/vyto/gfx/native/build-blend2d.sh          # defaults to linux-x64
> ```
>
> It clones and compiles blend2d, then drops the headers and
> `libblend2d.{so,a}` the package needs. Requires **git, cmake (≥3.22), a C++
> compiler, and ninja**.

To wire up interactivity (button clicks, text input, state), see
[examples/05_widgets.vt](../examples/05_widgets.vt). One thing to know up front:
**closures capture by value**, so a click handler can call methods on a captured
widget (widgets are references) but cannot mutate a captured primitive and have
it persist — hold changing state in an object or a top-level structure instead.

A handler does not have to be a closure literal. Anywhere a `fn(...)` type is
expected you can also pass a named function, a generic function (instantiated
from the target type), or a **bound method** — `obj.method`, which carries its
receiver, so the handler can reach that object's state without capturing
anything:

```js
class App {
    hits: int;
    fn health(req: ServerRequest, res: ServerResponse) {
        this.hits = this.hits + 1;
        res.text(200, "ok");
    }
}

let app = new App();
s.get("/health", app.health);      // no arrow, and it still sees app.hits
```

See [examples/98_fn_values.vt](../examples/98_fn_values.vt) for all four forms.

**A bound method holds its receiver alive.** Storing one back onto an object the
receiver owns — `this.btn.onPress = this.handle` — makes a reference cycle, and
Vyto has no cycle collector, so it leaks with nothing failing. Clear the field
during teardown, or keep the back-reference `weak`.

## 6. Imports and the standard library

- Your own files: `import { thing } from "other";` — a bare module stem,
  resolved local-first (an `other.vt` beside your source shadows anything else).
- The standard library lives under `lib/vyto/` and is imported as
  `vyto/<path>` — e.g. `vyto/util/json`, `vyto/io/file`, `vyto/util/fmt`,
  `vyto/ui/chart`, `vyto/ui/datatable`.
- Third-party packages live in a **package root** — a directory that *contains*
  packages, exactly as `lib/` contains `vyto/`.

An import is resolved by probing, in order:

1. the importing file's own directory (so a local file always wins, and a
   package's internal imports keep working wherever the package is dropped)
2. each `--modpath <dir>`, in the order given — repeatable, relative to the
   current directory
3. `$VYTO_PATH`, a `:`-separated list of the same, **ignored entirely if any
   `--modpath` was passed**
4. `vyto_modules/`, found by walking up from the entry file's directory
5. `$VYTO_HOME/lib` — last, so nothing can shadow the standard library

Pointing at a single checkout means naming its *parent*: for a package at
`/src/vytoweb`, pass `--modpath /src`.

**A file under a package root is named by its root-relative path**, not by its
bare stem — `vyto_modules/vytoweb/router/router.vt` is the module
`vytoweb_router`, the same rule that makes `lib/vyto/ui/ui.vt` into `vyto_ui`.
Because the package directory is always part of the name, two packages may each
ship a `config.vt` and both still load. A file outside every root keeps its bare
stem, and two of those sharing a stem is an error: a module name prefixes every
emitted C symbol, so the collision is unbuildable.

### Getting a package

A package root is just a directory, so **unzipping a download into
`vyto_modules/<name>/` is all it takes** — no manifest and no tool:

```
myapp/
  main.vt                        import { kv_get } from "vyto-kv/store";
  vyto_modules/vyto-kv/store/store.vt
```

`vytopack` automates the bookkeeping when you want it — resolving a tag to an
exact commit, recording it in `vyto.lock`, hashing what landed, and sharing a
cache across projects:

```sh
vytopack install --url=https://github.com/vytolang/vyto-kv --rev=v0.3.1
```

Both routes produce the same tree and the same module names. See
`src/vytopack/README.md`.

Each `.vt` file is one compilation unit, content-hash cached, with automatic
dead-code stripping at link time — an unused import costs essentially nothing in
the shipped binary. See the **Standard library** table in the
[README](../README.md#standard-library) for the full module list.

## 7. Shipping a binary

`vytoc build` produces a **self-contained native executable** — the Vyto runtime
is statically compiled in, so there is no `libvyto.so` to ship. It depends only
on base system libraries (libc, plus e.g. libX11 for a GUI app), so you can copy
it and run it without `vytoc` installed.

```sh
./vytoc build app.vt --release -o app          # native binary
./vytoc build app.vt --release --target linux-arm64 -o app-arm64   # cross-compile
```

Apps that use a prebuilt native library (e.g. `vyto/gfx`, which links blend2d)
ship the executable plus that `.so` beside it by default; add `--bundle` to
statically link everything into one file.

## 8. Cross-compiling for Windows

`windows-x64` builds are produced on Linux with the mingw-w64 toolchain:

```sh
sudo apt-get install -y gcc-mingw-w64-x86-64      # provides x86_64-w64-mingw32-gcc
./vytoc build app.vt --release --target windows-x64 -o app.exe
```

`vytoc` picks `x86_64-w64-mingw32-gcc` for that triple automatically. Add
`-static-libgcc` if the `.exe` has to run on a machine with no mingw runtime
DLLs:

```sh
./vytoc build app.vt --target windows-x64 --cc "x86_64-w64-mingw32-gcc -static-libgcc"
```

### What is portable today

The **core language, the runtime, and the stdlib packages with no POSIX
dependency**: `vyto/math`, `vyto/reactive`, `vyto/util/*` (fmt, json, sort, text,
time, date, and the data-format modules — csv, xml, toml, ini, markdown, html,
url, mime, log), `vyto/io/file`, `vyto/data/frame`, `vyto/os/os`, `vyto/anim`,
`vyto/cli`, `vyto/geom/*`, `vyto/geo/*`, plus FFI, native packages with in-tree C,
and prebuilt `native/windows-x64/*.dll` packages.

`vyto/util/uuid` is portable too, though unlike its `vyto/util` siblings it does
carry a native shim: `getrandom(2)` on Linux, `BCryptGenRandom` on Windows (so it
`#link`s `bcrypt` there), `/dev/urandom` otherwise.

`vyto/regex` is portable and needs no provisioning, which is worth calling out
because it carries by far the most in-tree C of any module: PCRE2 and its JIT
are vendored under `lib/vyto/regex/native/src/pcre2/` and compiled from source
like any other shim. That is exactly why it sits here rather than in the "not
portable" list with `vyto/intl` — there is no system library to be missing. Where
the JIT cannot run it falls back to PCRE2's interpreter with identical results.

**GUI and graphics**: `vyto/surface` (the Win32 GDI backend), `vyto/ui`, and
`vyto/gfx` all build for `windows-x64` — see the next section for the one
prerequisite `vyto/gfx` has.

**Networking**: `vyto/net/socket` builds on Winsock2 (`ws2_32`, linked
conditionally), covering TCP, UDP and `PollSet`. `vyto/net/http` and
`vyto/net/websocket` build too, against the prebuilt `libcurl-x64.dll` in
`lib/vyto/net/native/windows-x64/` — the ordinary prebuilt-native-package path,
so `vytoc` copies the DLL next to your executable. One caveat that is not
Vyto's: that DLL imports `api-ms-win-crt-*`, so it needs the Universal CRT.
Windows 10 and later ship it; Windows 7 needs KB2999226.

**Parallelism**: `vyto/os/worker` builds, but it is a **re-exec pool rather than
`fork()`** — Windows has no `fork`. Workers are the same executable relaunched
with `VYTO_WORKER_*` in the environment, re-running `main` until they divert
into serve mode, over a loopback socket with a `BCryptGenRandom` token
handshake. The consequence is real and worth designing around: **anything
`main()` does before that point runs once per worker.**

The fix is one line, and it is worth taking on every platform:

```js
fn main() {
    workerEntry((input) => heavyCompute(input));   // returns instantly in the parent
    ...
}
```

A re-launched worker serves jobs there and never returns, so nothing above it
can run twice. It also decouples the pool from startup — build it after a window
is open, or call `respawn()` to replace a worker that died.

**Memory mapping**: `vyto/mmap` builds everywhere. POSIX `mmap` covers Linux and
Android; Windows goes through `CreateFileMapping`/`MapViewOfFile`. One thing the
shim handles that a caller would otherwise get wrong: `mmap_at`'s offset is
rounded down to the platform's granularity, which is 4 KiB on POSIX and **64 KiB
on Windows** — you always get a base at the byte you asked for. `advise()` is
honoured on POSIX; on Windows only `WILLNEED` does anything (via
`PrefetchVirtualMemory`, resolved at runtime so the binary still loads on Win7)
and the rest are successful no-ops.

**Not portable**, and excluded from the Windows suite: `vyto/net/link`, `wifi`
and `raw` (Linux netlink/AF_PACKET), all of `vyto/hw/*` (Linux device
interfaces), and `vyto/intl` (ICU).

Three Windows-specific behaviours worth knowing: `vyto/os`'s `run()` and
`capture()` go through `cmd.exe`, not a POSIX shell, so shell built-ins differ;
`vyto/util/date`'s `parse()` runs on a strptime written for this port (Windows
ships none), covering the conversions `format()` can round-trip — anything else
returns the invalid-date sentinel rather than guessing; and `PollSet` uses
`WSAPoll`, which never reports a *failed* non-blocking connect, so a refused
`connectAsync` stays un-ready instead of surfacing an error. Put your own
timeout around it there.

### Graphics apps: blend2d and fonts

`vyto/gfx` is the one stdlib package backed by a **prebuilt** library rather than
in-tree C, so it needs a `windows-x64` build of blend2d before anything importing
it will link. blend2d is C++, so this needs the mingw C++ compiler too:

```sh
sudo apt-get install -y g++-mingw-w64-x86-64
./lib/vyto/gfx/native/build-blend2d.sh --dry-run windows-x64   # confirm the plan
./lib/vyto/gfx/native/build-blend2d.sh windows-x64
```

Every triple is built from **one** clone at **one** revision, stamped in
`native/blend2d.commit`. The headers in `native/src/blend2d/` are
shared by all platforms, so adding a triple checks that exact commit out rather
than taking current master — otherwise the headers would describe a newer
blend2d than the libraries already on disk were compiled from. `--refresh` is
the deliberate way to move the revision forward, and it rebuilds every triple.

Forgetting this step is not mysterious; `vytoc` says so:

```
package 'vyto_gfx' ships prebuilt native libraries, but none for windows-x64
  (has: linux-x64) — build one with lib/vyto/gfx/native/build-blend2d.sh windows-x64
```

**Fonts.** A graphics app that hardcodes a system font path will not find one on
Windows. Vendor the typeface into the app instead and embed it with
`--with-assets` — `apps/charts` and `apps/motiondemo` both do this:

```js
import { appDir } from "vyto/os";

fn font_path(): string {
    return appDir() + "/assets/Inter-Regular.ttf";
}
```

```sh
./vytoc build apps/charts/charts.vt --release --with-assets \
    --target windows-x64 -o charts.exe
```

One path serves both cases. Unbundled, `appDir()` makes it absolute so it
resolves from any working directory; bundled, the runtime VFS matches it by
suffix and the font comes out of the binary. `Canvas.loadFont` and
`GfxPainter` both consult the VFS before the disk. If you use `GfxPainter`,
ship the `-Medium` and `-Bold` siblings alongside the regular face — it derives
their paths from it.

By default the app ships as an exe plus `libblend2d.dll`, which `vytoc` copies
next to it. Adding `--bundle` links blend2d statically instead, for a single
file with nothing beside it — combine it with `--with-assets` and the whole app,
fonts included, is one exe that depends only on system DLLs.

### Testing on Windows

There is no Windows CI and no emulator in the loop — the binaries are checked on
a real machine:

```sh
make test-win        # cross-builds and stages tests/tmp/win-x64/
```

That stages one `.exe` per portable example and fixture, each with its golden, a
`manifest.txt`, and a generated `run.ps1`. It also runs one host-side test:
`tests/win/strptime_test.c` compiles `date_shim.c`'s `_WIN32` branch natively so
the hand-written strptime is verified without a Windows box.

Copy `tests/tmp/win-x64/` (or the `vyto-win-x64.zip` beside it) to a Windows
machine and run:

```powershell
powershell -ExecutionPolicy Bypass -File run.ps1
```

Then copy `results.txt` back. Goldens are resolved as
`<name>.expected.windows-x64` when present, falling back to `<name>.expected` —
that is how a program whose correct output genuinely differs per platform (such
as `examples/45_os.vt`) is handled.

## 9. Where to go next

- **[examples/](../examples/)** — `01_hello` … `53_datatable`, a guided tour of
  the language and stdlib (structs, classes, closures, FFI, generics, reactive
  state, files, time, JSON, HTTP, sockets, worker pools, and the UI toolkit).
- **Showcase apps** — [apps/charts](../apps/charts/charts.vt) (15-chart gallery),
  [apps/datagrid](../apps/datagrid/datagrid.vt) (spreadsheet-grade DataTable),
  [apps/vytopad](../apps/vytopad/vytopad.vt) (a text editor).
- **[Strings & regular expressions](strings.md)** — every string operation in
  one place: the built-in methods and their exact edge cases, `vyto/util/text`,
  the `vyto/regex` engine, and the Unicode-aware operations in
  `vyto/intl/unicode`.
- **[Types](types.md)** — every type in the language, value vs. reference,
  the fixed-width FFI family, arrays, `Map`, `struct`, and casts.
- **[Classes](classes.md)** — construction and `init` inheritance,
  `virtual`/`override`, `super`, `builder` methods, `deinit`, `weak`, checked
  downcasts, duck-typed `for-in`, and generic/arena-allocated classes.
- **[Memory](memory.md)** — the refcounting model, what it costs, the
  reference-cycle gotcha and `weak`, arenas, and the bulk-data allocation
  trap.
- **[Generating native bindings](native-bindings.md)** — turning a C header
  into a Vyto `extern "C"` module with `vytobind`.
- **[Math](math.md)** — `vyto/math` (constants, libm, integer helpers),
  `vyto/math/stats` and `vyto/math/random` (statistics and seeded
  randomness), and `vyto/math/algebra` (matrices, linear systems).
- **[Graphics](graphics.md)** — the drawing stack top to bottom: `vyto/geom`
  math, the `vyto/surface` and `vyto/gfx` rendering tiers, and what Vyto can
  and can't currently draw.
- **[Multiple windows](multiwindow.md)** — driving several OS windows from one
  process: the event loop that covers them all, monitors and placement,
  chromeless windows, and previewing one window inside another.
- **[Hardware & peripherals](hardware.md)** — cameras, sensors, GPIO, serial,
  USB and GPS through `vyto/hw/*`.
- **[README](../README.md)** — language highlights, why it's fast, the standard
  library, native packages, and platform status.
- **Run the tests** — `make test` builds every example and checks it against
  golden output; `make test-win` stages the Windows-portable slice for a real
  Windows machine (see section 8).
