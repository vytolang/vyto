# Math in Vyto

Everything under `vyto/math`, in three sections by what a program typically
reaches for first: **basic** (`vyto/math` itself — constants, libm, integer
helpers), **intermediate** (`vyto/math/stats`, `vyto/math/random` — derived
statistics and seeded randomness), **advanced** (`vyto/math/algebra` —
matrices and linear systems).

This is the `vyto/math` family only. 2D/3D vector/matrix types for graphics
(`Vec2`/`Vec3`/`Vec4`/`Mat4`) are a separate package, `vyto/geom` — see
[`docs/graphics.md`](graphics.md) for those, and for what Vyto can and can't
currently draw with them.

## 0. Converting between `int` and `float`

Vyto never auto-coerces between `int` and `float` — every conversion is an
explicit `as` cast, the same rule the FFI table in the top-level `CLAUDE.md`
documents for narrowing to `i32`/`i64`. Every function signature in this
document is exact about which type it wants; mixing them without a cast is a
type error, not a silent conversion.

```vyto
let n: int = 7;
let x: float = n as float;      // 7.0 — widening, always exact

let f: float = 3.9;
let i: int = f as int;          // 3 — truncates toward zero, like C

let g: float = -3.9;
let j: int = g as int;          // -3 — toward zero, not floor: floor(-3.9) is -4
```

For the away-from-toward-zero distinction, `vyto/math`'s `floor`/`ceil`/
`round`/`trunc` (§1) operate on `float` and return `float` — round or floor
*before* casting to `int` if truncation-toward-zero isn't what you want:

```vyto
(round(-3.9) as int)   // -4
(-3.9 as int)          // -3 — different answer, same input
```

**`int` → `float` is not always exact.** Vyto `int` is 64-bit; `float` is
`f64`, a 53-bit mantissa. An `int` whose magnitude exceeds `2^53`
(9007199254740992) loses precision on the cast to `float` — and casting back
to `int` does not recover the original value:

```vyto
let big: int = 9007199254740993;         // 2^53 + 1
(big as float) as int                    // 9007199254740992 — the +1 is gone
```

This matters for anything in this document that accepts an `int[]` and
widens to `float[]` internally (`vyto/math/stats`'s `toFloatArray`, `meanInt`,
etc., §2) — fine for counts/indices/typical data, a real risk for ids or
sums in the trillions.

There is no `int`-returning inverse of `iabs`/`imin`/`imax`/`iclamp` (§1) that
takes a `float` — cast to `int` first, or use the `float`-native libm
functions (`fabs`/`fmin`/`fmax`) and cast the result.

## 1. Basic — `vyto/math`

```vyto
import { sqrt, pow, sin, PI } from "vyto/math";
```

Vyto `float` is `f64` (C `double`); every function here is the C `math.h`
entry point of the same name, so it carries exactly C's semantics and
precision. libm is part of libSystem on macOS and the CRT on Windows; the
explicit link (`#link "m" if "linux"`) is only needed on Linux.

### Constants

| Name | Value |
|---|---|
| `PI` | 3.141592653589793 |
| `TAU` | 6.283185307179586 |
| `E` | 2.718281828459045 |
| `SQRT2` | 1.4142135623730951 |

### Powers & roots

| Function | Signature |
|---|---|
| `pow(x, y)` | `(float, float): float` |
| `sqrt(x)` | `(float): float` |
| `cbrt(x)` | `(float): float` |
| `hypot(x, y)` | `(float, float): float` — `sqrt(x*x + y*y)` without intermediate overflow |

### Exponential & logarithmic

| Function | Signature | Notes |
|---|---|---|
| `exp(x)` | `(float): float` | |
| `exp2(x)` | `(float): float` | 2^x |
| `log(x)` | `(float): float` | natural log |
| `log2(x)` | `(float): float` | |
| `log10(x)` | `(float): float` | |

### Trigonometric

| Function | Signature |
|---|---|
| `sin(x)` / `cos(x)` / `tan(x)` | `(float): float` |
| `asin(x)` / `acos(x)` / `atan(x)` | `(float): float` |
| `atan2(y, x)` | `(float, float): float` |

### Hyperbolic

| Function | Signature |
|---|---|
| `sinh(x)` / `cosh(x)` / `tanh(x)` | `(float): float` |

### Rounding & remainder

| Function | Signature | Notes |
|---|---|---|
| `floor(x)` / `ceil(x)` / `round(x)` / `trunc(x)` | `(float): float` | |
| `fmod(x, y)` | `(float, float): float` | C-style remainder (sign follows `x`) |

### Sign & extrema (float)

| Function | Signature |
|---|---|
| `fabs(x)` | `(float): float` |
| `fmin(x, y)` / `fmax(x, y)` | `(float, float): float` |

### Integer helpers

libm is float-only; these fill the gap for `int`.

| Function | Signature |
|---|---|
| `iabs(x)` | `(int): int` |
| `imin(a, b)` / `imax(a, b)` | `(int, int): int` |
| `iclamp(x, lo, hi)` | `(int, int, int): int` |

## 2. Intermediate — `vyto/math/stats`, `vyto/math/random`, `vyto/math/securerandom`

### Descriptive statistics — `vyto/math/stats`

```vyto
import { mean, stddev, correlation } from "vyto/math/stats";
```

Every function operates on `float[]` unless noted, and **panics on invalid
input** — an empty array, mismatched lengths, correlation against a constant
series — rather than returning a sentinel a caller might not check. The two
exceptions are `cumsum` and `normalize`, which are array transforms rather
than statistics, and return a documented default on their one degenerate
input instead of panicking.

| Function | Signature | Notes |
|---|---|---|
| `sum(xs)` | `(float[]): float` | |
| `mean(xs)` | `(float[]): float` | panics on empty |
| `variance(xs)` | `(float[]): float` | population (÷n) |
| `varianceSample(xs)` | `(float[]): float` | sample, Bessel-corrected (÷n-1); needs ≥2 elements |
| `stddev(xs)` / `stddevSample(xs)` | `(float[]): float` | `sqrt` of the corresponding variance |
| `min(xs)` / `max(xs)` | `(float[]): float` | panics on empty |
| `range(xs)` | `(float[]): float` | `max - min` |
| `sorted(xs)` | `(float[]): float[]` | new copy, never mutates `xs` |
| `median(xs)` | `(float[]): float` | `percentile(xs, 0.5)` |
| `percentile(xs, p)` | `(float[], float): float` | `p` in 0..1 (clamped), linear interpolation between ranks |
| `mode(xs)` | `(float[]): float` | ties broken by sorted-first (smaller value wins) |
| `covariance(xs, ys)` | `(float[], float[]): float` | population; panics on length mismatch |
| `correlation(xs, ys)` | `(float[], float[]): float` | Pearson r; panics if either series has zero variance |
| `zscore(xs, x)` | `(float[], float): float` | panics on zero stddev |
| `cumsum(xs)` | `(float[]): float[]` | running total, new array; empty in → empty out, no panic |
| `normalize(xs)` | `(float[]): float[]` | min-max scale to [0,1]; all-`0.0` if every element is equal, no panic |
| `summarize(xs)` | `(float[]): Summary` | one Welford pass computing n/mean/min/max/variance/stddev together — cheaper than calling several of the above separately |
| `sumInt(xs)` / `minInt(xs)` / `maxInt(xs)` | `(int[]): int` | |
| `meanInt(xs)` | `(int[]): float` | |
| `toFloatArray(xs)` | `(int[]): float[]` | escape hatch into the rest of this module |

`Summary` fields: `n: int`, `mean: float`, `min: float`, `max: float`,
`variance: float` (population), `stddev: float` (population).

### Random numbers — `vyto/math/random`

```vyto
import { rng, shuffle, choice } from "vyto/math/random";

let r = rng(42);
let x = r.nextRange(0, 100);
shuffle(deck, r);
```

`Rng` is a seeded xorshift64 generator — the same generator `vyto/ds/skiplist`
uses internally, generalized into a public API. Seeding is **required**;
there is no hidden global instance and no wall-clock auto-seed. Two `Rng`
values built from the same seed produce the exact same sequence — this module
is for reproducible sampling/simulation, **not for anything
security-sensitive** (session tokens, keys — see `vyto/math/securerandom`
below instead).

| Member | Signature | Notes |
|---|---|---|
| `rng(seed)` | `(u64): Rng` | factory |
| `Rng.reseed(seed)` | `(u64)` | restart the stream in place |
| `Rng.nextU64()` | `(): u64` | the raw generator word |
| `Rng.nextInt()` | `(): int` | uniform in [0, 2^63) |
| `Rng.nextBelow(bound)` | `(int): int` | uniform in [0, bound); rejection-sampled so no value is biased low; panics if `bound <= 0` |
| `Rng.nextRange(lo, hi)` | `(int, int): int` | uniform in [lo, hi); panics if `hi <= lo` |
| `Rng.nextFloat()` | `(): float` | uniform in [0.0, 1.0) |
| `Rng.nextFloatRange(lo, hi)` | `(float, float): float` | |
| `Rng.nextBool(p)` | `(float): bool` | true with probability `p` |
| `Rng.nextGaussian()` | `(): float` | standard normal (mean 0, stddev 1), via Box-Muller |
| `shuffle(arr, r)` | `<T>(T[], Rng)` | Fisher-Yates, in place |
| `choice(arr, r)` | `<T>(T[], Rng): T` | uniform pick; panics on empty array |

### Secure random numbers — `vyto/math/securerandom`

```vyto
import { secureBytes, secureBelow, secureToken } from "vyto/math/securerandom";

let key = secureBytes(32);
let otp = secureBelow(1000000);
let sessionId = secureToken(16);   // 32-char lowercase hex
```

Real OS entropy (`getrandom(2)` on Linux, `BCryptGenRandom` on Windows,
`/dev/urandom` otherwise), not a reproducible stream — the opposite of `Rng`
above on every axis: no seed, not reproducible, safe for anything where
predictability is a vulnerability. Free functions only — there's no `Rng`-
style object here, since OS entropy has no stream to seed or reseed; every
call reaches the OS fresh.

Implemented as a thin re-export over `vyto/util/uuid`'s existing entropy
shim rather than a second copy of the same OS-entropy ladder — duplicating
security-critical code is a real drift risk. Importing this module (not
plain `vyto/math`) is what pulls the native shim and its Windows bcrypt link
in; every other file in this package stays pure Vyto.

| Function | Signature | Notes |
|---|---|---|
| `secureBytes(n)` | `(int): byte[]` | `n <= 0` yields an empty buffer |
| `secureInt()` | `(): int` | uniform in [0, 2^63) |
| `secureBelow(bound)` | `(int): int` | uniform in [0, bound); rejection-sampled; panics if `bound <= 0` |
| `secureRange(lo, hi)` | `(int, int): int` | uniform in [lo, hi); panics if `hi <= lo` |
| `secureFloat()` | `(): float` | uniform in [0.0, 1.0) |
| `secureBool(p)` | `(float): bool` | true with probability `p` |
| `secureShuffle(arr)` | `<T>(T[])` | Fisher-Yates, in place |
| `secureChoice(arr)` | `<T>(T[]): T` | uniform pick; panics on empty array |
| `secureToken(n)` | `(int): string` | `n` random bytes, hex-encoded to a `2*n`-char string — session ids, API keys, CSRF tokens |

## 3. Advanced — `vyto/math/algebra`

```vyto
import { matrixIdentity, matrixFromRows, vdot } from "vyto/math/algebra";

let a = matrixFromRows([[2.0, 1.0], [1.0, 3.0]]);
let x = a.solve([3.0, 5.0]);
```

General dense N×M matrices and arbitrary-length vectors — the general-shape
counterpart to `vyto/geom`'s fixed-size `Vec2`/`Vec3`/`Vec4`/`Mat4` (see
[`docs/graphics.md`](graphics.md) for those). No overlap with `vyto/geom`: no
3D cross product, nothing that package already covers.

`Matrix` is a class (it holds an array field), which gives it a real `panic`
channel: `solve()`/`inverted()` panic on a singular or non-square system,
unlike `vyto/geom`'s value types, which silently fall back on degenerate
input. `determinant()` is the one exception even here — `0.0` for a singular
matrix is the mathematically correct answer, not an error, so it returns
rather than panics.

### `Matrix`

| Member | Signature | Notes |
|---|---|---|
| `new Matrix(rows, cols)` | `(int, int)` | panics if `rows <= 0` or `cols <= 0`; zero-filled |
| `.rows` / `.cols` | `int` | |
| `.data` | `float[]` | row-major backing array, `data[r*cols+c]` |
| `.get(r, c)` / `.set(r, c, v)` | | |
| `.row(i)` / `.col(i)` | `(int): float[]` | |
| `.isSquare()` | `(): bool` | |
| `.add(o)` / `.sub(o)` | `(Matrix): Matrix` | panics on shape mismatch |
| `.scale(s)` | `(float): Matrix` | |
| `.mul(o)` | `(Matrix): Matrix` | panics unless `this.cols == o.rows` |
| `.transposed()` | `(): Matrix` | |
| `.trace()` | `(): float` | panics if not square |
| `.determinant()` | `(): float` | LU decomposition, partial pivoting; panics if not square; `0.0` on singular |
| `.inverted()` | `(): Matrix` | Gauss-Jordan elimination; panics if not square or singular |
| `.solve(b)` | `(float[]): float[]` | solves `Ax = b`, Gaussian elimination with partial pivoting; panics if not square, singular, or `b.len != rows` |
| `.approxEquals(o, eps)` | `(Matrix, float): bool` | |
| `.toArray()` | `(): float[]` | row-major copy |
| `.clone()` | `(): Matrix` | |

### Matrix constructors

| Function | Signature |
|---|---|
| `matrixIdentity(n)` | `(int): Matrix` |
| `matrixZeros(rows, cols)` | `(int, int): Matrix` |
| `matrixFromRows(rows)` | `(float[][]): Matrix` — panics on a ragged input |

### Vectors (plain `float[]`)

No wrapper type — a 1D array needs none; `Matrix` is the only shape here that
does.

| Function | Signature | Notes |
|---|---|---|
| `vdot(a, b)` | `(float[], float[]): float` | panics on length mismatch |
| `vadd(a, b)` / `vsub(a, b)` | `(float[], float[]): float[]` | panics on length mismatch |
| `vscale(a, s)` | `(float[], float): float[]` | |
| `vnorm(a)` | `(float[]): float` | Euclidean (L2) |
| `vnormalized(a)` | `(float[]): float[]` | panics on a zero vector — deliberately, unlike `vyto/geom`'s `.normalized()`, which returns a zero vector instead |

**Deliberately out of scope:** eigenvalues/eigenvectors, SVD/QR
decomposition, sparse matrices. General dense N×M only.
