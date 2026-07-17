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

## 6. Imports and the standard library

- Your own files: `import { thing } from "other";` — a bare module stem,
  resolved local-first (an `other.vt` beside your source shadows anything else).
- The standard library lives under `lib/vyto/` and is imported as
  `vyto/<path>` — e.g. `vyto/util/json`, `vyto/io/file`, `vyto/util/fmt`,
  `vyto/ui/chart`, `vyto/ui/datatable`.

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

## 8. Where to go next

- **[examples/](../examples/)** — `01_hello` … `53_datatable`, a guided tour of
  the language and stdlib (structs, classes, closures, FFI, generics, reactive
  state, files, time, JSON, HTTP, sockets, worker pools, and the UI toolkit).
- **Showcase apps** — [apps/charts](../apps/charts/charts.vt) (15-chart gallery),
  [apps/datagrid](../apps/datagrid/datagrid.vt) (spreadsheet-grade DataTable),
  [apps/vytopad](../apps/vytopad/vytopad.vt) (a text editor).
- **[README](../README.md)** — language highlights, why it's fast, the standard
  library, native packages, and platform status.
- **Run the tests** — `make test` builds every example and checks it against
  golden output.
