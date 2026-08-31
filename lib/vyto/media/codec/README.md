# vyto/media/codec — containers, without decoding

What is inside a media file, and how to get the compressed packets out of it.

```vyto
import { demux_open } from "vyto/media/codec";

let d = demux_open("clip.mp4");
print(d.duration());                       // no decoder linked
for (let i in 0..d.streamCount()) { print(d.stream(i).codec); }
let pkt = d.readPacket();                  // still compressed
```

## Status: stub. Every entry point panics.

Nothing here is implemented. The signatures in `codec.vt` are real and settled
so the shape can be reviewed and imported against, and so this list is checkable
rather than a promise — the `vyto/crypto/openssl` convention.

### Implemented

Nothing yet.

### Not implemented — these still `panic`

`demux_open`, `Demuxer` (`streamCount`, `stream`, `duration`, `metadata`,
`readPacket`, `seek`), `mux_create`, `Muxer` (`addStream`, `writePacket`,
`finish`), `MediaStream`, `Packet`, `codec_available`.

### Also out of scope, and likely to stay there

Decoding and encoding. That is `vyto/media/video`, which is built on this.

## Why this is separate from `vyto/media/video`

A media file is two independent problems. The **container** (mp4, mkv, webm,
ogg) says what streams exist, how they are timed, and where each compressed
packet starts. The **codec** (h264, vp9, aac, opus) turns one such packet into
pixels or samples. FFmpeg splits these as libavformat and libavcodec, and the
split is not an implementation detail — it is the difference between reading a
file and decoding one.

Keeping them apart buys three things:

- **Remuxing without decoding.** Changing an mkv into an mp4, trimming a clip on
  keyframe boundaries, or extracting an audio track is a copy of packets from one
  container to another. Decoding to do that would cost orders of magnitude more
  time and lose quality on the re-encode.
- **Inspection without a decoder.** Duration, resolution, track list and metadata
  come from the container alone. A media library scanner wants exactly this.
- **A smaller dependency for the common case.** `native/src` compiles per package
  directory, so a program that only reads container metadata does not link the
  decoders.

> **If that layering ever collapses — if `vyto/media/video` stops going through
> this module — then this module has no reason to exist separately** and should
> be folded into it rather than maintained alongside. A second way to open an
> mp4 is worse than no second way.

## Things worth knowing before this is built

> **Timestamps are in the stream's `timeBase`, not milliseconds.** A packet's
> time in seconds is `pts * timeBase`. Containers do not agree on a unit, and
> assuming milliseconds is how audio drifts out of sync over a long file. This
> module deliberately does not convert — `vyto/media/video`, the presentation
> layer, hands out seconds.

> **`pts` and `dts` differ, and the difference is not a rounding artefact.**
> Presentation and decode order diverge whenever a codec reorders frames
> (h264 B-frames reference pictures that come later in the file). A caller that
> assumes they are equal will present frames out of order on exactly the files
> where it matters.

> **`seek` is not exact, by design.** It lands on the keyframe at or before the
> requested time. A non-keyframe cannot be decoded without the frames before it,
> so exact seeking means decoding forward — a cost the caller should choose,
> not one hidden here. `vyto/media/video.seek` is the exact one.

> **A file closed without `finish()` is unplayable** even though every packet
> reached the disk, because the trailer and index are written at the end.

## Planned backend

FFmpeg's libavformat, provisioned by a build script rather than vendored — it is
far too large and has a real build system, so it follows the blend2d and ICU
pattern rather than the stb one.
