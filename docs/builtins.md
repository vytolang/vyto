# Built-in functions & methods in Vyto

> **Status: experimental.** The language and standard library are still moving.
> The operations below are stable enough to build on, but names and edge-case
> behaviour can still change.

Everything you can call **without an import**. These are compiled directly by
`vytoc` rather than resolved through a module, so they cost nothing to reach for
and are available in every program, including a `--freestanding` one.

Two things follow from being built in. They are **positional-only** — named
arguments are a compile error on every function and method here — and a local
does not intercept the call: `let print = 5;` is accepted, but a later
`print("hi")` still reaches the builtin, not the local. Avoid the collision
rather than relying on either reading.

The authority for this list is `BuiltinKind` in `src/ast.h`; the signatures below
are what `src/check.c` enforces.

**Reading the signatures.** `T` is an array's element type and `V` a map's value
type — both are whatever the receiver was declared with, and the checker requires
an exact match, not a convertible one. Where a parameter is written `string` it
means a real `string`: passing an `int` is an error, except on `print` and
`panic`, which are the only two that convert for you.

## 1. Free functions

### Output and failure

| Signature | Notes |
|---|---|
| `print(s: string)` | Writes a line to stdout. Accepts anything string-convertible — `int`, `float`, `bool` — so `print(42)` needs no `str()`. |
| `panic(s: string)` → *never returns* | Same conversion. Prints `file:line: panic: <s>` to stderr and exits **101**. There is no catch. |

`panic` is the failure mode for a bug — a broken invariant, an impossible state.
A condition a caller could reasonably hit at runtime should return a sentinel
instead: `false`, `-1`, `null`, or an empty array.

### Conversion

| Signature | Notes |
|---|---|
| `str(x): string` | `x` is `int`, `float`, `bool`, or `cstring` — nothing else. |
| `bytes(n: int): byte[]` | `n` **zeroed** bytes. The sized buffer to hand to C. |
| `strbytes(b: byte[], n: int): string` | The first `n` bytes, with no `strlen` scan — so it can carry NUL bytes, unlike `str(b as cstring)`. |

```js
str(1.0)        // "1"    — %g at 6 significant digits, not shortest-round-trip
str(0.1)        // "0.1"
str(true)       // "true"
```

> **Never print a bare float into a golden file.** `str()` uses `%g` at six
> significant digits, and the freestanding path uses a hand-rolled `%g` that is
> explicitly not shortest-round-trip. Quantise to an int (`round(x * 1000)`) or
> print a bool. Every fixture in `tests/fixtures/` follows this rule.

### Files and directories

| Signature | On failure |
|---|---|
| `readfile(path: string): string` | **panics** |
| `readlines(path: string): string[]` | **panics** |
| `listdir(path: string): string[]` | **panics** |
| `isdir(path: string): bool` | `false` |
| `writefile(path: string, data: string): bool` | `false` |
| `appendfile(path: string, data: string): bool` | `false` |

> **Reads panic, writes return `false`.** That asymmetry is deliberate but it is
> easy to be caught by: `writefile("/nope/x", d)` gives you `false` to handle,
> while `readfile("/nope/x")` ends the process. Guard a read with `isdir` or
> `file_exists` (`vyto/io/file`) when the path comes from outside the program.

`readlines` drops the trailing empty line, so a file ending in `\n` yields no
final `""` entry.

> **`readlines()` allocates a string per line — a memory cost, not a speed one.**
> Measured on a 55 MB / 1M-line file: **157 MB peak RSS vs 110 MB** for
> `readfile()` plus one pass over the single buffer (+43%), while wall time is a
> wash (0.13 s vs 0.14 s — `readlines()` is marginally *faster*). Reach for
> `readfile()` when the file is large relative to memory, not for throughput.
> Rewriting a working `readlines()` loop to chase latency is wasted effort.

For anything beyond one-shot whole-file work — an owning handle, buffered I/O,
`path_join`, `file_exists`, `mkdirs` — use `vyto/io/file`. For a file too large
to hold, use `vyto/mmap`.

### Process

| Signature | Notes |
|---|---|
| `args(): string[]` | Command-line arguments, **excluding** the program name. Empty when there are none. Takes no arguments — `args(0)` is an error. |

`vyto/cli` parses these into flags, options and subcommands with a generated
`--help`; it also has the non-panicking `parse_int`/`parse_float`/`parse_bool`
that `to_int` below deliberately lacks.

### Byte ranges

| Signature | Notes |
|---|---|
| `byte_comp(a: string, alo: int, ahi: int, b: string, blo: int, bhi: int): int` | Lexicographic compare of two byte ranges. `<0`, `0`, `>0`. Half-open `[lo, hi)`, clamped at runtime rather than panicking. |

**This is the only way to order strings.** Vyto has no `<` on `string` at all —
`"abc" < "abd"` is a compile error — so a string sort has to go through this.
It compares in place, which matters inside a comparator: slicing to compare
would allocate twice per comparison.

```js
let names: string[] = ["pear", "apple", "fig"];
names.sort((a, b) => byte_comp(a, 0, a.len, b, 0, b.len));
```

### C interop

| Signature | Notes |
|---|---|
| `cthunk(f: fn(...): R): rawptr` | A C function pointer for a Vyto closure, userdata **first**. |
| `cthunk_last(f: fn(...): R): rawptr` | The same, userdata **last** — the more common C convention. |

The argument must be a closure (`TY_FN`); anything else is rejected. Its
parameters and return type must each be **C-compatible** — a numeric type,
`cstring`, or `rawptr` — and the checker names the offending parameter by
position when they are not. A `void` return is allowed. See
[native-bindings.md](native-bindings.md).

## 2. `.len` — the one built-in property

`string`, array and `map` all carry `.len` as a **field, not a call**:

```js
"héllo".len      // 6  — BYTES, not characters
[1, 2, 3].len    // 3
m.len            // number of entries
```

Writing `.len()` is an error. Everything else in this document is a method.

## 3. `int` methods

Every argument is an `int`. A `float` is a compile error, not a narrowing —
`2.0` included, since a float literal already has a definite type.

| Signature | Notes |
|---|---|
| `abs(): int` · `sign(): int` | `sign` gives `-1`, `0`, `1`. |
| `min(o: int): int` · `max(o: int): int` | |
| `clamp(lo: int, hi: int): int` | |
| `pow(e: int): int` | Integer exponentiation. A **negative** exponent gives `0`. |
| `gcd(o: int): int` | Always non-negative: `(-12).gcd(18)` is `6`, and `(0).gcd(5)` is `5`. |
| `to_float(): float` | |
| `to_string(): string` | Same as `str(i)`. |

`int` is **signed 64-bit**, and division **truncates toward zero** — `-7 / 2` is
`-3`, not `-4`. `%` takes the sign of the left operand: `-7 % 3` is `-1`.

## 4. `float` methods

Every argument is a `float`. A bare `10` is fine — an untyped numeric literal
takes the expected type — but an `int` **variable** is a compile error, with no
implicit widening:

```js
(2.0).pow(10)      // ok: the literal becomes 10.0
let n = 10;
(2.0).pow(n)       // error: expected float, got int — use n.to_float()
```

| Signature | Notes |
|---|---|
| `abs(): float` · `floor(): float` · `ceil(): float` · `round(): float` · `trunc(): float` · `sqrt(): float` | `round` goes **away from zero** at `.5`: `2.5 → 3`, `-2.5 → -3`. `sqrt` of a negative gives NaN. |
| `min(o: float): float` · `max(o: float): float` · `pow(e: float): float` | |
| `clamp(lo: float, hi: float): float` | |
| `to_int(): int` | **Truncates** toward zero: `2.7 → 2`, `-2.7 → -2`. Not rounding. |
| `is_nan(): bool` | The only way to test — `x != x` works but reads as a mistake. |

`float` is `f64` (C `double`). For trigonometry, logs and the rest of libm, use
`vyto/math`, which binds them directly.

## 5. `string` methods

Strings are **immutable UTF-8 bytes**, so every method here returns a new string
rather than mutating. **All of them count bytes, not characters** — see
[strings.md](strings.md) §1, and reach for `vyto/intl/unicode` when you need
graphemes or locale-aware case.

### Inspecting

| Signature | Notes |
|---|---|
| `is_empty(): bool` | |
| `contains(s: string): bool` · `starts_with(s: string): bool` · `ends_with(s: string): bool` | |
| `index_of(s: string): int` · `last_index_of(s: string): int` | Byte offset, or **`-1`** when absent. |
| `count(s: string): int` | **Non-overlapping**: `"aaa".count("aa")` is `1`, not `2`. |
| `char_at(i: int): string` | One byte as a string. **Panics** out of bounds. |

### Transforming

| Signature | Notes |
|---|---|
| `to_upper(): string` · `to_lower(): string` | **ASCII only.** `"café".to_upper()` is `"CAFé"`. |
| `trim(): string` · `trim_start(): string` · `trim_end(): string` | ASCII whitespace. |
| `reverse(): string` | **Reverses bytes**, so it mangles any multi-byte character. |
| `repeat(n: int): string` | `n <= 0` gives `""`. |
| `pad_start(w: int, fill: string): string` · `pad_end(w: int, fill: string): string` | Already `>= w` is returned unchanged — it never truncates. A multi-byte `fill` is cut to fit: `"a".pad_start(5, "xy")` is `"xyxya"`. |
| `replace(old: string, new: string): string` | **All** occurrences. An empty `old` is a no-op. |
| `slice(lo: int, hi: int): string` | Half-open `[lo, hi)`. **Panics** if out of range or reversed — it does not clamp. |

### Splitting

| Signature | Notes |
|---|---|
| `split(sep: string): string[]` | Keeps empty fields, including a trailing one: `"a,b,".split(",")` has **3** elements. Splitting `""` gives one empty string, and a missing separator gives the whole string. |
| `lines(): string[]` | **Drops** a trailing newline's empty field: `"a\nb\n".lines()` has **2**. `"".lines()` is empty. |

Note `split` and `lines` disagree about trailing separators, deliberately —
`lines` matches what reading a text file should mean.

### Converting

| Signature | Notes |
|---|---|
| `to_int(): int` | **Panics** on text that is not a number. |
| `to_float(): float` | **Panics** likewise. |
| `to_float_at(lo: int, hi: int): float` | Parses from the byte range in place, with no substring allocation — for a parser in a hot loop. |
| `cstr(): cstring` | A NUL-terminated view for C. Cannot represent an embedded NUL. |

> **`to_int` aborts the process on bad text.** That is wrong for anything a user
> typed. `vyto/cli`'s `parse_int` / `parse_float` / `parse_bool` return a
> success flag instead, and exist for exactly this.

Concatenation with `+` accepts any string-convertible operand, so
`"n=" + 42` works. But **`s = s + x` in a loop is O(n²)** — use
`StringBuilder` from `vyto/util/text`.

## 6. Array methods

Arrays are **references**: assigning one, or capturing it in a closure, shares
the backing store. `slice` and `concat` copy; nothing else does.

`T` below is the element type. Every `T` argument must match it exactly — a
`T[]` argument means an array of the *same* element type, so `int[]` and
`float[]` never mix.

### Access

| Signature | Notes |
|---|---|
| `first(): T` · `last(): T` | **Panic** on an empty array. |
| `nth(i: int): T` | **Panics** on a bad index. |
| `is_empty(): bool` | |
| `contains(x: T): bool` · `index_of(x: T): int` | `-1` when absent. **Not available on struct-element arrays** — padding bytes make a byte-wise compare unreliable, so this is a compile error there. |

### Mutating

| Signature | Notes |
|---|---|
| `push(x: T)` | |
| `pop(): T` | |
| `insert(i: int, x: T)` | |
| `remove_at(i: int): T` | Returns the removed element. |
| `clear()` · `reverse()` | In place. |
| `fill(x: T)` | Every existing slot; does not change the length. |
| `extend(other: T[])` | Appends in place. |
| `reserve(n: int)` | Pre-size to avoid repeated realloc in a fill loop. |

### Producing

| Signature | Notes |
|---|---|
| `concat(other: T[]): T[]` | A **new** array; the receiver is untouched. |
| `slice(lo: int, hi: int): T[]` | A **copy** of `[lo, hi)`, so writing to it does not touch the original. |
| `join(sep: string): string` | **`string[]` only** — a compile error on any other element type. |

```js
let a = [1, 2, 3];
a.reserve(1000);            // one allocation instead of ~10 reallocs
let b = a.slice(0, 2);
b.push(99);                 // a is still length 3
```

### Higher-order

All take closures, and the closure's parameter and return types must match
**exactly** — the checker compares them by identity, so a `fn(int): int` will
not pass where `fn(int): bool` is wanted.

| Signature | Notes |
|---|---|
| `map(f: fn(T): U): U[]` | `U` is the closure's return type and must not be `void`. |
| `filter(f: fn(T): bool): T[]` | |
| `reduce(init: U, f: fn(U, T): U): U` | `U` is taken from `init`. |
| `each(f: fn(T))` | The closure must return `void`. |
| `find_index(f: fn(T): bool): int` | `-1` if none matches. |
| `any(f: fn(T): bool): bool` · `all(f: fn(T): bool): bool` | |
| `sort(cmp: fn(T, T): int)` | In place. `cmp` returns `<0`, `0`, `>0`. |

> **`map` needs a typed closure.** Its result type cannot be inferred from a
> bare arrow, so assign the arrow to a typed value first. The others fix or
> derive their return type and take an inline arrow directly.

```js
let dbl: fn(int): int = (x) => x * 2;
let doubled = a.map(dbl);              // map: typed value required

let evens = a.filter((x) => x % 2 == 0);   // the rest: inline is fine
let sum   = a.reduce(0, (acc, x) => acc + x);
a.sort((x, y) => x - y);                   // comparator returns <0 / 0 / >0
```

## 7. `map` methods

A `map` is **string-keyed** — every key parameter below is a `string`, and there
is no other key type. Build one with `new Map<string, V>()`; `V` is the value
type. For keys that are not strings, use `vyto/coll`'s `HashMap`.

| Signature | Notes |
|---|---|
| `set(k: string, v: V)` | Inserts or replaces. |
| `get(k: string): V` | **Panics** when the key is absent. |
| `get_or(k: string, default: V): V` | The safe read, and usually the one you want. |
| `has(k: string): bool` | |
| `remove(k: string)` | A missing key is a no-op, not an error. |
| `keys(): string[]` | |
| `values(): V[]` | |
| `is_empty(): bool` | |
| `clear()` | |
| `merge(other: Map<string, V>)` | Copies `other`'s entries in, overwriting on collision. Must have the same value type. |

> **`get` panics on a missing key; `get_or` does not.** That is the single
> easiest mistake to make here. Use `get` only where the key's presence is an
> invariant you have already checked with `has`.

> **`keys()` and `values()` are bucket-ordered, not insertion-ordered**, and the
> order is not stable across runs or versions. Sort them, or accumulate
> order-independently, if the output is compared against a golden.

## 8. `enum` methods

Given `enum Color { RED, GREEN, BLUE }`:

| Signature | Notes |
|---|---|
| `Color.count(): int` | The variant count. Folded to a **compile-time literal** — the enum itself never reaches runtime. |
| `Color.has(name: string): bool` | Whether that name is a variant. |
| `Color.parse(name: string): Color` | The variant. **Panics** on a name that matches nothing. |
| `c.name(): string` | Called on a **value**, not the type: `Color.GREEN.name()` is `"GREEN"`. |

The first three are static — asked of the type. `.name()` is the only one asked
of a value, and `.name()` is the only method an enum value has at all.

> **`parse` panics rather than returning a sentinel** because an enum is a bare
> int with no room for a "missing" value — exactly as an out-of-range
> `as Color` does. **Test with `has()` first when the input is untrusted.**

Enums are also iterable: `for (let c in Color)` walks every variant.

## 9. Every signature, in one place

`T` is an array's element type, `V` a map's value type, `U` a closure's result.

```js
// free functions
print(s: string)                    panic(s: string)          // never returns
str(x: int|float|bool|cstring): string
bytes(n: int): byte[]               strbytes(b: byte[], n: int): string
readfile(path: string): string      readlines(path: string): string[]
listdir(path: string): string[]     isdir(path: string): bool
writefile(path: string, data: string): bool
appendfile(path: string, data: string): bool
args(): string[]
byte_comp(a: string, alo: int, ahi: int, b: string, blo: int, bhi: int): int
cthunk(f: fn(...): R): rawptr       cthunk_last(f: fn(...): R): rawptr

// property (no parentheses)
s.len: int    a.len: int    m.len: int

// int
abs(): int          sign(): int         min(o: int): int    max(o: int): int
clamp(lo: int, hi: int): int            pow(e: int): int    gcd(o: int): int
to_float(): float   to_string(): string

// float
abs(): float        floor(): float      ceil(): float       round(): float
trunc(): float      sqrt(): float       pow(e: float): float
min(o: float): float                    max(o: float): float
clamp(lo: float, hi: float): float
to_int(): int       is_nan(): bool

// string
is_empty(): bool            contains(s: string): bool
starts_with(s: string): bool            ends_with(s: string): bool
index_of(s: string): int                last_index_of(s: string): int
count(s: string): int                   char_at(i: int): string
to_upper(): string          to_lower(): string      reverse(): string
trim(): string              trim_start(): string    trim_end(): string
repeat(n: int): string      slice(lo: int, hi: int): string
pad_start(w: int, fill: string): string
pad_end(w: int, fill: string): string
replace(old: string, new: string): string
split(sep: string): string[]            lines(): string[]
to_int(): int               to_float(): float
to_float_at(lo: int, hi: int): float    cstr(): cstring

// array
first(): T          last(): T           nth(i: int): T      is_empty(): bool
contains(x: T): bool                    index_of(x: T): int
push(x: T)          pop(): T            insert(i: int, x: T)
remove_at(i: int): T                    fill(x: T)
clear()             reverse()           reserve(n: int)     extend(other: T[])
concat(other: T[]): T[]                 slice(lo: int, hi: int): T[]
join(sep: string): string               // string[] only
map(f: fn(T): U): U[]                   filter(f: fn(T): bool): T[]
reduce(init: U, f: fn(U, T): U): U      each(f: fn(T))
find_index(f: fn(T): bool): int         any(f: fn(T): bool): bool
all(f: fn(T): bool): bool               sort(cmp: fn(T, T): int)

// map — keys are always string
set(k: string, v: V)        get(k: string): V       has(k: string): bool
get_or(k: string, default: V): V        remove(k: string)
keys(): string[]            values(): V[]
is_empty(): bool            clear()     merge(other: Map<string, V>)

// enum
Color.count(): int          Color.has(name: string): bool
Color.parse(name: string): Color        c.name(): string
```

## 10. Which failures panic

Worth memorising, because the split is not obvious from the names:

| Panics | Returns a sentinel |
|---|---|
| `readfile`, `readlines`, `listdir` | `isdir` → `false` |
| `s.to_int()`, `s.to_float()` | `writefile`, `appendfile` → `false` |
| `s.char_at(i)`, `s.slice(lo, hi)` out of range | `index_of`, `last_index_of`, `find_index` → `-1` |
| `a.first()`, `a.last()`, `a.nth(i)` out of range | `m.get_or(k, d)` → the default |
| `m.get(k)` on a missing key | `m.remove(k)` on a missing key → no-op |
| `Enum.parse(name)` on an unknown name | `Enum.has(name)` → `false` |
| Any array index out of bounds | |

The rule behind it: an operation with **an obvious in-band "not found" value**
returns one, and an operation whose result type has no room for failure panics.
`index_of` can say `-1`; `char_at` has no string that means "no character".

Bounds checks are never elided — that cost is the memory-safety guarantee, and
it is measured at about 1.7× hand-written C on a fill-bound inner loop
(`local/docs/RASTER3D-SPIKE.md`).

## 11. Where to go next

| For | Use |
|---|---|
| Formatting, `StringBuilder`, `chr`, `charCount` | `vyto/util/text`, `vyto/util/fmt` |
| Counting **characters** rather than bytes | `charCount(s)` in `vyto/util/text` — `.len` is bytes |
| Parsing user input without panicking | `vyto/cli` |
| Ordering, searching, custom comparators | `vyto/util/sort` |
| Deque, heap, hash map with non-string keys, LRU | `vyto/coll` |
| Trie, graph, interval tree, bloom filter | `vyto/ds` |
| Files as handles, paths, directories | `vyto/io/file` |
| A file too large to hold in memory | `vyto/mmap` |
| trig, log, exp | `vyto/math` |
| Unicode-correct case, graphemes, collation | `vyto/intl/unicode` |
| Regular expressions | `vyto/regex` |

## Examples in the tree

Each of these is a golden test, so they are kept honest:

| Example | Covers |
|---|---|
| `examples/10_strings.vt` | string basics |
| `examples/24_string_methods.vt` | every string method |
| `examples/23_num_methods.vt` | `int` and `float` methods |
| `examples/25_array_methods.vt` | array methods |
| `examples/26_array_hof.vt` | `map`/`filter`/`reduce`/`sort` and the typed-closure rule |
| `examples/27_map_methods.vt` | `map` methods |
| `examples/110_enum.vt` | enum `count`/`parse`/`has`/`name` and iteration |
