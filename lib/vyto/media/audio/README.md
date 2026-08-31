# vyto/media/audio — playback, capture, device enumeration

```vyto
import { audio_open_playback } from "vyto/media/audio";

let p = audio_open_playback(48000, 2);
p.start();
p.write(samples);                    // f32 interleaved, -1..1
```

## Status: stub. Every entry point panics.

Nothing here is implemented. The signatures in `audio.vt` are real and settled
so the shape can be reviewed and imported against, and so this list is checkable
rather than a promise — the `vyto/crypto/openssl` convention.

### Implemented

Nothing yet.

### Not implemented — these still `panic`

`audio_devices`, `AudioDevice`, `audio_open_playback`,
`audio_open_playback_device`, `AudioPlayback` (`start`, `stop`, `write`,
`writable`, `underruns`, `framesPlayed`), `audio_open_capture`,
`audio_open_capture_device`, `AudioCapture` (`start`, `stop`, `read`,
`readable`, `overruns`), `audio_available`.

They panic rather than returning empty results on purpose: a caller who mistakes
a zero-length read for silence would ship a program that appears to work and
records nothing.

### Also out of scope, and likely to stay there

Decoding compressed audio (that is `vyto/media/codec` plus `vyto/media/video`'s
decoder), mixing and effects, MIDI, spatial audio, and any DSP beyond format
conversion. This module moves samples between a program and a device.

## The constraint that shapes the API

**An audio callback runs on a thread the OS owns**, at real-time priority, with
a deadline of a few milliseconds. Vyto's memory model is single-address-space
reference counting with **no atomics on the refcount**, so a foreign thread may
touch C memory only — it must never retain or release a Vyto object.

That rules out the API every audio library reaches for first: a Vyto closure
invoked from the callback. There is no safe way to offer one.

So the sample path is a **lock-free ring buffer owned by C**. The callback fills
or drains it and touches nothing else; the Vyto side reads and writes it from
its own thread whenever it likes. `write()` and `read()` are that ring, and they
are why this module has no `onAudio(cb)` and will not grow one.

Two consequences worth stating, because they look like bugs otherwise:

> **A short `write()` is backpressure, not an error.** It returns how many
> samples were accepted, which is fewer than offered when the ring is full. A
> caller that ignores the return value will drop audio.

> **An underrun is normal, not exceptional.** If the Vyto side is late the
> callback emits silence rather than blocking — blocking a real-time thread is
> how one glitch becomes a system-wide stutter. `underruns()` and `overruns()`
> are how that becomes visible instead of mysterious.

## Planned backend

miniaudio: one public-domain C header covering WASAPI, CoreAudio, ALSA,
PulseAudio and AAudio. It vendors the way `stb_image_write.h` does for
`vyto/media/image` — no build system, nothing to provision — which is what makes
it a candidate at all.

`native/src` compiles once per package directory, which is why this is its own
directory: a program that shapes text with `vyto/media/font` must not link an
audio backend.
