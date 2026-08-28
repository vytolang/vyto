# Types in Vyto

> **Status: experimental.** Names and edge-case behaviour can still change.

Every type in Vyto, what it's for, and whether it's a value or a reference.
The short version: `int`, `float`, `bool`, `byte`, and `struct` are copied;
`string`, arrays, `Map`, `class`, and `fn` values are reference-counted
pointers under the hood. Nothing here needs an import — these are the types
the parser and checker know natively (`src/ast.h:17`). For what that
refcounting actually costs, the cycle gotcha, and arenas, see
[`docs/memory.md`](memory.md).

## 1. Value types

| Type | C-side width | Signed | Notes |
|---|---|---|---|
| `bool` | 1 byte | — | `true` / `false` |
| `int` | 64-bit | yes | **the** general-purpose integer. Identical to `i64` — `int` is just its name in ordinary code (`src/check.c:21`) |
| `float` | 64-bit | — | **the** general-purpose float. Identical to `f64` (`src/check.c:22`) |
| `byte` | 8-bit | no | 0–255. Identical to `u8` (`src/check.c:23`) |
| `void` | — | — | return-type only; not a value you can hold in a variable |

`int` and `float` never auto-coerce into each other or into the fixed-width
family below — every conversion is an explicit `as` cast:

```js
let n: int = 7;
let x: float = n as float;   // widening, always exact
let i: int = 3.9 as int;     // 3 — truncates toward zero, like C
```

See [`docs/math.md`](math.md) §0 for the full truncation-vs-floor discussion.

### Integer overflow

Signed `+`, `-`, `*` (and unary `-`) are **checked in debug builds** — an
overflow panics with a file:line message rather than wrapping. `--release`
opts into fast two's-complement wrapping instead. Unsigned arithmetic always
wraps, in both build modes, by definition:

```js
let u: u32 = 4294967295;
let w: u32 = u + 1;      // wraps to 0 — defined, unchecked, both modes
```

(`examples/13_overflow.vt`)

## 2. Fixed-width types (FFI-shaped)

These exist for talking to C — struct fields and `extern "C"` signatures —
not for everyday arithmetic. Reach for `int`/`float`/`byte` unless you're
matching a C type exactly. See [`docs/native-bindings.md`](native-bindings.md)
for how `vytobind` picks these when generating a binding from a header.

| Type | Width | Signed | Maps from C |
|---|---|---|---|
| `i8` | 8-bit | yes | `signed char` |
| `u8` | 8-bit | no | `unsigned char` (same width/sign as `byte`, different name) |
| `i16` | 16-bit | yes | `short` |
| `u16` | 16-bit | no | `unsigned short` |
| `i32` | 32-bit | yes | `int` |
| `u32` | 32-bit | no | `unsigned int` |
| `i64` | 64-bit | yes | `long long` (same as `int`, different name) |
| `u64` | 64-bit | no | `unsigned long long` |
| `f32` | 32-bit | — | `float` |
| `f64` | 64-bit | — | `double` (same as `float`, different name) |
| `clong` | target-width | yes | C `long` — 64-bit on LP64 (Linux/macOS), 32-bit on LLP64 (Windows) |
| `culong` | target-width | no | C `unsigned long` |

FFI parameter conversions are explicit at every call site — the checker
requires it:

| Direction | How |
|---|---|
| `int` → `i32` | `n as i32` (narrows — `int` is 64-bit) |
| `float` → `f64` | nothing; already exact |
| `string` → `cstring` | `s.cstr()` |
| `cstring` → `string` | `str(p)` |
| `T[]` → `rawptr` | `xs as rawptr`, plus `xs.len as i32` as a separate argument |

## 3. `string`

Immutable, reference-counted, UTF-8 byte sequence. A reference type — `null`
is a valid `string` value. Full method reference, byte-vs-character
semantics, and the regex/Unicode packages: [`docs/strings.md`](strings.md).

## 4. `cstring` and `rawptr`

FFI-only pointer types, not part of everyday Vyto code:

- **`cstring`** — a raw C `char*`. Convert with `s.cstr()` (`string` →
  `cstring`) and `str(p)` (`cstring` → `string`, a builtin).
- **`rawptr`** — an untyped pointer, the landing type for everything else
  that crosses the FFI boundary: `T[]` arrays (with a separate length arg),
  struct pointers, opaque handles, function pointers (pass one with the
  `cthunk()` builtin — see [`docs/native-bindings.md`](native-bindings.md)).

Both accept `null`.

## 5. Arrays — `T[]`

A growable, reference-counted array. Declared and indexed like C, one
dimension at a time (`string[][]` is an array of arrays):

```js
let nums: int[] = [10, 20, 30];
nums.push(40);
print(str(nums[nums.len - 1]));   // 40
```

`.len` is a field, not a call. Indexing panics out of bounds.

| Method | Signature | Notes |
|---|---|---|
| `.push(x)` | `(T) -> void` | |
| `.pop()` | `() -> T` | |
| `.first()` / `.last()` | `() -> T` | |
| `.nth(i)` | `(int) -> T` | |
| `.is_empty()` | `() -> bool` | |
| `.contains(x)` / `.index_of(x)` | `(T) -> bool` / `(T) -> int` | not on `struct`-element arrays — padding makes byte compare unreliable |
| `.reverse()` / `.clear()` | `() -> void` | in place |
| `.reserve(n)` | `(int) -> void` | pre-size the backing buffer |
| `.insert(i, x)` | `(int, T) -> void` | |
| `.remove_at(i)` | `(int) -> T` | |
| `.extend(other)` | `(T[]) -> void` | in place, appends `other` |
| `.concat(other)` | `(T[]) -> T[]` | new array |
| `.slice(lo, hi)` | `(int, int) -> T[]` | new array |
| `.fill(x)` | `(T) -> void` | overwrites every element |
| `.join(sep)` | `(string) -> string` | `string[]` only |
| `.map(fn(T): U)` | `-> U[]` | |
| `.filter(fn(T): bool)` | `-> T[]` | |
| `.each(fn(T): void)` | `-> void` | |
| `.find_index(fn(T): bool)` | `-> int` | |
| `.any(fn(T): bool)` / `.all(fn(T): bool)` | `-> bool` | |
| `.reduce(init, fn(acc, T): acc)` | `-> acc` | |
| `.sort(fn(T, T): int)` | `-> void` | in place, comparator like C's `qsort` |

`null` is a valid array value.

## 6. `Map<string, V>`

Keys are always `string` (v0.1). Constructed with `new`:

```js
let m = new Map<string, int>();
m.set("a", 1);
print(str(m.get_or("a", 0)));
```

| Method | Signature | Notes |
|---|---|---|
| `.set(k, v)` | `(string, V) -> void` | |
| `.get(k)` | `(string) -> V` | |
| `.has(k)` | `(string) -> bool` | |
| `.remove(k)` | `(string) -> void` | |
| `.get_or(k, default)` | `(string, V) -> V` | |
| `.keys()` | `-> string[]` | bucket-ordered, not insertion-ordered |
| `.values()` | `-> V[]` | bucket-ordered |
| `.is_empty()` | `-> bool` | |
| `.clear()` | `-> void` | |
| `.merge(other)` | `(Map<string, V>) -> void` | `other`'s keys win on collision |
| `.len` | field | |

`null` is a valid `Map` value.

For anything beyond a growable array or a string-keyed map, two stdlib
packages cover more specialized data structures: general-purpose containers
(deque, heap, non-string-keyed hashmap, LRU cache, bitset) in
[`vyto/coll/README.md`](../lib/vyto/coll/README.md), and index/graph
structures backed by flat `int[]`s (trie, DSU, Fenwick tree, segment tree,
Bloom filter, graph) in [`vyto/ds/README.md`](../lib/vyto/ds/README.md).

## 7. `struct` — value types

A `struct` is copied on assignment, parameter pass, and return — no refcount,
no allocation for the copy:

```js
struct Point { x: float; y: float; }

fn scaled(p: Point): Point { return Point(p.x * 2.0, p.y * 2.0); }

let p = Point(1.5, 2.0);
let r = p;
r.x = 99.0;
print("p.x still " + p.x);   // p is untouched — r was a copy
```

(`examples/02_structs.vt`) Structs take value-receiver methods (`fn` bodies
that read `this` but never let it escape by reference) and can be generic
(§9). Because a struct is a plain value, it's **not** nullable and array
`.contains()`/`.index_of()` refuse struct-element arrays.

`extern "C" { struct Foo { ... } }` declares a struct whose layout matches a
C struct exactly, for FFI use — see
[`docs/native-bindings.md`](native-bindings.md), where `vytobind` generates
these automatically from headers.

## 8. `class` — reference types

A `class` is heap-allocated and reference-counted (`src/ast.h:25` —
`TY_CLASS` is one of the reference kinds). Supports single inheritance,
`virtual`/`override`, an `init` constructor, and a deterministic `deinit`:

```js
class Shape {
    name: string;
    fn init(name: string) { this.name = name; }
    virtual fn area(): float { return 0.0; }
    deinit { print("~" + this.name); }
}

class Circle extends Shape {
    r: float;
    fn init(r: float) { super.init("circle"); this.r = r; }
    override fn area(): float { return 3.14159 * this.r * this.r; }
}

let shapes: Shape[] = [new Circle(1.0)];
let c = shapes[0] as Circle;   // checked downcast
```

(`examples/03_classes.vt`)

`super.method(...)` isn't limited to the constructor — inside any
`override`, it calls the base class's implementation directly (emitted as a
non-virtual call, so it can't recurse back into the override), letting an
override extend the base behavior instead of copying its body:

```js
override fn area(): float {
    return super.area() + extra;   // base implementation, then extend it
}
```

`super.<method>()` requires a base class that defines that method, and isn't
currently usable inside a closure — call the base method outside it instead.

**No GC, no cycle collector.** A reference cycle leaks silently — nothing
detects it. Mark any back-reference (child → parent, part → owner) `weak`,
which is a real keyword and applies **only** to class types
(`src/check.c:240`): it reads as `null` once the target is gone instead of
dangling.

```js
parent: weak Widget;
```

See [`docs/classes.md`](classes.md) §7 for the full rule and worked examples
from `vyto/ui`.

For the full picture — construction and `init` inheritance, `virtual`/
`override` rules, `builder` methods, `deinit` teardown order, checked
downcasts, duck-typed `for-in` iteration, generic classes, and
arena-allocated instances — see [`docs/classes.md`](classes.md).

## 9. `fn(...)` — function values

A first-class function type, written `fn(ParamTypes...): RetType`. Four
things satisfy it: a closure literal (`(x, y) => x > y`), a named top-level
function, a generic function (instantiated from the target's types), or a
bound method (`obj.method` — carries its receiver, so it needs no capture):

```js
let cmp: fn(int, int): bool = (x, y) => x > y;
s.get("/health", app.health);   // bound method, no arrow needed
```

**Closures capture by value.** A handler can call methods on a captured
class instance (instances are references) but can't mutate a captured
primitive and have it persist. See `examples/98_fn_values.vt` for all four
forms, and [`docs/getting-started.md`](getting-started.md) §5 for the
reference-cycle trap of storing a bound method back onto its own receiver.

`null` is a valid `fn` value.

## 10. Generics

Functions, structs, and classes can take type parameters:

```js
fn id<T>(x: T): T { return x; }

struct Pair<A, B> {
    a: A;
    b: B;
    fn first(): A { return this.a; }
}

let p = Pair<int, float>(3, 2.5);   // explicit
let q = Pair(10, 20);               // inferred
let arr: Pair<int, int>[] = [Pair(1, 2), Pair(3, 4)];
```

(`examples/28_generics.vt`) Generics are **monomorphized** at compile time —
every instantiation gets its own emitted code, generated into the module that
*declared* the generic (see the two-cache section of
[`docs/getting-started.md`](getting-started.md) §4 for what that means for
build caching). Two restrictions worth knowing before reaching for them:
**no bounds** (a type parameter can't require "implements X"), and a generic
class may only extend a **non-generic** base.

## 11. `null`

There's no separate optional/nullable wrapper type — `null` is a value of
every reference type: `string`, arrays, `Map`, `class`, `fn`, plus the FFI
pointer types `rawptr` and `cstring`. It is **not** valid for `struct`s or
any value type (`int`, `float`, `bool`, `byte`) — those have no representable
"absent" state.

## 12. Casts — `as`

The one and only conversion operator, used for: `int`↔`float` widening/
truncation, narrowing to an FFI-shaped type, `string`↔`cstring` (via the
`.cstr()`/`str()` builtins, not `as`), and checked class downcasts
(`shapes[0] as Circle` — panics if the actual instance isn't a `Circle` or
subclass). There is no implicit numeric coercion anywhere in the language;
every mismatch is a type error until you write the cast.
