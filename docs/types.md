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
(§11). Because a struct is a plain value, it's **not** nullable and array
`.contains()`/`.index_of()` refuse struct-element arrays.

`extern "C" { struct Foo { ... } }` declares a struct whose layout matches a
C struct exactly, for FFI use — see
[`docs/native-bindings.md`](native-bindings.md), where `vytobind` generates
these automatically from headers.

## 8. `enum` — a closed set of named values

An enum is a **distinct type that is an int at runtime** — one machine word, no
allocation, no reference counting. Variants are always reached through the enum
name, so they cannot collide with anything:

```js
enum Color { Red, Green, Blue }        // ordinals 0, 1, 2

let c: Color = Color.Green;
```

The point of it is **exhaustiveness**. A `switch` over an enum must handle every
variant, or say `default`:

```js
fn describe(c: Color): string {
    switch (c) {
        case Color.Red:   { return "red"; }
        case Color.Green: { return "green"; }
        case Color.Blue:  { return "blue"; }
    }
}
```

Miss one and it is a compile error naming what you missed, so **adding a variant
later breaks every switch that forgot it** — which a group of `const int` tags
can never do. An exhaustive switch is also *total*, so the function above needs
no unreachable `default` just to satisfy the return checker.

Values can be pinned, which is what makes an enum usable for a wire protocol or
a C ABI. An unvalued variant continues from the previous one + 1, C-style:

```js
enum PgMsg { Auth = 82, BackendKey = 75, Ready = 90 }
enum Mixed { A, B = 10, C, D = 3 }      // 0, 10, 11, 3
```

Two variants may not share a value, and duplicate names are rejected.

An enum is its own type, not an int and not another enum:

```js
let n: int = Color.Red;        // error: expected int, got Color
let x = Color.Red + 1;         // error: arithmetic on an enum
let b = Color.Red == Level.Low; // error: cannot compare Color and Level
```

`==` and `!=` work between values of the *same* enum. Ordering (`<`, `>`) does
not — an enum is a set, not a range. Arithmetic is rejected outright rather than
half-supported, because bitflags are a separate feature.

To cross into or out of the integer world, write the cast:

```js
gfx_set_mode(mode as i32);          // out to FFI
let name = names[Color.Green as int];  // out, to index an array
let k = raw as Color;               // in — checked in debug builds
```

The inbound direction is **range-checked in debug builds** and panics with the
offending value if it names no variant, because such a value typically came from
C, a file, or the network. In `--release` the check compiles away entirely.

Four accessors come with every enum:

```js
print(Color.Blue.name());     // "Blue"  — the variant's own spelling
print(str(Color.count()));    // 3       — how many variants there are
print(str(Color.has("Red"))); // true    — does that name a variant
let c = Color.parse("Red");   // Color.Red — the inverse of .name()
```

`count()` is a compile-time literal; the enum type does not exist at runtime at
all. `.name()` and the `parse()`/`has()` lookup are each written **once per
enum, and only if something asks for them** — an enum nobody names or parses
costs nothing in the binary. The strings `.name()` returns are immortal, so
calling it in a loop allocates nothing.

`.name()` is called on a *value*; `count()`, `parse()` and `has()` on the
*type*. Asking the wrong one which is which gives an error saying so.

### `parse()` panics; `has()` asks

`parse()` returns the enum itself, not an int, so a parsed value is exactly as
type-safe as a written one. That is also why a name matching nothing **panics**
rather than returning a miss: an enum is one machine word with every bit-pattern
spoken for, so there is no in-band value left to mean "absent", and handing back
a real variant would be inventing data.

Unlike the `as Color` range check above, this panic fires in `--release` too.

So `parse()` alone is right when the string is yours — a name you wrote, or one
`.name()` produced. When it came from a file, a user, or the network, ask first:

```js
if (Color.has(s)) {
    let c = Color.parse(s);
    ...
} else {
    // your own fallback: a default, an error, a skipped record
}
```

Both calls share one emitted lookup, so testing before parsing costs a second
scan of a handful of names, not a second table.

Matching is exact and byte-for-byte: no case folding, no trimming. `"red"` does
not parse as `Color.Red`. Normalize before the call if you want it looser.

### Variants can carry their own spelling

A wire format rarely spells things the way an identifier can. Give a variant a
string and `.name()`/`parse()` use it instead of the identifier:

```js
enum Header { ContentType = "content-type", XReal = "x-real-ip" }

print(Header.ContentType.name());          // "content-type"
print(str(Header.parse("x-real-ip") as int));  // 1
print(str(Header.has("XReal")));           // false — the string is the name now
```

The string is **metadata, not the value**. Ordinals stay positional, exactly as
in an untagged enum, and `as int` still gives you the ordinal. What the string
pins is the *spelling*, which is the thing that actually crosses the wire — so
unlike ordinals, a string-tagged enum survives having its variants reordered.

Two rules keep the serialization unambiguous, both compile errors:

- **All or nothing.** Tag every variant or none. A half-tagged enum would
  serialize some variants by string and the rest by identifier.
- **No duplicate strings**, for the same reason two variants cannot share an
  ordinal — `parse()` could not choose, and `.name()` would lose information.

A variant takes a string or a number, never both: they answer different
questions, and mixing them in one enum is rejected.

**Ordinals are not a wire format.** Reordering variants renumbers the ones that
follow. If a number has to survive across a version boundary, pin it explicitly
as `PgMsg` does above.

An enum field on a class is zero-initialized like every other value type, so it
defaults to whichever variant has value 0 — declare a sensible one first.

## 9. `class` — reference types

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

See [`docs/classes.md`](classes.md) §8 for the full rule and worked examples
from `vyto/ui`.

For the full picture — construction and `init` inheritance, `virtual`/
`override` rules, `builder` methods, `deinit` teardown order, checked
downcasts, duck-typed `for-in` iteration, generic classes, and
arena-allocated instances — see [`docs/classes.md`](classes.md).

## 10. `fn(...)` — function values

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

## 11. Generics

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

## 12. `null` and `T?`

`null` is a value of every reference type: `string`, arrays, `Map`, `class`,
`fn`, plus the FFI pointer types `rawptr` and `cstring`. It is **not** valid for
`struct`s or any value type (`int`, `float`, `bool`, `byte`) — those have no
representable "absent" state.

### `T?` — a reference that may be null

A `?` suffix marks a type whose value may be null, and **the checker refuses to
dereference one until you have tested it**:

```js
class Session {
    cert: Cert?;                     // may be null — say so in the type
    fn init() { this.cert = null; }
}

fn subject(s: Session): string {
    return s.cert.subject();         // error: 's.cert' is declared 'Cert?' and
                                     // may be null here
}
```

The fix is any ordinary check, which narrows the path for the rest of the
branch:

```js
if (s.cert != null) { return s.cert.subject(); }
```

This is the same machinery `weak` uses, so the narrowing shapes are identical —
`if (p != null)`, an exiting `if (p == null) { return; }`, the right side of
`&&`, a `while`, or binding a local and checking that. Writing to the path drops
what was known about it. All five shapes, and the one deliberate hole (a
narrowing survives a call), are documented once in
[`docs/memory.md`](memory.md) §3; everything there applies verbatim to `T?`.

The suffix binds tightly, so it composes without ambiguity:

```js
a: Widget?[];              // an array (never null) of nullable Widget
b: Widget[]?;              // a nullable array of non-null Widget
c: Map<string, Widget?>;   // nullable map values
d: fn(): Widget?;          // a function returning a nullable Widget
```

`T?` also works as a generic type argument — `Box<Widget?>`, and the explicit
call form `ident<Widget?>(null)`.

### Where `T?` does not apply

**Only reference types can be null**, so `?` on anything else is refused rather
than silently accepted as a no-op:

```js
count: int?;               // error: 'int?' is not a nullable type
opts: Options?;            // error, if Options is a struct
c: Color?;                 // error, if Color is an enum
p: rawptr?;                // error — see below
```

**`weak T` is already nullable**, so `weak T?` is refused as redundant — a weak
slot is *designed* to read null once its target is gone, which is the whole
point of it. Write `weak Widget`.

**The FFI pointer types stay outside the model.** `rawptr` and `cstring` accept
`null` and always will, but they cannot be written `rawptr?`. C returns NULL
constantly and the checker cannot see into C, so annotating the boundary would
cost every `extern "C"` signature a `?` and buy no safety. Check them by hand,
as you would in C.

### What is not enforced yet

**A plain `T` is not currently a promise of non-null.** These all still compile:

```js
let w: Widget = null;      // null into a plain T
this.plain = this.maybe;   // a T? assigned into a plain T
take(this.maybe);          // a T? passed to a plain T parameter
```

So `T?` today buys you a **checked dereference** at the sites that declare it,
not a guarantee that everything else is non-null. Making a plain `T` mean
non-null is a larger change — it needs assignability rules and definite
assignment in `init`, and it reclassifies roughly 160 `return null;` sites in
the standard library. Until then, read `T?` as documentation that the checker
happens to enforce at the point of use.

Debug builds carry a runtime backstop for the gap: dereferencing a null
reference panics with a file and line rather than segfaulting. It compiles out
under `--release`, exactly like the overflow and bounds checks (§1, §5).

## 13. Casts — `as`

The one and only conversion operator, used for: `int`↔`float` widening/
truncation, narrowing to an FFI-shaped type, `string`↔`cstring` (via the
`.cstr()`/`str()` builtins, not `as`), and checked class downcasts
(`shapes[0] as Circle` — panics if the actual instance isn't a `Circle` or
subclass). There is no implicit numeric coercion anywhere in the language;
every mismatch is a type error until you write the cast.
