# vyto/media/font — text shaping

Turning a string plus a font into positioned glyphs.

```vyto
import { font_load, SCRIPT_ARABIC, DIR_RTL } from "vyto/media/font";

let f = font_load("NotoNaskhArabic.ttf", 0);
let run = f.shape("مرحبا", 16.0, SCRIPT_ARABIC, DIR_RTL, "ar");
run.glyphs[0].id;          // a glyph index, not a character
run.width;                 // what a layout actually needs
```

## Status: stub. Every entry point panics.

Nothing here is implemented. The signatures in `font.vt` are real and settled so
the shape can be reviewed and imported against, and so this list is checkable
rather than a promise — the `vyto/crypto/openssl` convention.

### Implemented

Nothing yet.

### Not implemented — these still `panic`

`font_load`, `font_load_bytes`, `Font` (`shape`, `measure`, `rasterize`,
`hasGlyph`), `GlyphRun`, `font_available`.

### Also out of scope, and likely to stay there

Line breaking (UAX #14), bidi paragraph reordering (UAX #9), justification, and
font fallback chains. Those need Unicode data that `vyto/intl` already carries
through ICU, and they operate **on** shaped runs rather than producing them.
This module's job ends at "these glyphs, at these positions, in this order".

## Why this is not a `vyto/gfx` feature

`vyto/gfx` already draws text through blend2d and does Latin well. What it does
is map each character to a glyph and advance by that glyph's width, which is the
whole story for scripts where it is the whole story.

It is not the whole story for most of the world:

- **Arabic joins.** One letter takes four different shapes depending on its
  neighbours.
- **Devanagari reorders.** A vowel written after a consonant is drawn before it.
- **Latin has ligatures and kerning pairs**, so even here the naive mapping is
  merely usually adequate.

In every case the mapping is many-to-many and depends on the surrounding run, so
it cannot be done a character at a time by anything, however careful. Shaping is
that mapping, and it is a separate concern from rasterization and from drawing.

## Things worth knowing before this is built

> **A glyph id is not a codepoint.** `Glyph.id` indexes into the font. The whole
> point of shaping is that the two do not correspond — one character can become
> several glyphs and several characters can become one.

> **`cluster` is how you get back to the text.** It is the byte offset in the
> source string that produced a glyph, and it is what maps a caret position or a
> selection onto glyphs. Several glyphs may share a cluster (one character drawn
> as a base plus marks) and several characters may share one (a ligature).
> Without it, "put the cursor after the third character" is unanswerable.

> **`lang` is not decorative.** The same script renders differently by language:
> Serbian and Russian Cyrillic italics differ, and Turkish has a dotless i.
> Passing `""` gets the font's default rather than an error.

> **Rasterization returns coverage, not colour.** 8-bit per pixel, which the
> caller multiplies by the text colour — that is what lets one rasterized glyph
> serve every colour it is drawn in.

## Planned backend

FreeType for rasterization and metrics, HarfBuzz for shaping. Both are large C
libraries with real build systems, so they would be **provisioned by a build
script** the way blend2d and ICU are, not vendored. That means this package will
degrade to unavailable on a clone that has not built them, and
`font_available()` is how a caller finds out.
