# Vyto

A small, statically typed language with JavaScript-like syntax and Turbo
Pascal's soul: units with separate compilation, records and classes with
virtual dispatch, deterministic reference-counted memory, first-class C FFI,
and near-instant compiles via C transpilation. Designed as a foundation for
building cross-platform UI toolkits.

```js
class Button extends Widget {
    onClick: fn(Button);
    override fn draw(indent: string) { print(indent + "[ " + this.label + " ]"); }
}

fn main() {
    let ok = new Button("btn-ok", "OK");
    ok.onClick = (b) => print("clicked: " + b.label);
    ok.click();
}   // deterministic teardown: deinit runs here
```

## A real app: VytoTodo

[apps/todo](apps/todo/) is a working X11 GUI todo manager written entirely
in Vyto — a mini widget toolkit (virtual dispatch, closure event handlers,
weak parent refs), vytobind-generated Xlib bindings, two native packages,
file persistence, and deterministic display teardown via `deinit`.
AddressSanitizer-clean across full interactive sessions.

```sh
cd apps/todo && ../../vytoc run todo.vt
```

## Quick start

```sh
make                              # builds ./vytoc (plain C99, zero deps)
./vytoc run examples/05_widgets.vt
make test                         # runs all examples against golden output
```

`vytoc build file.vt` compiles to `.vyto-cache/<name>` next to the source.
Generated C is human-readable — look inside `.vyto-cache/`.

## Build a standalone executable

`vytoc run` builds and runs in one step. To produce a distributable binary,
use `vytoc build` — optionally with `-o` to choose the output path and
`--release` for the optimized (`cc -O2`) build:

```sh
./vytoc build apps/snake/snake.vt --release -o snake   # → ./snake
./snake                                                 # run it directly
```

The result is a **self-contained native executable**: the Vyto runtime (ref
counting, strings, arrays, the surface shim) is statically compiled in — there
is no `libvyto.so` to ship. It depends only on the platform's own libraries
(libc, plus e.g. libX11 for a GUI app), so you can copy it anywhere and run it
without `vytoc` installed. A GUI app like snake lands around 48 KB; `strip` (or
`--cc 'cc -s'`) trims it further.

Cross-compile the same way with `--target` (see below):

```sh
./vytoc build app.vt --release --target linux-arm64 -o app-arm64
```

Apps that use a **prebuilt native library** (e.g. `vyto/gfx`, which links
blend2d) ship the executable plus that library's `.so` next to it by default —
the shared lib is then amortized across apps on the device. For single-file
distribution, `--bundle` statically links every prebuilt native lib (and the
C++/GCC runtimes) into one executable, so there is no `.so` to ship alongside;
it then depends only on base system libraries (libc, libX11):

```sh
./vytoc build apps/uigfx/uigfx.vt --release --bundle -o app   # one file, no .so
```

## Why it's fast

- One pass per module: lex → parse → check → emit C. No IR.
- One `.c`/`.h` per module, content-hash cached — unchanged units are not
  re-emitted or recompiled (the Turbo Pascal unit model).
- Dev builds use `tcc` when installed, `cc -O0` otherwise; `--release` uses
  `cc -O2`. Typical rebuild on this repo's examples: ~50 ms with gcc.

## Highlights

- **Memory**: automatic ref counting, `weak` for back-references, `deinit`
  destructors that fire deterministically — widget trees tear down top-down.
- **Classes**: single inheritance, `virtual`/`override`, `new`, `super.init`.
- **Closures**: `(x) => expr`, typed `fn(T): U`, captures by value.
- **FFI**: `extern "C"` blocks, exact-layout structs, `#link "lib"`.
- **Native packages**: a module directory with `native/src/*.c` (compiled
  and linked automatically) or prebuilt `native/<platform>/*.so` (linked
  with an `$ORIGIN` rpath and shipped next to the executable).
- **vytobind**: generates the `extern` binding from a C header —
  `vytobind zlib.h --lib z --filter 'compress*' > zlib.vt`.
- **C callbacks**: `cthunk(closure)` turns a Vyto closure into a C function
  pointer (userdata-first or `cthunk_last` for userdata-last APIs).
- **Cross-compilation**: `vytoc build app.vt --target linux-arm64`
  (`--cc`/`VYTO_CC` for custom toolchains, e.g. `zig cc`).
- **Safety**: bounds-checked arrays, checked downcasts, `panic` with
  file:line.

See `examples/` (`01_hello` … `50_worker_pool`) for a tour of the language
and stdlib.

## Standard library

Bundled under `lib/vyto/`, imported as `vyto/<path>`. Modules marked ⚙ are
backed by a native shim (compiled from source or a `#link`ed system library);
everything else is pure Vyto.

**Core & utilities**

| Module | What it gives you |
|--------|-------------------|
| `vyto/util/fmt` ⚙ | printf-style formatting (`fmt`, `fixed`, `hex`, `commas`, …) |
| `vyto/util/json` | JSON parse/encode (`JsonValue`, `json_parse`, `json_encode`) |
| `vyto/util/date` ⚙ · `vyto/util/time` ⚙ | calendar dates (strftime/strptime) · clocks, durations, timers |
| `vyto/math` ⚙ | libm math (`sin`, `sqrt`, `pow`, …) |
| `vyto/io/file` ⚙ | files & paths (`File`, read/write, `file_exists`, `mkdirs`, …) |
| `vyto/os` ⚙ | environment, args, process helpers |
| `vyto/os/worker` ⚙ | `fork()`-based `WorkerPool` — CPU parallelism, no shared state |

**Internationalization** — `vyto/intl` ⚙ (ICU-backed)

| Module | What it gives you |
|--------|-------------------|
| `vyto/intl` | `Locale`, locale detection, shared enums |
| `vyto/intl/unicode` | codepoints, NFC/NFD normalization, locale case, grapheme/word/line segmentation, collation |
| `vyto/intl/number` | locale number / currency / percent formatting |
| `vyto/intl/datefmt` | locale date/time formatting |
| `vyto/intl/message` | ICU-MessageFormat subset + CLDR plurals + JSON catalogs |

**Networking** — `vyto/net` ⚙

| Module | What it gives you |
|--------|-------------------|
| `vyto/net/http` ⚙ | HTTP client, `HttpPool` fan-out (libcurl multi) |
| `vyto/net/socket` ⚙ | TCP/UDP sockets, non-blocking mode, `PollSet` event loop |
| `vyto/net/websocket` ⚙ | WebSocket client |

**Graphics & UI**

| Module | What it gives you |
|--------|-------------------|
| `vyto/surface` ⚙ | windowing + event loop (X11 / Win32 / framebuffer / headless) |
| `vyto/gfx` ⚙ | 2D canvas (blend2d): shapes, gradients, text, images |
| `vyto/ui` | widget toolkit — layout, widgets, dialogs, nav, skins (iOS/macOS/Material) |
| `vyto/anim` · `vyto/geom` | animation/easing · geometry & vector paths |

**Hardware** — `vyto/hw`

| Module | What it gives you |
|--------|-------------------|
| `vyto/hw/serial` ⚙ | serial / TTY ports as poll-able fds |
| `vyto/hw/usb` ⚙ | USB device enumeration |

Some packages need a one-time native setup — see [Native dependencies](#native-dependencies).

## Layout

```
src/       compiler (C99): lexer, recursive-descent parser, checker, C emitter
runtime/   vyto_rt.{c,h}: RC objects, strings, arrays, maps, closures
lib/vyto/  bundled stdlib modules (see "Standard library" above)
examples/  01_hello … 50_worker_pool + golden .expected outputs
```

## Native dependencies

The compiler itself (`make`) needs only a C99 toolchain — zero third-party
deps. Some stdlib packages bind native libraries and need a one-time setup
before use (a bare clone builds `vytoc` and runs the core/stdlib examples
without them):

- **`vyto/gfx`** (blend2d) — run `lib/vyto/gfx/native/build-blend2d.sh`
  (needs cmake + ninja + a C++ compiler). Not vendored in git.
- **`vyto/intl`** (ICU) — links the system ICU by default; install
  `libicu-dev` (Debian/Ubuntu) or `libicu-devel` (Fedora). For `--bundle` or
  targets without a system ICU, run `lib/vyto/intl/native/build-icu.sh`.

## License

MIT — see [LICENSE](LICENSE).
