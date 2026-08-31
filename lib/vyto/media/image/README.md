# vyto/media/image — writing image files, and reading one back

Before this module, no Vyto program could write an image. `vyto/gfx` decodes —
blend2d has PNG/JPEG/BMP/QOI codecs built in — but has never had an encoder, and
until now had no way to read a decoded image's pixels either: the only route to
a readable buffer was drawing onto a `Canvas` and walking its stride-padded
`rawptr`.

```vyto
import { Image, encodePNG, encodePNGBytes } from "vyto/media/image";

let img = new Image(64, 64);
img.fill(0xFF102030);
img.set(0, 0, 0xFFFF0000);

encodePNG(img, "out.png");            // to disk
let bytes = encodePNGBytes(img);      // or to memory, for an HTTP body
```

```vyto
import { decodeFile } from "vyto/media/image/decode";   // separate module

let photo = decodeFile("photo.jpg");
if (photo == null) { print("not decodable in this build"); return; }
photo.get(10, 10);                    // 0xAARRGGBB
```

## Two modules, because they cost different things

| Module | Backend | In git? | Always works? |
|---|---|---|---|
| `vyto/media/image` | stb_image_write, **vendored** | yes | yes |
| `vyto/media/image/decode` | blend2d via `vyto/gfx` | **no** | only where blend2d was built |

The split is a link-cost decision, not tidiness. blend2d is a 2.5 MB C++ library
built by `lib/vyto/gfx/native/build-blend2d.sh`. If the encoder imported the
decoder, every program that merely writes a PNG would link it — and on a fresh
clone, where it was never provisioned, would fail to build.

So encoding costs one vendored header and always works. Decoding costs blend2d
and you ask for it by name. `decodeAvailable()` answers by actually decoding a
1×1 PNG held in the module, rather than checking a flag someone has to remember
to set.

That split is also what makes `tests/fixtures/image_freestanding.vt` possible:
the encoder builds under `--freestanding` (as stubs), and blend2d, being C++,
never could.

## The `Image` type

`i32[]` of **0xAARRGGBB**, row-major, `width * height`, **no stride padding**.

```vyto
new Image(w, h)      // transparent — a zero pixel has zero alpha
img.get(x, y)        // out of bounds reads 0 rather than panicking
img.set(x, y, argb)  // out of bounds is dropped
img.fill(argb)  ·  img.clone()  ·  img.isEmpty()
```

| Converter | From |
|---|---|
| `imageFromARGB(px, w, h)` | already 0xAARRGGBB — **adopts** the array, no copy |
| `imageFromRGB(px, w, h)` | 0xRRGGBB with no alpha — copies, adding opaque alpha |

## Encoding

| To a file | To a `byte[]` |
|---|---|
| `encodePNG(img, path)` | `encodePNGBytes(img)` |
| `encodeJPEG(img, path, quality)` | `encodeJPEGBytes(img, quality)` |
| `encodeBMP(img, path)` | `encodeBMPBytes(img)` |
| `encodeTGA(img, path)` | `encodeTGABytes(img)` |

File writers return `false` and memory writers return an empty `byte[]` rather
than panicking — a full disk or a missing directory is a runtime condition, not
a bug in the caller. No encoder produces zero bytes on success, so empty is
unambiguous.

`setPNGCompression(0..9)` sets the deflate level, default 8. It is a global in
stb, hence a call rather than a per-encode argument.

## Things worth knowing

> **The pixel format is a decision, not a default.** The tree already had three
> layouts that disagree: `raster3d`'s `Framebuffer.color` (`i32[]` 0xRRGGBB, no
> alpha), `raster3d`'s `Texture.pixels`, and `vyto/gfx`'s `Canvas.pixels()` (a
> `rawptr` of 0xAARRGGBB, stride-padded, stride in **bytes**). `Image` is a
> superset of the first and drops the stride trap of the last. Conversion is
> explicit because Vyto has no overloading, and a silently guessed format is how
> a red image comes out blue.

> **0xAARRGGBB is a number, not a byte order.** What those bytes look like in
> memory depends on the host. The shim repacks by shifting out of the integer
> rather than casting a pointer, so nothing here is endian-dependent — the same
> argument `vyto/hash` makes for reading a byte at a time.

> **blend2d stores premultiplied alpha; this does not.** `BL_FORMAT_PRGB32`
> holds colour already multiplied by alpha, so a half-transparent pure red is
> `0x80800000` there and `0x80FF0000` here. `gfx_image_copy_pixels` undoes it on
> the way out. Skipping that step darkens every translucent pixel toward black,
> a little more on each round trip — and it is invisible on any opaque test
> image, which is why `tests/fixtures/image_decode.vt` checks a translucent
> pixel specifically.

> **A decoded image's stride is not `width * 4`, and it can be negative.**
> blend2d documents a negative stride as meaning the buffer starts at the bottom
> row (`core/image.h:59`), so `base + y * stride` reads before the allocation for
> every row but the first. The shim steps the row pointer instead, and hands
> back tightly packed pixels so no caller ever sees a stride.

> **JPEG has no alpha.** `encodeJPEG` drops the alpha channel rather than
> compositing against a background, because there is no background colour to
> pick. An image with transparency will not survive a round trip; use PNG.

> **Compression level does not always change the file size.** Measured: a 4×3
> image is 78 bytes at level 0 and at level 8, because 48 bytes of pixel data is
> below the point where a Huffman table pays for itself. Even 64×64 moves only
> 265 → 263. The level is honoured; it is just not a size knob at these scales.

## Why stb_image_write is vendored

The repo's standing rule is fetch-don't-vendor, broken deliberately twice before
this — PCRE2 (`vyto/regex`) and micro-ecc (`vyto/crypto/ecc`) — for one reason:
**a fetched dependency makes tests silently skip on a fresh clone.** That is
tolerable for a canvas backend and not for the only way a Vyto program can write
an image file. A green run that proved nothing is worse than a red one.

stb is also the cheapest possible thing to vendor: **one public-domain header,
71 KB**, no build system, no configure step, no generated files, no C++. That is
the same profile that justified the PCRE2 exception at a twentieth of the size.

`native/refresh-stb.sh` moves it to a new upstream release and verifies the
committed tree against `native/stb.files.sha256`. It is not part of the build;
nothing calls it.

```sh
sh lib/vyto/media/image/native/refresh-stb.sh --verify
```

## Tests

- `examples/111_image.vt` — 35 checks on the encoder. Runs everywhere, since it
  never touches blend2d. Asserts on the **structure** of the output (signature,
  IHDR contents, colour type, IEND) rather than a whole-file hash, which would
  also be a hash of stb's deflate and would move on any upstream refresh for
  reasons that say nothing about this code.
- `tests/fixtures/image_decode.vt` — the round trip, gated on blend2d in
  `run_tests.sh` and reported as SKIP without it. Encodes with stb, decodes with
  blend2d, and compares **every pixel** — a stride bug shows up on some rows
  only, and one on row 3 of 5 would survive a check of the four corners.
- `tests/fixtures/image_freestanding.vt` — builds the `VT_NO_LIBC` arm. Nothing
  else compiles this shim without a libc, so that arm would otherwise rot until
  a freestanding build of an unrelated program stopped linking.
