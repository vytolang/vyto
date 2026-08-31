# vyto/media/video — decoding video and audio streams

```vyto
import { video_open } from "vyto/media/video";

let v = video_open("clip.mp4");
while (true) {
    let f = v.nextFrame();
    if (f == null) { break; }
    surf.blit(f.image.pixels, f.image.width, f.image.height, dst);
}
```

## Status: stub. Every entry point panics.

Nothing here is implemented. The signatures in `video.vt` are real and settled
so the shape can be reviewed and imported against, and so this list is checkable
rather than a promise — the `vyto/crypto/openssl` convention.

### Implemented

Nothing yet.

### Not implemented — these still `panic`

`video_open`, `VideoDecoder` (`nextFrame`, `nextAudio`, `hasAudio`, `seek`,
`setOutputSize`), `VideoFrame`, `AudioFrame`, `video_thumbnail`,
`video_available`.

### Also out of scope, and likely to stay there

Encoding video, filtering and effects, subtitle rendering, and playback timing.
Timing in particular belongs to the caller: this module hands out frames with
timestamps, and deciding when to show one is an application's event loop, not a
library's thread.

## What it produces

Decoded frames are `vyto/media/image`'s **`Image`** — `i32[]` 0xAARRGGBB,
row-major, no stride padding. That is why the pixel format was settled before
this module was written: a decoder that invented its own layout would put a
fourth incompatible pixel type in a tree that already had three.

Decoded audio is interleaved `f32[]`, the format `vyto/media/audio` consumes, so
a decoded track can be written straight to a playback stream.

## What it is built on

`vyto/media/codec`, which demuxes the container and hands over compressed
packets. This module turns packets into pixels. A caller that only needs a
file's duration and resolution should use that module directly and never link a
decoder.

## Things worth knowing before this is built

> **The colour conversion is real work, not a cast.** Video is almost
> universally YUV 4:2:0 with chroma at half resolution, in one of several colour
> spaces — BT.601 for SD, BT.709 for HD, BT.2020 for HDR. Decoding with the
> wrong matrix produces an image that looks *plausible* and has visibly wrong
> skin tones, which is the worst kind of bug: it does not crash and it does not
> look broken until compared. libswscale does this properly, and that is why it
> is a dependency rather than fifty lines of arithmetic here.

> **`nextFrame` returns presentation order, not decode order.** Codecs reorder
> internally, so the decoder buffers and reorders on the way out. That is also
> why a decoder cannot be stateless, and why this is a class rather than a
> function over packets.

> **`seek` is exact and therefore not constant-time.** It seeks to the preceding
> keyframe and decodes forward, discarding frames — which is what "seek to
> 1:23.4" has to mean. That costs up to a keyframe interval of decoding,
> typically a fraction of a second but neither free nor predictable.
> `vyto/media/codec`'s seek is the cheap, inexact one.

> **`setOutputSize` is not a convenience.** Scaling inside libswscale is
> vectorised and happens as part of the conversion that was going to run anyway.
> Decoding at full size and resampling afterwards — what a thumbnailer would
> otherwise do — costs several times more for the same output.

## Planned backend

FFmpeg's libavcodec and libswscale, provisioned by a build script rather than
vendored: far too large, with a real build system, so it follows the blend2d and
ICU pattern rather than the stb one. `video_available()` is how a caller finds
out whether this build has it.
