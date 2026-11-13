# vyto/win/d2d

A `Painter` for Windows built on Direct2D and DirectWrite. It is the rich
tier without blend2d: native text (system fonts, the user's ClearType
settings, OS font fallback for CJK and other scripts), drawn by components
that ship with Windows 7 and later. It costs about 3.5 MB more memory than
`GfxPainter`; read [Caveats](#caveats) before choosing it.

```js
import { Window } from "vyto/ui";
import { D2DPainter } from "vyto/win/d2d";

fn main() {
    let win = new Window("App", 800, 600);
    win.use_painter(new D2DPainter(win.surface(), 15.0));
    win.run();
}
```

The font size is in 96-dpi pixels. `D2DPainter` applies the display scale
itself, so do not multiply by `win.scale` as you would for `GfxPainter`.

## Picking a painter per platform

Vyto has no conditional imports, and an imported painter stays linked even if
it is never constructed. Importing both `vyto/win/d2d` and `vyto/gfx` would
put blend2d into the Windows build too. Keep one small entry file per
platform and the app itself in a shared module:

```
app/
  app.vt        // shared: builds the widget tree
  main.vt       // Linux/macOS: imports vyto/gfx/painter
  main_win.vt   // Windows: imports vyto/win/d2d
```

`apps/d2dbench` is a working example of this layout.

The package compiles on every target. Off Windows its native code is a stub,
`d2d_available()` returns false, and constructing a `D2DPainter` panics.

## What it draws

Every `Painter` operation except `load_image_url`, which `GfxPainter` leaves to
the base class too:

- shapes, strokes, linear and radial gradients, polygons, vector paths, arcs,
  bevels
- rectangular and rounded clips, transforms (`save`, `restore`, `translate`,
  `scale`, `rotate`)
- images, decoded by WIC (PNG, JPEG, BMP, GIF, TIFF, ICO); an asset embedded
  with `--with-assets` is found before the disk
- shadows, blur and frosted backdrops, ported from `vyto/gfx` so the two rich
  tiers agree

## Fonts

The default family is Segoe UI, falling back to Tahoma, then Arial.
`useFamily("name")` selects any installed family and returns false if it is
missing. Bundled font files (an app's own TTF) are not supported yet; an app
that needs its own typeface on Windows should use `GfxPainter` for now.

Families without a medium (500) face, such as Segoe UI on Windows 7, draw
medium text as regular rather than jumping to semibold.

## Performance

Measured on Windows 7 against `GfxPainter` (blend2d), November 2026:

| | D2DPainter vs GfxPainter |
|---|---|
| full widget redraw | about equal |
| text drawing | about 4× faster |
| images | about 4× faster |
| shadows (cached after first draw) | about 30× faster |
| blur, backdrop | equal (same algorithm) |
| fills and geometry (paths, polygons, arcs) | about 2× slower |

Rendering is on the CPU, into a bitmap the painter owns, then copied to the
window the same way `GfxPainter` does. That keeps `Window.repaint()`'s
partial redraw working and lets the painter run headless (`VS_HEADLESS=1`),
which is how it is tested.

## Caveats

Choose `GfxPainter` instead when memory matters more than native text. The
rest of this list is what to expect if you choose `D2DPainter`.

**It uses more memory: about 3.5 MB more than `GfxPainter`.** On Windows 7, an
open window after its first frame measured 7.3 MB private working set against
3.8 MB (640×480; the Task Manager default column). The gap does not grow with
the window: at 1280×960 it was the same. It is Direct2D's own price: on
Windows 7 its software rendering runs on WARP, Microsoft's software Direct3D
(`D3D10Warp.dll` shows up in the process), and WARP's buffers and caches live
in the process heap. DirectWrite and the WIC codecs add a little more.

Heavy frames used to add more. Direct2D queues every draw until the frame
ends and keeps its buffers at the largest batch it has seen; one frame of 2000
rounded rects left 1.7 MB behind. The painter now hands Direct2D its queue
every 256 draws, which caps that, and the shadow cache is held to a 2 MB
budget. Both caps were added after the measurement above. A leak check
(every drawing operation repeated, memory read after each round) found no
leaks: everything levels off after its first use.

**Windows 7 or later, with no fallback.** The exe imports `d2d1.dll` and
`DWrite.dll` directly, so on an older Windows it does not start at all; it
never reaches `d2d_available()`. That function only reports false off
Windows. Tested on Windows 7 SP1.

**System fonts only.** An app cannot ship its own TTF to this painter yet; use
`GfxPainter` for a bundled typeface. A font the user installed themselves
works through `useFamily`, but expect it to be slow: Inter installed that way
drew about 6× slower than Segoe UI, and measured new text about 12× slower.

**Per-monitor DPI is new and checked headless only.** Every Vyto exe declares
per-monitor awareness, and a `Window` moved to a display of another scale
rescales its theme and calls `on_scale`, which moves this painter's default
font size. That path is covered by a headless test; it has not yet been tried
on a real two-display Windows 10 machine (`dpi_demo.exe` in the Windows kit is
the manual check).

**The first launch after a reboot is slow to draw.** DirectWrite builds its
font cache on first use: the first frame took about 200–270 ms once, then
8–17 ms on later launches.

**Geometry is about 2× slower than blend2d.** Paths, polygons, arcs and
rounded fills cost about twice as much (WARP versus blend2d's rasteriser).
Text, images and cached shadows more than make up for it in a typical widget
frame.

**Rounded clips under rotation are slow.** Rounded clips copy the corner
pixels aside and blend them back (about 30 µs). That only works for clips that
stay axis-aligned; under a rotating transform the painter falls back to a
Direct2D layer, measured at about 190 µs.

**Effects read what is already drawn.** `blur` and `backdrop` soften the
pixels underneath, so draw the content behind them first.

**`load_image_url` is not supported.** The base `Painter` returns null and the
widget draws its placeholder, as with plain `GfxPainter`.

**Grayscale text on a rare fallback.** If Direct2D refuses the opaque target
the painter asks for, it falls back to one with alpha, where ClearType is not
possible. `mode()` returns `D2D_MODE_WIC_PBGRA` when that happens; Windows 7
never needed it.

## Tests

- `tests/win/d2d_props.vt`: 32 pixel-level checks, run on real Windows by
  `make test-win`. All 32 held on Windows 7 SP1.
- `tests/fixtures/d2d_stub.vt`: run by `make test`, checks the package links
  and reports itself unavailable off Windows; a second check builds it
  freestanding and asserts the stub references no external symbol.
