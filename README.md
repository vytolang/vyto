# Volt

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

## A real app: VoltTodo

[apps/todo](apps/todo/) is a working X11 GUI todo manager written entirely
in Volt — a mini widget toolkit (virtual dispatch, closure event handlers,
weak parent refs), voltbind-generated Xlib bindings, two native packages,
file persistence, and deterministic display teardown via `deinit`.
AddressSanitizer-clean across full interactive sessions.

```sh
cd apps/todo && ../../voltc run todo.vt
```

## Quick start

```sh
make                              # builds ./voltc (plain C99, zero deps)
./voltc run examples/05_widgets.vt
make test                         # runs all examples against golden output
```

`voltc build file.vt` compiles to `.volt-cache/<name>` next to the source.
Generated C is human-readable — look inside `.volt-cache/`.

## Build a standalone executable

`voltc run` builds and runs in one step. To produce a distributable binary,
use `voltc build` — optionally with `-o` to choose the output path and
`--release` for the optimized (`cc -O2`) build:

```sh
./voltc build apps/snake/snake.vt --release -o snake   # → ./snake
./snake                                                 # run it directly
```

The result is a **self-contained native executable**: the Volt runtime (ref
counting, strings, arrays, the surface shim) is statically compiled in — there
is no `libvolt.so` to ship. It depends only on the platform's own libraries
(libc, plus e.g. libX11 for a GUI app), so you can copy it anywhere and run it
without `voltc` installed. A GUI app like snake lands around 48 KB; `strip` (or
`--cc 'cc -s'`) trims it further.

Cross-compile the same way with `--target` (see below):

```sh
./voltc build app.vt --release --target linux-arm64 -o app-arm64
```

Apps that use a **prebuilt native library** (e.g. `volt/gfx`, which links
blend2d) ship the executable plus that library's `.so` next to it by default —
the shared lib is then amortized across apps on the device. For single-file
distribution, `--bundle` statically links every prebuilt native lib (and the
C++/GCC runtimes) into one executable, so there is no `.so` to ship alongside;
it then depends only on base system libraries (libc, libX11):

```sh
./voltc build apps/uigfx/uigfx.vt --release --bundle -o app   # one file, no .so
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
- **voltbind**: generates the `extern` binding from a C header —
  `voltbind zlib.h --lib z --filter 'compress*' > zlib.vt`.
- **C callbacks**: `cthunk(closure)` turns a Volt closure into a C function
  pointer (userdata-first or `cthunk_last` for userdata-last APIs).
- **Cross-compilation**: `voltc build app.vt --target linux-arm64`
  (`--cc`/`VOLT_CC` for custom toolchains, e.g. `zig cc`).
- **Safety**: bounds-checked arrays, checked downcasts, `panic` with
  file:line.

See [docs/SPEC.md](docs/SPEC.md) for the full language reference,
[docs/PORTABILITY.md](docs/PORTABILITY.md) for the path to other platforms
(Windows, Android, iOS, embedded, …),
[docs/UI-TOOLKIT.md](docs/UI-TOOLKIT.md) for the bundled-toolkit strategy
(`volt/ui`, native facades, escape hatches), and `examples/` for a tour.

## Layout

```
src/       compiler (C99): lexer, recursive-descent parser, checker, C emitter
runtime/   volt_rt.{c,h}: RC objects, strings, arrays, maps, closures
lib/volt/  bundled stdlib modules: math (libm), surface, ui
examples/  01_hello … 12_math + golden .expected outputs
docs/      SPEC.md
```

## Native dependencies

The compiler itself (`make`) needs only a C99 toolchain — zero third-party
deps. Some stdlib packages bind native libraries and need a one-time setup
before use (a bare clone builds `voltc` and runs the core/stdlib examples
without them):

- **`volt/gfx`** (blend2d) — run `lib/volt/gfx/native/build-blend2d.sh`
  (needs cmake + ninja + a C++ compiler). Not vendored in git.
- **`volt/intl`** (ICU) — links the system ICU by default; install
  `libicu-dev` (Debian/Ubuntu) or `libicu-devel` (Fedora). For `--bundle` or
  targets without a system ICU, run `lib/volt/intl/native/build-icu.sh`.

## License

MIT — see [LICENSE](LICENSE).
