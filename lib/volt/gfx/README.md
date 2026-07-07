# volt/gfx

A blend2d-backed 2D `Canvas` — anti-aliased paths, gradients, and proportional
text rasterized into a pixel buffer you hand to `Surface.blit`. The rich-tier
paint layer under `volt/ui` (the vector `surface` calls can't do AA, clipping,
gradients, or shaped text). See `docs/UI-TOOLKIT.md` for the strategy.

```
import { Surface, Rect } from "volt/surface";
import { Canvas } from "volt/gfx";

let s = new Surface("app", 1024, 768);
let cv = new Canvas(1024, 768);
cv.loadFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 15.0);
cv.linearGradientRect(0.0, 0.0, 1024.0, 768.0, 0.0, 0.0, 0.0, 768.0, 0x141821, 0x20242E);
cv.fillRoundRect(20.0, 20.0, 200.0, 80.0, 12.0, 0x3C60C0);
cv.text(32.0, 64.0, "Hello", 0xECECEC);
cv.flush();
s.blitPtr(cv.pixels(), 1024, 768, Rect(0, 0, 1024, 768));
s.present();
```

## Building the blend2d dependency

blend2d is a large C++ library and is **not** vendored in git. Build it once:

```sh
lib/volt/gfx/native/build-blend2d.sh          # linux-x64
lib/volt/gfx/native/build-blend2d.sh <triple> # other targets
```

This populates (all gitignored):
- `native/src/blend2d/**.h` — headers (so the shim compiles under `-Inative/src`)
- `native/<triple>/libblend2d.so` — shared lib (default: linked + shipped next
  to the executable via `$ORIGIN` rpath)
- `native/<triple>/libblend2d.a` (+ `.a.deps`) — static archive for `--bundle`

The build is **NO_JIT**: self-contained (no asmjit), and the config hardened
W^X kiosks and iOS require anyway.

## Shipping: shared (default) vs `--bundle`

By default a gfx app ships as the executable **plus** `libblend2d.so` next to
it. The shared lib is amortized across apps — on a device running several
blend2d UIs it's mapped once (measured: ~2.7 MB private per app, ~3.8 MB shared
code total).

For single-file distribution, `voltc build --bundle` statically links
`libblend2d.a` + its deps + the C++/GCC runtimes into one executable (no
co-located `.so`); it then depends only on base system libs (libc, libX11):

```sh
voltc build apps/uigfx/uigfx.vt --release --bundle -o app   # one file
```

| | shared (default) | `--bundle` |
|--|------------------|------------|
| Distribution | exe + `libblend2d.so` | one file |
| Per-app RAM | ~2.7 MB private, ~3.8 MB shared once | ~4–5 MB private each |
| Binary size | ~160 KB | ~2 MB |
| Best for | multi-app devices (ATM/kiosk) | single-app appliances |

## Measured footprint (1024×768)

blend2d `Canvas` → `blitPtr` → X11 window: **~12.4 MB RSS**, ~2 ms full-frame
repaint (portable pipeline; JIT is faster), ~0 % idle. Two orders of magnitude
below Electron. Numbers and the phased roadmap live in `docs/UI-TOOLKIT.md`.

## Notes

- Colors are `0xRRGGBB` (opaque); the shim adds alpha.
- `blitPtr` needs a tightly packed buffer (`stride == srcw*4`); blend2d aligns
  stride to the width, so widths that are a multiple of 4 satisfy this.
- Text shaping is blend2d's built-in (Latin + kerning); pair with HarfBuzz
  later for full complex-script/bidi.
