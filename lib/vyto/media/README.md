# vyto/media — images, audio, video, fonts

> **Status: one module implemented, five stubs.** `vyto/media/image` works.
> `audio`, `video`, `font`, `codec` and `camera` declare their APIs and every
> entry point panics — see [Status](#status) before depending on one.

Media is where a systems language meets a pile of large C libraries, and the
grouping exists so that cost stays visible and stays *separable*. Each module
below is its own package directory, because `native/src` is compiled once per
directory for every module in it (`src/main.c:826-833`): a program that writes a
PNG must not link FFmpeg, and one that shapes Arabic text must not link
miniaudio.

There is no `media.vt` and no barrel — `vyto/media` is a namespace, not a
module, the same shape as `vyto/hw` and `vyto/mobile`. Import children by full
path.

```vyto
import { Image, imageFromRGB, encodePNG } from "vyto/media/image";

let px = [0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFFFF];
encodePNG(imageFromRGB(px, 2, 2), "out.png");
```

## Modules

| Module | What it gives you | Status | Native |
|--------|-------------------|:------:|:------:|
| `vyto/media/image` | PNG/JPEG/BMP/TGA **encode**, decode to a readable `i32[]`, `Image` | ✅ | stb_image_write, vendored |
| `vyto/media/audio` | miniaudio playback, capture, device enumeration | 🚧 stub | planned |
| `vyto/media/video` | FFmpeg demux, decode and scale to `Image` frames | 🚧 stub | planned |
| `vyto/media/font` | FreeType/HarfBuzz shaping — Arabic, Indic, ligatures | 🚧 stub | planned |
| `vyto/media/codec` | container mux/demux and stream metadata, below `video` | 🚧 stub | planned |
| `vyto/media/camera` | cross-platform capture, over `vyto/hw/camera` and its peers | 🚧 stub | planned |

## Status

A stub here is not a placeholder file. Every entry point carries its **real
signature and its documentation**, and calls a private `todo()` that panics —
the `vyto/crypto/openssl` convention (`openssl.vt:220-231`). The point is that
the shape is committed and *checkable*: the API can be reviewed, argued with,
and imported against before any C is written, and the list of what is missing
cannot silently drift from what the code does.

They panic rather than returning empty results because a caller who mistakes a
zero-length buffer for a successful decode is exactly the failure this
arrangement exists to prevent.

## `Image` is the shared currency

`vyto/media/image` defines `Image` — `i32[]` of `0xAARRGGBB`, row-major, no
stride padding — and everything else here that produces pixels produces one.
`media/video`'s decoded frames and `media/camera`'s captures are `Image`s, which
is why the pixel format was settled before those modules exist rather than after.

This matters because the tree already had **three** disagreeing pixel layouts:
`raster3d`'s `Framebuffer.color` (`i32[]`, `0xRRGGBB`, no alpha), `raster3d`'s
`Texture.pixels`, and `vyto/gfx`'s canvas `pixels()` (a `rawptr` of
`0xAARRGGBB` with stride padding measured in bytes). `Image` is a superset of
the first and drops the stride trap of the last. Converting is explicit —
`imageFromFramebuffer`, `imageFromRGB` — because Vyto has no overloading and a
silent format guess is how a red image comes out blue.

## What is deliberately not here

**Rendering.** `vyto/gfx` draws (blend2d), `vyto/ui` lays out, `vyto/raster3d`
rasterizes 3D. This namespace is codecs and device I/O: getting pixels and
samples in and out of files and hardware. `media/image` decodes *through*
`vyto/gfx` where it is available rather than vendoring a second decoder.

**`vyto/hw/camera`.** That is the Linux V4L2 device interface and it ships and
works today. `media/camera` is the cross-platform façade that would sit on it
plus AVFoundation, Media Foundation and Camera2 — a different job at a different
layer. If that distinction ever stops holding, `media/camera` should be deleted
rather than allowed to become a second way to open `/dev/video0`.

**Text layout.** `media/font` is shaping — turning a string plus a font into
positioned glyphs. Deciding where lines break and how a paragraph flows belongs
above it.
