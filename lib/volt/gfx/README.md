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

This populates (both gitignored):
- `native/src/blend2d/**.h` — headers (so the shim compiles under `-Inative/src`)
- `native/<triple>/libblend2d.so` — the prebuilt shared lib (linked + shipped
  next to the executable via `$ORIGIN` rpath)

The build uses a shared, **NO_JIT** configuration: self-contained (no asmjit),
and NO_JIT is the config that hardened W^X kiosks and iOS require anyway.

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
