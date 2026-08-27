# vyto/raster3d

A software 3D rasterizer in pure Vyto. Triangle fill with a depth buffer,
perspective-correct interpolation, near-plane clipping, backface culling,
flat/Gouraud shading and texture sampling.

**No native code, no GPU, no dependencies outside `vyto/geom`, `vyto/surface`
and `vyto/math`.** In particular it does *not* depend on `vyto/gfx`: the
framebuffer is a plain `i32[]` this module owns and hands to `Surface.blit()`,
so it works on every surface backend — X11, Win32, Android, the Linux
framebuffer and headless — including the ones where blend2d is absent.

This is the piece that makes `vyto/geom`'s `Mat4` and `vyto/geo`'s 3D layer
*visible*. blend2d is a 2D rasterizer with no depth buffer, so it can only do
painter's-algorithm sorting: exact for a convex body like a globe, wrong the
moment geometry interpenetrates.

## What it is for — measured, not hoped

| Target | Result |
|---|---|
| **720p full-screen, heavy geometry** | **70 fps** (65k triangles, 100% coverage) |
| **1080p, widget-sized view** | **~70 fps** |
| 1080p full-screen | 37–48 fps |

**Use it for:** 720p full-screen 3D, a 3D view inside a UI at any resolution,
data and map visualisation, CAD-style previews, anything that must run without
a GPU, and anything that needs a *reproducible* image.

**Do not use it for:** full-screen 1080p+ 3D at 60fps. That ceiling is not a
Vyto limitation — the same rasterizer hand-written in C manages only 57 fps on
the 1080p heavy case. Single-threaded scalar rasterization of two megapixels
costs what it costs. That workload wants a GPU.

Full measurements, including the C comparison, in
`local/docs/RASTER3D-SPIKE.md`.

## Quick start

```vyto
import { Framebuffer, Renderer, camera, light } from "vyto/raster3d/raster3d";
import { sphere, material_shaded, SHADE_GOURAUD } from "vyto/raster3d/mesh";
import { Vec3, mat4Identity, mat4Perspective, mat4LookAt } from "vyto/geom";

let fb = new Framebuffer(800, 600);
let r = new Renderer(fb);
r.setCamera(camera(mat4LookAt(Vec3(0.0, 0.0, 3.0), Vec3(0.0, 0.0, 0.0), Vec3(0.0, 1.0, 0.0)),
                   mat4Perspective(1.0472, 800.0 / 600.0, 0.1, 100.0)));
r.setLight(light(Vec3(0.4, 0.7, 0.6), 1.0));

r.beginFrame(0x101418);
r.draw(sphere(1.0, 48, 24), mat4Identity(), material_shaded(0x4FA3D1, SHADE_GOURAUD));
fb.blitToOrigin(surface);   // or fb.blitTo(surface, rect)
surface.present();
```

## Types

| Type | What |
|---|---|
| `Framebuffer` | `i32[]` colour + `f32[]` depth, `clear`/`blitTo`/`hash` |
| `Renderer` | camera, light, optional texture; `beginFrame` then `draw` |
| `Camera` | a view and a projection `Mat4`, both from `vyto/geom` |
| `Light` | one directional light |
| `Mesh` | struct-of-arrays geometry: positions, normals, uvs, indices |
| `Material` | colour, shading model, double-sidedness, ambient floor |
| `Texture` | `i32[]` pixels, nearest/bilinear, repeat/clamp |

`Mesh` is deliberately struct-of-arrays rather than an array of vertex objects
— the arena rule from the root `CLAUDE.md`. A 65k-triangle mesh built one
object per vertex would pay 33k allocation headers and 33k refcount lifetimes
for data that is never individually referenced.

Primitives: `sphere`, `cube`, `box`, `plane`, `cylinder`, or build your own with
`MeshBuilder`.

## Conventions

Inherited from `vyto/geom` and stated there in full: **right-handed, looking
down -z, clip depth in [-1, 1]** — the OpenGL convention, not Direct3D's.

**Triangles wind counter-clockwise seen from outside.** Backface culling drops
the rest unless the material sets `doubleSided`. Note that projection flips y,
which reverses the winding in screen space, so a front-facing triangle has a
*negative* signed screen area; the renderer accounts for this, but anyone
touching `rasterTri` should know it.

## Depth precision

The depth buffer is `f32`, not `float` (Vyto's `float` is `f64`). That is a
third less memory for a byte-identical image in every scene tested — measured
32.0 MB → 21.2 MB peak RSS on a 1080p sweep.

It is not free in precision: `f32` depth z-fights sooner than `f64` when the
far/near ratio is large. If you see z-fighting, **move the near plane out**
before anything else — near/far of 0.1/100 is far more forgiving than
0.001/10000.

## Determinism

Rendering is all scalar `f64` arithmetic with no driver involved, so
`Framebuffer.hash()` is a stable fingerprint: the same scene produces the same
32-bit value on every platform and every run. `examples/108_raster3d.vt` uses
it as a golden.

This is the property a GL backend cannot offer, and it is the reason this
module exists before one.

## Relationship to a future `vyto/gl`

`Mesh`, `Material` and `Camera` are renderer-neutral on purpose. A GPU backend
should accelerate these types rather than invent a parallel vocabulary —
`positions` is already the flat `float[]` that `glBufferData` wants, and
`Mat4.columnMajorArray()` already exists for the uniform upload.

`Material.shading` is an enum rather than a shader source string precisely so
that this module can implement everything the type can express.

## Not implemented

Alpha blending, mipmapping, shadow mapping, multiple lights, and threading.
Threading is the one with a measured case for it: the 1080p bottleneck is
per-pixel floating-point work, which parallelises cleanly by tile. It is not
here because it has not been measured, and this module's numbers are all
measured.
