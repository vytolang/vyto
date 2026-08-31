# vyto/geom — vectors, transforms & vector paths

The geometry primitives the rest of the toolkit is built on: `Vec2`/`Vec3`/
`Vec4` for points and directions, `Mat4` for 3D transforms, and `Path` for
2D vector outlines that render on both graphics tiers.

Pure Vyto. The only dependency is `vyto/math`, which is an `extern "C"`
binding to libm.

```vyto
import { Vec2, Vec3, Vec4, Mat4, mat4Perspective, mat4LookAt,
         mat4Translation, mat4RotationY } from "vyto/geom";
import { Path } from "vyto/geom/path";

Vec2(3.0, 4.0).len()                     // 5.0
Vec3(1.0, 0.0, 0.0).cross(Vec3(0.0, 1.0, 0.0))   // (0, 0, 1)
Vec4(2.0, 4.0, 6.0, 2.0).homogenized()   // (1, 2, 3, 1) — the perspective divide

// A model-view-projection chain, written in the order the maths is.
let proj  = mat4Perspective(1.0472, 16.0 / 9.0, 0.1, 1000.0);
let view  = mat4LookAt(Vec3(0.0, 0.0, 5.0), Vec3(0.0, 0.0, 0.0), Vec3(0.0, 1.0, 0.0));
let model = mat4Translation(Vec3(0.0, 0.0, -2.0)).mul(mat4RotationY(0.4));
let mvp   = proj.mul(view).mul(model);

mvp.transformPoint(Vec3(1.0, 0.0, 0.0))  // a model point, in clip space
mvp.inverted().transformPoint(clipPt)    // and back again

let p = new Path();
p.moveTo(0.0, 0.0).cubicTo(10.0, 0.0, 20.0, 10.0, 20.0, 20.0).close();
```

## Modules

| Module | What it gives you |
|--------|-------------------|
| `vyto/geom` | `Vec2`, `Vec3`, `Vec4`, `Mat4` and the `mat4*` constructors. |
| `vyto/geom/quat` | `Quat` and the `quat*` constructors — orientation, composition, slerp. |
| `vyto/geom/path` | `Path` (move/line/quad/cubic/close), `FlatPoly`, the `PATH_*` opcodes. |

`Rect` is **not** here — it is declared in `vyto/surface` along with its methods
(`inset` / `offset` / `take_*` / `union` / …), because that is where it lives and
every widget already imports it.

## Value types vs classes

| Type | Kind | Why |
|---|---|---|
| `Vec2` / `Vec3` / `Vec4` | `struct` | Stack-allocated. Methods return new values and never allocate, so vector maths in a hot loop costs nothing the refcount has to track. |
| `Mat4` | `struct` | Sixteen floats, 128 bytes, copied rather than referenced. Building an MVP chain allocates nothing. |
| `Path` / `FlatPoly` | `class` | They hold arrays, and a Vyto struct cannot have reference fields. |

Every `Vec` degenerate case returns a defined value rather than a NaN:
`normalized()` on a zero vector returns the zero vector, and
`Vec4.homogenized()` with `w == 0` returns its input unchanged — that vector is
a direction, not a position, and dividing would produce infinities a downstream
`as int` turns into undefined behaviour in C.

---

## Vectors

### `Vec2`

`add` · `sub` · `scale` · `neg` · `dot` · `len2` · `len` · `dist` · `lerp` ·
`normalized` · `perp`

`perp()` is the 2D stand-in for a cross product: rotate 90° counter-clockwise.

### `Vec3`

`add` · `sub` · `scale` · `neg` · `dot` · **`cross`** · `len2` · `len` ·
`dist` · `lerp` · `normalized` · `xy`

3D space — orientation, surface normals, and the Cartesian side of `vyto/geo`'s
geodesy. `cross` is the operation `Vec2` has no analogue for: a vector
perpendicular to both, with length `|a||b|sin θ`.

### `Vec4`

`add` · `sub` · `scale` · `neg` · `dot` · `len2` · `len` · `lerp` ·
`normalized` · `xyz` · **`homogenized`**

The homogeneous coordinate a 4×4 transform consumes: an MVP pipeline is `Vec4`
in, perspective divide, `Vec3` out. It doubles as an RGBA carrier.

No `cross` — it is not defined in 4D.

`homogenized()` is the perspective divide; `xyz()` drops `w` afterwards to land
back in 3D.

Use `len2()` over `len()` wherever you only need to compare or threshold — it
skips the `sqrt`.

---

## `Mat4`

**Every 4×4 bug is a convention mismatch**, so the conventions are worth reading
before the function list:

| | This module |
|---|---|
| Field naming | `mRC` is **row R, column C** — ordinary mathematical subscript order. |
| Storage | **Row-major**, following from the naming: `m00, m01, m02, m03` is the first row. |
| Vectors | **Columns**: a point transforms as `v' = M · v`, so translation lives in the fourth **column** (`m03`, `m13`, `m23`). |
| Composition | `a.mul(b)` applies **b first**, then `a` — the order the maths is written in, so a chain reads `proj.mul(view).mul(model)`. |
| Handedness | Right-handed, looking down **−z**. |
| Clip depth | `[-1, 1]` — the OpenGL convention, not Direct3D's `[0, 1]`. |

> **Uploading to OpenGL.** GL wants column-major memory, so use
> `columnMajorArray()`, or pass `toArray()` with
> `glUniformMatrix4fv(..., transpose = GL_TRUE, ...)`. Getting this wrong
> transposes the world, which looks like a rotation bug and gets debugged as
> one.

### Transforming

| Method | Notes |
|---|---|
| `mulVec4(v)` | The raw homogeneous product, **no divide**. Use when `w` matters — a clip-space position, or a plane. |
| `transformPoint(v: Vec3)` | A **position**: `w = 1` in, perspective divide out. |
| `transformDirection(v: Vec3)` | A **direction**: `w = 0`, so translation drops out and no divide happens. Velocities and axes want this. |
| `mul(o)` | Matrix product; `o` is applied first. |

> **Normals are not directions.** Under a non-uniform scale, a normal
> transformed by `M` stops being perpendicular to its surface. Use
> `normalMatrix()` — which is `inverted().transposed()` — instead. For a rigid
> motion the two agree.

### Structure and inversion

`row(i)` · `col(i)` · `add` · `sub` · `scale` · `transposed` · `determinant` ·
`isInvertible` · `inverted` · `normalMatrix` · `approxEquals(o, eps)` ·
`toArray` (row-major) · `columnMajorArray` (GL)

`inverted()` is a Laplace expansion over 2×2 complementary minors. A singular
matrix has no inverse and a value type has no error channel, so it **returns
the identity** — the same choice `Vec.normalized()` makes in returning a zero
vector rather than a NaN. Call `isInvertible()` first when a wrong answer would
be worse than a loud one.

### Constructors

Free functions rather than a 16-argument literal, because nobody should have to
count commas to build a rotation.

| Function | Notes |
|---|---|
| `mat4Identity()` / `mat4Zero()` | |
| `mat4FromRows(r0..r3)` / `mat4FromColumns(c0..c3)` | Each takes four `Vec4`. |
| `mat4Translation(t: Vec3)` | Into the fourth column. |
| `mat4Scaling(s: Vec3)` / `mat4ScalingUniform(s)` | |
| `mat4RotationX/Y/Z(rad)` | Positive angles turn counter-clockwise when the axis points towards the viewer. |
| `mat4RotationAxis(axis, rad)` | Rodrigues' formula. The axis is normalized internally, so callers need not; a zero axis gives the identity. |
| `mat4LookAt(eye, target, up)` | A view matrix. `up` need only be roughly up — the true up is derived — but must not be parallel to the view direction, which returns the identity rather than a NaN. |
| `mat4Perspective(fovyRad, aspect, near, far)` | `fovyRad` is **vertical** FOV; `aspect` is width/height. The fourth row is `(0, 0, -1, 0)`: it copies −z into `w`, which is what makes the later divide shrink distant geometry. |
| `mat4Ortho(l, r, b, t, n, f)` | No divide, so parallel lines stay parallel and depth is linear. CAD, 2D overlays and shadow maps. |

---

## `vyto/geom/quat`

Orientation as a unit quaternion. A `Mat4` already represents a rotation, so
this exists for the two things a matrix is bad at: **composing** rotations
without gimbal lock, and **interpolating** between them.

```vyto
import { Quat, quatIdentity, quatFromAxisAngle, quatSlerp } from "vyto/geom/quat";

let a = quatFromAxisAngle(Vec3(0.0, 1.0, 0.0), 0.0);
let b = quatFromAxisAngle(Vec3(0.0, 1.0, 0.0), PI * 0.5);
quatSlerp(a, b, 0.5).rotate(Vec3(1.0, 0.0, 0.0));   // 45°, no matrix built
quatSlerp(a, b, 0.5).toMat4();                      // when one is wanted
```

A `struct` of four floats like `Vec4`, so animating a thousand orientations per
frame allocates nothing.

| Method | Notes |
|---|---|
| `mul(o)` | Hamilton product. `a.mul(b)` applies **b first**, same order as `Mat4.mul`. |
| `conjugate()` / `inverse()` | Equal for a unit quaternion; `conjugate` is the cheap one. `inverse` of a zero quaternion is the identity, not a NaN. |
| `rotate(v: Vec3)` | Rotates without building a matrix — about half the cost for a handful of vectors. Past that, build the matrix once. |
| `toMat4()` | The interop point. |
| `axis()` / `angle()` | Inverse of `quatFromAxisAngle`. `angle()` is in `[0, PI]` and uses `atan2`, which keeps precision at the small angles animation spends its time in. |
| `dot` · `len2` · `len` · `normalized` · `add` · `sub` · `scale` · `neg` | Linear-space operations, as on `Vec4`. |
| `approxEquals(o, eps)` | Componentwise — a test on the **representation**. |
| `approxSameRotation(o, eps)` | Whether the two are the same **rotation**, which is the usually-wanted test. |

| Constructor | Notes |
|---|---|
| `quatIdentity()` | |
| `quatFromAxisAngle(axis, rad)` | Axis normalized internally; a zero axis gives the identity. Matches `mat4RotationAxis`. |
| `quatFromEuler(yaw, pitch, roll)` | Intrinsic Z-Y-X. For converting *input*; do not store orientation this way. |
| `quatFromMat4(m)` | Shepperd's four branches — the single-formula version divides by zero at 180°. |
| `quatLookAt(forward, up)` | An **orientation**, not a view matrix. See below. |
| `quatSlerp(a, b, t)` | Constant angular velocity along the shortest arc. |
| `quatNlerp(a, b, t)` | Cheaper, same path, eased velocity. |

> **`q` and `-q` are the same rotation.** That is not a redundancy to tidy away:
> it is why `quatSlerp` has to pick a hemisphere (negating one input when the
> dot product is negative), and why comparing two quaternions componentwise is
> usually the wrong test. Without the hemisphere fix, half of all
> interpolations take the 300° path instead of the 60° one — the single most
> common slerp bug. `approxSameRotation` is the test that ignores the sign.

> **`quatLookAt` is the inverse of `mat4LookAt`.** `quatLookAt` returns an
> orientation (object-to-world: it takes −z to `forward`), which is what a
> camera or a turret *is*. `mat4LookAt` returns a view matrix, which moves the
> world into camera space. So
> `quatLookAt(dir, up).toMat4().transposed() == mat4LookAt(origin, dir, up)`.
> Feeding the orientation to a renderer as a view matrix mirrors the camera,
> which looks like a handedness bug and gets debugged as one.

> **`slerp` vs `nlerp`.** They trace the same path and differ only in velocity:
> `nlerp` eases in and out by up to a few degrees at the midpoint of a wide
> rotation. Right for blending animation poses, where the cost is paid per bone
> and the difference is invisible. Wrong for a camera sweeping a known arc,
> where the velocity change is exactly what would be seen.

---

## `vyto/geom/path`

A backend-neutral 2D vector path. Record `move`/`line`/`quad`/`cubic`/`close`
once, then render it on **either** graphics tier from the same source of truth:

- **Rich tier** — `vyto/gfx`'s `Canvas` hands the raw `cmds`/`coords` arrays
  straight to blend2d, which rasterizes true anti-aliased curves.
- **Lean tier** — X11 core and the framebuffer have no curve primitive, so
  `flatten()` subdivides every curve into 16 line segments and the base
  `Painter.fill_path` / `stroke_path` draw the resulting polygon.

That fallback lives on the `Painter` base class, so a lean backend gets curves
for free — approximated, but never missing.

```vyto
let p = new Path();
p.rect(0.0, 0.0, 100.0, 50.0);                 // a closed subpath
p.moveTo(10.0, 10.0).quadTo(50.0, 0.0, 90.0, 10.0);
```

| Member | Notes |
|---|---|
| `moveTo` / `lineTo` / `quadTo` / `cubicTo` / `close` | Each returns `this`, so calls chain. |
| `rect(x, y, w, h)` | Convenience: an axis-aligned rectangle as its own closed subpath. |
| `flatten()` | → `FlatPoly` (parallel `xs` / `ys`). |
| `cmds: i32[]` / `coords: float[]` | The raw recording, for a rasterizer that wants curves. |

Operand layout in `coords`, in command order: move/line take `x, y` · quad
takes `cx, cy, x, y` · cubic takes `c1x, c1y, c2x, c2y, x, y` · close takes
none.

`PATH_MOVE` · `PATH_LINE` · `PATH_QUAD` · `PATH_CUBIC` · `PATH_CLOSE`.

> **`flatten()` concatenates every subpath into one point list.** A caller that
> needs per-subpath fills should build separate `Path` objects — otherwise the
> gap between subpaths becomes an edge of the filled polygon.

---

## Deliberately out of scope

`Mat3` · camera controllers, frustum culling and scene graphs · 2D affine
transforms (`vyto/gfx` has its own via the blend2d shim) · polygon boolean
operations, convex hull and triangulation · spline fitting and path
stroking-to-outline · anything geographic — that is `vyto/geo`, which builds on
`Vec3` here.

These are renderer or engine concerns rather than primitives, and nothing needs
them until something is drawing in 3D.

## Tests

`tests/fixtures/geom_vec.vt`, `tests/fixtures/geom_mat.vt` and
`tests/fixtures/geom_quat.vt` — pure computation, so they run on every host.
The quaternion fixture checks itself against `Mat4` throughout, which is
evidence rather than a tautology because `geom_mat.vt` tests `Mat4`
independently.

Golden files print **ints and bools only, never a bare float**: the runtime
prints `%g` at six significant digits and the freestanding path uses a
hand-rolled `%g` that is explicitly not shortest-round-trip.

The `Mat4` fixture is built around properties rather than transcribed numbers,
since a hand-written cofactor expansion is exactly the kind of code that looks
right and is not: `M · M⁻¹ = I` is checked against translation, scale, rotation,
a composed matrix, a dense general matrix, perspective, ortho and lookAt; a
rotation's inverse must equal its transpose; `(AB)ᵀ = BᵀAᵀ`; and rotations must
preserve length and determinant 1.

`normalMatrix` is tested by first asserting the **naive** path is broken — that
a normal transformed by a non-uniform scale really does stop being
perpendicular — and only then that the normal matrix fixes it. A test that
checked only the correct path would pass against a wrong implementation.
