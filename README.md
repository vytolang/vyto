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
- **Safety**: bounds-checked arrays, checked downcasts, `panic` with
  file:line.

See [docs/SPEC.md](docs/SPEC.md) for the full language reference and
`examples/` for a tour (01 hello → 07 modules).

## Layout

```
src/       compiler (C99): lexer, recursive-descent parser, checker, C emitter
runtime/   volt_rt.{c,h}: RC objects, strings, arrays, maps, closures
examples/  01_hello … 07_modules + golden .expected outputs
docs/      SPEC.md
```
