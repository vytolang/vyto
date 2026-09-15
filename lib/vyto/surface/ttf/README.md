# vyto/surface/ttf

Anti-aliased TrueType text for `vyto/surface` apps, in **pure Vyto**. No native
code, no `#link` of its own, no C++, nothing vendored.

```vyto
import { interFont } from "vyto/surface/ttf";

let f = interFont(15.0);                              // once, not per frame
f.drawOn(s, 12, 26, "Score: 40", 0xC8C8DC, 0x0F1118);
let w = f.width("Score: 40");                         // for centering
```

## Why

`Surface.text()` bottoms out in whatever the backend has, and none of it is
good:

| Backend | What you get |
|---|---|
| X11 | server-side core fonts (`-misc-fixed`, `9x15`, `fixed`) via `XDrawString16` — no AA, no hinting, no subpixel positioning, BMP-only |
| fbdev | `fb_font[95][8]` — 95 glyphs of printable ASCII, 1bpp, monospace |
| Win32 | GDI `TextOutA` |

The rich alternative is `vyto/gfx`, but that is blend2d: a large C++ library,
not vendored in git, needing a `NO_JIT` build step and shipping a `.so` next to
the binary via `$ORIGIN` rpath. Paying a C++ runtime and a multi-MB native
dependency to draw a score counter defeats the point of sitting on `surface`.

`vyto/raster3d` already proved the alternative — a full software 3D rasterizer
in pure Vyto at 70fps/720p with no native code. Glyph compositing is far
cheaper than that.

## The two draw forms

Surface apps come in two shapes, so there are two entry points.

**`drawOn(surface, x, y, text, rgb, bg)`** — for an app that draws only with
`fill`/`line`/`text` and owns no pixel buffer (snake, multiwindow). Rasterizes
the run into a reused scratch buffer, blends against the flat `bg`, and issues
one 1:1 `Surface.blit`.

`bg` must be the colour actually under the text — glyph edges are blended
against it, so a wrong value shows as a fringe. Name the constant and use it
for both the fill and the text:

```vyto
const C_BAR_BG: int = 987416;   // rgb(15, 17, 24)
s.fill(bar, C_BAR_BG);
f.drawOn(s, 12, 26, scoreStr, C_SCORE, C_BAR_BG);
```

**`draw(dst, stride, x, y, text, rgb)`** — for an app that owns an `i32[]`
(a raster3d `Framebuffer.color`, a chip8 screen). Composites per-pixel against
whatever is already there, so text over a 3D scene or a blitted image comes out
correct rather than boxed. This is the form planets3d and shapes3d want, since
their HUD text sits over arbitrary rendered pixels.

Each takes a clipped variant bounded to a half-open pixel box:

```vyto
f.drawOnClipped(s, x, y, text, rgb, bg, clipX, clipY, clipW, clipH);
f.drawClipped(dst, stride, x, y, text, rgb, cx, cy, cw, ch);
```

The box cuts **mid-glyph** rather than dropping a straddling glyph whole —
that is the difference between a scroll viewport whose last row crops cleanly
and one whose text pops in and out a line at a time. It is also faster, not
slower: a glyph fully outside the box is rejected before its coverage is
touched, so a long run clipped to a narrow column measured **4× faster** than
the same run unclipped (9 ms vs 38 ms over 2000 draws), while a full-width clip
costs about 5%.

This is the call a `Painter` needs, since `Surface.clip_set` cannot reach a
blit.

## Conventions

Deliberately identical to `vyto/surface`, so a call site can be swapped one
line at a time:

- **`y` is the baseline**, not the top of the cell — the X11 convention that
  `Surface.text` and the fbdev `fb_text` both follow.
- Colours are `0xRRGGBB` ints.
- `width()` counts the same advance run `draw()` lays down, so centering and
  right-alignment are exact.

## Performance

The glyph cache lives on the `Font` and persists for its lifetime. Build the
font **once**, outside the render loop — a per-frame `interFont()` re-parses a
411 KB file and re-bakes every glyph.

Measured, `--release`, 17-character string:

| | |
|---|---|
| first draw (bakes every glyph) | < 1 ms |
| 1000 cached draws | 6 ms (≈6 µs each) |

Coverage lives in one contiguous arena with doubling growth; glyphs are offsets
into it, not objects (the bulk-data rule from `CLAUDE.md`). A 95-glyph ASCII
set is six `int[]` and one `byte[]`, not 95 allocations.

## Fonts

`interFont(size)` uses `Inter-Regular.ttf`, already vendored **and tracked in
git** at `lib/vyto/gfx/assets/` — present on every fresh clone, unlike the
blend2d and ICU binaries. `stdFontPath(name)` resolves any file in that
directory, mirroring `vyto/gfx/fonts.vt` so an app can switch between the gfx
painter and this module without moving assets.

Both resolve through `libDir()`, so they need the stdlib directory at runtime.
An app that must ship as a lone binary should bundle its own `.ttf` with
`--with-assets` and use `new Font(path, size)`.

## Limits

- **Clipping is explicit.** `Surface.clip_set` is invisible here: every text
  path ends in a blit, and blit ignores the clip (`surface.vt:605-606`) while
  `Surface.text` honours it. Pass a box to `drawClipped` / `drawOnClipped`
  instead — `draw` / `drawOn` are those calls with a clip the size of the
  target.
- **Importing this still links X11 on Linux**, because `drawOn` needs
  `Surface`. Only the parse+raster half is platform-free.
- **No kerning, no hinting, no GPOS.** Advances come from `hmtx`.
- **No CFF/OpenType PostScript outlines** — `glyf` only. An `OTTO` file is
  rejected rather than mis-parsed; check `valid()`.
- **Composite glyph transforms are translation-only.** Scale and 2×2 variants
  are parsed to advance the cursor correctly, but a scaled component lands
  unscaled rather than corrupting the run. Inter uses plain offsets.
- A missing or unparseable file leaves `valid()` false and makes every draw a
  no-op — no panic.

## Scope

TrueType `glyf` outlines (simple and composite), cmap formats 4 and 12,
horizontal metrics from `hmtx`/`hhea`. Rasterized with 4× vertical
supersampling and exact horizontal span coverage.
