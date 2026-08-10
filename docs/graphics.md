# Graphics in Vyto

What a Vyto program can draw with, top to bottom.

> **3D, up front:** `vyto/geom` ships a complete, tested MVP-transform math
> library — `Mat4`, `Vec3`/`Vec4`, `mat4Perspective`/`mat4LookAt`/rotations —
> but **nothing in the repo turns those matrices into rasterized 3D pixels**.
> There is no mesh type, no triangle rasterizer, no depth buffer, and no
> 3D-capable canvas method anywhere. Every actual drawing surface described
> below (`vyto/surface`, `vyto/gfx`) is exhaustively 2D. If you want a
> rotating cube on screen, the math to compute its transform exists; the
> rasterizer to paint it does not — you'd be writing one. See §6.

## 1. The stack

```
vyto/geom            math: vectors, Mat4, vector paths — no drawing surface, no window
     │
     ▼
vyto/surface          Layer 0: a window + pixel canvas + event queue (lean 2D)
     │                          ▲
     ▼                          │ blitPtr (blend2d's buffer onto the window)
vyto/gfx              rich 2D: blend2d-backed canvas — AA shapes, gradients, text, images
     │                          │
     ▼                          ▼
vyto/ui Painter       the seam: widgets draw through this abstract class only
     ├── SurfacePainter   (default, forwards to vyto/surface — lean tier)
     └── GfxPainter        (forwards to vyto/gfx — rich tier)
```

Each layer is independently usable. A game or emulator can draw straight on
`vyto/surface` and never touch `vyto/gfx` or `vyto/ui`. A custom renderer can
use `vyto/gfx`'s `Canvas` with no window at all. `vyto/ui`'s widgets never
import `vyto/gfx` directly — they draw through `Painter`, and an app wires in
whichever tier it wants at runtime.

## 2. `vyto/geom` — the math, no drawing

```vyto
import { Vec2, Vec3, Vec4, Mat4, mat4Identity, mat4Perspective } from "vyto/geom";
import { Path } from "vyto/geom/path";
```

Pure math — no window, no pixels. See `docs/math.md` for the general
`vyto/math` family; this package is specifically the 2D/3D geometry
primitives graphics and `vyto/geo` (geodesy) build on.

### Vectors

| Type | Fields | Methods |
|---|---|---|
| `Vec2` | `x, y` | `add` `sub` `scale` `neg` `dot` `len2` `len` `dist` `lerp` `normalized` `perp` |
| `Vec3` | `x, y, z` | `add` `sub` `scale` `neg` `dot` `cross` `len2` `len` `dist` `lerp` `normalized` `xy` |
| `Vec4` | `x, y, z, w` | `add` `sub` `scale` `neg` `dot` `len2` `len` `lerp` `normalized` `xyz` `homogenized` |

All are value-type structs. Degenerate input (e.g. normalizing a zero vector)
returns a zero result rather than panicking — "there is no error channel on a
value type" (`geom/README.md`). `Vec4` has no `cross` (undefined in 4D); it
doubles as a homogeneous-coordinate / RGBA carrier.

### `Mat4` — row-major 4×4

Fields `m00..m33`; `row(i)`/`col(i)` read a row/column. Methods: `add` `sub`
`scale` `mul(o)` (o applied first) `mulVec4(v)` `transformPoint(v: Vec3)`
`transformDirection(v: Vec3)` `transposed` `determinant` `isInvertible`
`inverted` (identity on singular — no error channel) `normalMatrix`
`approxEquals(o, eps)` `toArray` (row-major) `columnMajorArray` (for handing
to a column-major GPU API).

Constructors (free functions): `mat4Zero` `mat4Identity` `mat4FromRows`
`mat4FromColumns` `mat4Translation(t)` `mat4Scaling(s)`
`mat4ScalingUniform(s)` `mat4RotationX/Y/Z(rad)` `mat4RotationAxis(axis,rad)`
`mat4LookAt(eye, target, up)` `mat4Perspective(fovyRad, aspect, near, far)`
`mat4Ortho(l, r, b, t, n, f)`.

Convention: right-handed, looking down -z, clip depth in `[-1, 1]` (OpenGL,
not D3D). A full model-view-projection pipeline is `Vec4` in, perspective
divide, `Vec3` out — the math for it is complete. What consumes that math
(§6) is the open question.

**Deliberately out of scope** (`geom/README.md`): `Mat3`, quaternions,
Euler-angle conversions, camera controllers, frustum culling, scene graphs,
2D affine transforms (`vyto/gfx` has its own), polygon boolean ops/convex
hull/triangulation, spline fitting, path-stroking-to-outline. Stated reason:
these are renderer/engine concerns, and nothing needs them "until something
is drawing in 3D."

### `Path` — backend-neutral vector path

```vyto
import { Path } from "vyto/geom/path";
```

`class Path`: `moveTo(x,y)` `lineTo(x,y)` `quadTo(cx,cy,x,y)`
`cubicTo(c1x,c1y,c2x,c2y,x,y)` `close()` (all chainable) `rect(x,y,w,h)`
`flatten(): FlatPoly` (subdivides curves into 16 segments, for a renderer
with no native curve support). `class FlatPoly { xs: float[]; ys: float[]; }`
is the flattened result.

`Path` is the one structured type in the `Painter` API (§5) and is
deliberately sourced from here, not from `vyto/gfx`, so it stays
renderer-neutral — both the lean and rich tiers accept the same `Path`.

## 3. `vyto/surface` — window, pixel canvas, events (lean 2D)

```vyto
import { Surface, rgb, Rect } from "vyto/surface";
```

"Layer 0 of the bundled UI stack... a supported public layer: consume it
directly for games, custom renderers, or visualization tools, or let
`vyto/ui` sit on top of it." Backed by X11 on Linux (`native/src/vsurf.c`),
GDI on Windows.

### `Rect`

`right`/`bottom`/`cx`/`cy`/`is_empty`/`has`; transforms `inset` `inset_xy`
`outset` `offset` `move_to` `resize` `round_out`; edge slicing `take_top/
bottom/left/right` `drop_top/bottom/left/right`; layout `center_box`
`center_x` `center_y`; set ops `union` `intersect` `overlaps`.

### Colors

`rgb(r,g,b)` `rgba(r,g,b,a)` `with_alpha(color,a)` `alpha_of` `red_of`
`green_of` `blue_of` `mix(c0,c1,t)`.

### `Surface`

- **Lifecycle**: `init(title,w,h)`, `width()`, `height()`, `set_title(t)`
- **Vector draw** (integer-snapped): `fill(r,color)` `frame(r,color)`
  `line(x0,y0,x1,y1,color)` `text(x,y,s,color)` `text_in(r,pad,s,color)`
  `text_width(s)` `font_ascent()` `font_height()` — a single fixed bitmap
  font, no resizing/weight
- **Blit** (raw pixel buffers — "for emulators, pixel art, ray tracers"):
  `blit(pixels: i32[], srcw, srch, dst: Rect)` (nearest-neighbor scaled),
  `blitPtr(p: rawptr, srcw, srch, dst)`, `blitRect(p, stridePx, srcx, srcy,
  w, h, dstx, dsty)` (stride-aware sub-rect — this is what `GfxPainter` uses
  to move a blend2d canvas onto the window)
- **Present**: `present()` `present_rect(r)`
- **Clip**: `clip_set(r)` `clip_clear()`
- **Events/input**: `wait()` `poll()` `wait_timeout(ms)` `set_vsync(on): bool`
  `key()` `text_ev()` `mouse_x()` `mouse_y()` `wheel()` `mods()` `now_ms()`
  `scale()` (HiDPI factor)
- **Clipboard**: `clipboard()` `set_clipboard(t)`
- **Escape hatches**: `raw_handle()` `native_display()` `native_window()`
  `native_gc()`

Event constants `EV_NONE..EV_VSYNC` (12 kinds), key codes `KEY_SPACE..
KEY_F12`, modifier masks `MOD_SHIFT/CTRL/ALT/SUPER`.

### Example: a filled rectangle

`Surface` has no circle primitive — `fill`/`frame` are rect-only on this
tier (§4's `Canvas` has `fillCircle`).

```vyto
import { Surface, Rect, rgb } from "vyto/surface";

fn main() {
    let s = new Surface("Rectangle", 200, 200);
    s.fill(Rect(20.0, 20.0, 100.0, 60.0), rgb(220, 60, 60));
    s.present();
}
```

**Real, working example**: `apps/chip8` renders its entire CHIP-8 framebuffer
via one `Surface.blit(cpu.fb, DISP_W, DISP_H, dstRect)` call per frame — a
raw pixel array, no decoding, no gfx involved. `apps/snake` is built directly
on `Surface.fill` + events + a timer, no `vyto/ui` and no `vyto/gfx`.

## 4. `vyto/gfx` — rich 2D drawing (blend2d)

```vyto
import { Canvas } from "vyto/gfx";
```

> **Not vendored in git.** blend2d binaries are built by
> `lib/vyto/gfx/native/build-blend2d.sh`; on a fresh clone, gfx examples/tests
> skip rather than fail.

`class Canvas { handle: rawptr; ... }` — an off-screen AA raster surface you
blit onto a `Surface` window (or export standalone).

- **Lifecycle**: `init(w,h)`, `deinit`, `width()` `height()`
- **Fill/stroke shapes**: `clear(rgb)` `fillRect` `fillRoundRect`
  `fillCircle` `strokeLine` `strokeRoundRect` `strokeCircle`
  `fillPolygon(xs, ys, rgb)` `strokeArc(cx,cy,rx,ry,startDeg,sweepDeg,width,rgb)`
- **Vector paths**: `fillPath(p: Path, rgb)` `strokePath(p: Path, width, rgb)`
  — crisp AA curves straight from a `vyto/geom/path` `Path`
- **Gradients**: `linearGradientRect(x,y,w,h,x0,y0,x1,y1,rgb0,rgb1)` (2-stop),
  `linearGradientRectN(...,colors: i32[], positions: float[])` (multi-stop),
  `radialGradientRectN(x,y,w,h,cx,cy,radius,colors,positions)`
- **Effects**: `bevelRound` `shadowRound` `blurRect` `backdropBlur`
- **Clipping**: `clipPush(x,y,w,h,r)` `clipPop()`
- **Affine transforms** (2D only — see the 3D note at the top of this doc):
  `save()` `restore()` `translate(dx,dy)` `scale(sx,sy)` `rotate(deg)`
  `resetTransform()`, nested on the same state stack as clip push/pop
- **Fonts/text**: `loadFont(path,size)` `loadFontWeight(path,size,weight)`
  `loadFontBytes(data,len,size,weight)` `setWeight(w)` `setFontSize(size)`
  `fontSize()` `text(x,y,s,rgb)` `textWidth(s)` `fontAscent()` `fontHeight()`
  — embedded-asset paths shadow disk automatically (§7)
- **Images**: `drawImage(img,x,y,w,h)`, plus the free functions in §8
- **Output**: `flush()` `pixels(): rawptr` `stride(): int` — hand to
  `Surface.blitRect`
- **Testing/debug**: `hash()` (FNV fingerprint for golden tests),
  `writePPM(path)`, `pixelAt(x,y)`

Colors are packed `0xAARRGGBB`; a bare `0xRRGGBB` is fully transparent — use
`rgb()`/`rgba()` from `vyto/surface` to build one with alpha.

### Example: a filled circle, saved to disk

No window needed — a `Canvas` is an off-screen raster surface on its own;
`writePPM` is enough to see the result without wiring up a `Surface`.

```vyto
import { Canvas } from "vyto/gfx";

fn main() {
    let c = new Canvas(200, 200);
    c.clear(0xFFFFFFFF);                      // opaque white
    c.fillCircle(100.0, 100.0, 60.0, 0xFF3060DC);  // opaque blue
    c.flush();
    c.writePPM("circle.ppm");
}
```

To put the same circle in a window instead of a file, blit the canvas onto a
`Surface` after `flush()`: `surf.blitRect(canvas.pixels(), canvas.stride() /
4, 0, 0, canvas.width(), canvas.height(), 0, 0)`, then `surf.present()` — the
pattern `GfxPainter` itself uses every frame.

**Known limitation**: some PNGs fail to decode on the vendored blend2d build
(`BL_ERROR_IMAGE_UNKNOWN_FILE_FORMAT`) even though codec detection succeeds —
not simply "palette PNGs," trigger not fully isolated. Documented
regeneration recipe in `lib/vyto/gfx/native/src/gfx_shim.h`.

**Shipping cost**: measured at 1024×768, ~12.4MB RSS, ~2ms full-frame
repaint. Shared `.so` by default (~2.7MB); `--bundle` static link is
~4-5MB single file.

**`GfxPainterNet`** (`vyto/gfx/painter_net.vt`) extends the `GfxPainter`
described in §5 with `load_image_url(url)` via `vyto/net`'s `fetchUrl` — kept
in its own module so a plain gfx app doesn't link libcurl/TLS.

**Real, working examples**: `apps/gfxdemo` (radial gradient orb, a `Path`
heart via 4 `cubicTo`s, transformed/animated square), `apps/motiondemo` (13
tiled animations exercising transforms, paths, both gradient kinds, clip, AA
text), `apps/glass` (Aero/iOS-style translucency — real blur/shadow on real
pixels, not stacked translucent rects; notes blur isn't yet exposed through
`Painter`, so it draws against a raw `Canvas`).

## 5. `vyto/ui` `Painter` — the seam

```vyto
import { Painter, Window } from "vyto/ui";
```

`Painter` (`lib/vyto/ui/core.vt`) is an abstract class with ~47 virtual
methods. Widgets only ever call through `Painter` — this is what lets
`vyto/ui` itself have zero dependency on `vyto/gfx` (verified: the only
mentions of `vyto/gfx` anywhere in `lib/vyto/ui` are comments). Every rich
method has a documented degrade to a lean one in the base class (e.g.
`fill_round`→`fill`, `gradient_n`→flat mix of first/last stop,
`fill_path`→flatten-to-polygon, `load_image`→`null`,
`draw_image(null)`→gray placeholder rect) — so a widget written against the
rich vocabulary still renders, just less prettily, on the lean tier.

| Group | Virtuals |
|---|---|
| Base draw | `fill` `frame` `line` `text` `text_in` `text_width` `font_ascent` `font_height` |
| Sizing/lifecycle | `width` `height` `present` `present_rect` `on_resize` |
| Capability + degraded stroke | `can_clip` `line_w` |
| Typography | `set_weight` `push_font` `pop_font` `font_size` |
| Rich vocabulary | `fill_round` `stroke_round` `circle` `stroke_circle` `gradient_v` `shadow` `clip_push` `clip_pop` |
| Materials | `can_blur` `blur` `backdrop` |
| More vocabulary | `gradient_n` `bevel` `polygon` `stroke_arc` |
| Transforms | `save` `restore` `translate` `scale` `rotate` `can_transform` `radial_gradient` |
| Vector paths | `fill_path` `stroke_path` |
| Images | `load_image` `draw_image` `free_image` `load_image_url` |
| Concrete helpers (non-virtual) | `baseline_in` `text_center` `text_right` |

### Implementations

| Painter | Backs onto | Notes |
|---|---|---|
| `SurfacePainter` | `vyto/surface` | the default (lean tier); real geometric clipping via `Surface.clip_set` |
| `GfxPainter` | `vyto/gfx` `Canvas` | the rich tier (blend2d) |
| `GfxPainterNet` | `GfxPainter` | + network image loading |
| `AndroidPainter` | `android.graphics.Canvas` (JNI) | renders through platform Skia; requires Android NDK |
| `ClippedPainter` | wraps another `Painter` | a *decorator*, not a renderer — clips every draw call to a rect; used by `ScrollView` |

A `Window` defaults to `SurfacePainter`; swap tiers at runtime with
`win.use_painter(gfxPainter)`. No library constructs a concrete `Painter` —
only app code does, which is what keeps `vyto/ui` gfx-free.

**Real, working examples**: `apps/uigfx` (same widget tree, switch tiers by
changing one `use_painter` line), `apps/skingallery` (every widget × all 5
skins × light/dark), `apps/gallery`/`gallery2` (full widget showcase, both
tiers), `apps/charts` (line/area/bar/pie/step/histogram/scatter/gauge/
sparkline/bubble via `vyto/ui/chart`), `apps/vytopad` (multi-tab notepad,
`ClippedPainter` for scroll viewports), `apps/iphone` (iOS skin + hand-drawn
vector icons + `Avatar`/`Image` widgets), `apps/datagrid` (spreadsheet-grade
`DataTable`, both tiers). Lean-tier-only tutorial series:
`examples/14_layout.vt` through `examples/18_nav.vt`.

## 6. Fonts & text

| Tier | Mechanism |
|---|---|
| `vyto/gfx` (rich) | blend2d: proportional, AA, kerning. `Canvas.loadFont/loadFontWeight/loadFontBytes` load TTF regular/medium/bold "slots"; `GfxPainter` auto-derives `-Medium`/`-Bold` sibling filenames from a regular path and falls back to the regular face if a variant is missing, so text never blanks. `push_font`/`pop_font` nest size+weight for type scales. |
| `vyto/surface` (lean) | one fixed bitmap font baked into the X11 backend — no resizing, no weight. `SurfacePainter`'s `push_font`/`pop_font`/`set_weight` are no-ops. |
| `vyto/mobile/android` | platform `android.graphics.Paint`/Canvas — system fonts, automatic CJK/emoji fallback "for free," contrasted with blend2d needing a vendored font per script. |

**Vendored house font**: `lib/vyto/gfx/fonts.vt` — `fontPath(name)` /
`interFontPath()` resolve the bundled **Inter** typeface
(`lib/vyto/gfx/assets/Inter-{Regular,Medium,Bold}.ttf`, OFL-licensed) shipped
inside the stdlib, so an app can pin a consistent look without depending on a
system font being installed.

**In-memory fonts**: `Canvas.loadFontBytes(data,len,size,weight)` and
`GfxPainter.useFontBytes(reg,regLen,med,medLen,bold,boldLen)` load a TTF from
a byte buffer rather than a disk path. Both `Canvas.loadFont`/`loadFontWeight`
and `GfxPainter.load_weight` check the embedded-asset VFS
(`vt_vfs_ptr`/`vt_vfs_size`) first — an app built with `--with-assets` gets
the same relative-path font call resolved from the binary instead of disk,
zero code change. `lib/vyto/asset` is the general form of this VFS-or-disk
pattern for any asset, not just fonts.

## 7. Images

- **Decode**: `loadImage(path): rawptr` / `loadImageBytes(data, len)` in
  `vyto/gfx` — blend2d's built-in codecs, **PNG/JPEG/BMP/QOI**, no extra
  codec dependency. Checks the embedded-asset VFS before disk. `null` on
  failure (see the PNG limitation in §4).
- **Free/introspect**: `freeImage(img)` `imageWidth(img)` `imageHeight(img)`
- **Draw**: `Canvas.drawImage(img, x, y, w, h)` — scaled into the target rect
- **Network**: `GfxPainterNet.load_image_url(url)` bridges `vyto/net`'s
  `fetchUrl`/`fetchData`/`fetchLen` into `loadImageBytes`
- **Raw pixels, not decoded**: `Surface.blit(pixels: i32[], srcw, srch, dst)`
  nearest-neighbor scales an arbitrary `0xRRGGBB` in-memory buffer onto the
  window — this is the emulator/pixel-art path, no codec involved
- **Widget**: `Image` (`vyto/ui/image.vt`) — wraps a decoded bitmap at an
  explicit size, optional rounded-corner clip, URL-vs-local-path dispatch,
  flat gray placeholder on decode failure

## 8. 3D — what exists and what doesn't

To be unambiguous, since it's easy to assume `Mat4`/`mat4Perspective` implies
a 3D renderer:

**Exists**: the complete linear-algebra side of a model-view-projection
pipeline (§2) — `Mat4` composition, `mat4LookAt`/`mat4Perspective`/
`mat4Ortho`, `Vec3`/`Vec4` transforms, `columnMajorArray()` for handing a
matrix to a column-major GPU API. It's real, tested (`tests/fixtures/
geom_mat.vt`), and used today as the Cartesian substrate under `vyto/geo`'s
geodesy (ECEF conversion, ray/sphere intersection, slerp — see
`tests/fixtures/geo_3d.vt`).

**Does not exist**: any consumer that turns those matrices into rasterized
pixels. No mesh/vertex-buffer type, no triangle rasterizer, no depth buffer,
no 3D-capable `Canvas` or `Painter` method, no GPU binding (no OpenGL/Vulkan/
Metal/D3D shim anywhere in the repo). `vyto/gfx`'s `Canvas` transforms
(`save`/`translate`/`scale`/`rotate`) are 2D affine only.

`geom/README.md` says this outright — camera controllers, frustum culling,
and scene graphs are out of scope "until something is drawing in 3D." As of
today, nothing is. Writing a software rasterizer (or a GPU backend) on top
of the existing `Mat4`/`Vec3` math and a `Surface.blit`/`Canvas.pixels()`
target is open work, not a gap in what's documented here.
