# vyto/media/audio — playback, capture, device enumeration

```vyto
import { audio_open_playback } from "vyto/media/audio";

let p = audio_open_playback(48000, 2);
if (p != null) {
    p.start();
    p.write(samples);                // f32 interleaved, -1..1
}
```

## Status: implemented (2026-11-19)

Backed by **miniaudio 0.11.26**, vendored byte-for-byte in
`native/src/miniaudio/` (public domain / MIT-0). `native/refresh-miniaudio.sh
--verify` checks the tree against its sha256 manifest, and `make test` runs it.

| Entry point | Does |
|---|---|
| `audio_available()` | a real backend initialised |
| `audio_backend()` | which one: `"PulseAudio"`, `"WASAPI"`, `"ALSA"`, `"Null"`, or `""` |
| `audio_devices()` | every device, both directions, as `AudioDevice` |
| `audio_open_playback[_device]` | an `AudioPlayback`, or null |
| `AudioPlayback` | `start`, `stop`, `write`, `writable`, `underruns`, `framesPlayed` |
| `audio_open_capture[_device]` | an `AudioCapture`, or null |
| `AudioCapture` | `start`, `stop`, `read`, `readable`, `overruns` |

Verified 2026-11-19:

- **Linux**: PulseAudio, 3 devices enumerated, 100 ms written = 4800 frames played
- **windows-x64** under Wine: the WASAPI backend, same numbers; the null-backend
  golden is byte-identical to Linux's
- **android-arm64**: links under `-Wl,--no-undefined`; AAudio and OpenSL are
  `dlopen`ed. Not run on a device yet
- **`--freestanding`**: both TUs carry a `VT_NO_LIBC` arm
- **macOS**: not built. miniaudio turns off run-time linking on Apple, so
  CoreAudio wants `-framework` flags that `#link` cannot express yet

Real Windows 7 hardware is the check of record for Windows, and it has not been
run yet. WASAPI exists from Vista on, and DirectSound and WinMM are behind it.

## Backends, and the two test switches

Every OS audio library is loaded at run time (`dlopen`/`LoadLibrary`), so a
program links nothing new, and a machine without PulseAudio simply falls
through to ALSA. Windows needs no `#link` at all. Linux links only
`pthread`, `dl` and `m`.

The order is miniaudio's: WASAPI, then DirectSound, then WinMM; PulseAudio,
then ALSA; AAudio, then OpenSL. `audio_config.h` whitelists those plus CoreAudio
and null, and compiles out everything above the device layer (decoding,
encoding, generation, resource manager, node graph, engine).

| `VYTO_AUDIO` | Effect |
|---|---|
| unset | every enabled backend **except null** |
| `null` | null only: a device that consumes samples at real-time pace and plays nothing |
| `off` | no backend: `audio_available()` false and every open null |

**Null is never a fallback.** A headless box must report "no audio", not
silently accept a stream that goes nowhere. That is also what makes the `off`
golden honest.

## Cost

About **170 KB** stripped, on both Linux and windows-x64 `--release`, and only
in programs that import this package. That is the price of eight backends plus
resampling and channel conversion, which happen in the shim so callers never
see a device's native format.

A ring is **half a second**: 192 KB for a 48 kHz stereo stream. Its capacity
limits how far ahead `write()` can get. It does not set latency: latency is
however much the caller chooses to queue.

## Short sounds: `vyto/media/audio/cue`

```vyto
import { alert_sounds, cue_play, cue_volume } from "vyto/media/audio/cue";

let s = alert_sounds();        // at startup: builds the sounds, opens the device
cue_play(s.error);             // returns at once; the sound plays on
```

A stream is something you keep feeding. A **cue** is a clip loaded once into C
memory and played with one call that never blocks.

- **Eight voices**, mixed in the device callback.
- **Replaying a cue that is still sounding restarts it** rather than stacking a
  copy, so a burst of five scan errors is one error sound, not five overlapping
  buzzes.
- **Five built-in alerts**, synthesised in Vyto with no files and no licences:
  `success` (two rising notes), `warning` (one note twice), `error` (a low
  falling buzz), `attention` (three rising notes), and `click`.
- **Your own sounds**: `cue_load_wav(path)` / `cue_from_wav(bytes)` take PCM
  8/16/24/32-bit or 32-bit float, any channel count, up to 30 s. Or build
  samples with `note()`, `buzz()`, `rest()` and `then()`.

| Call | |
|---|---|
| `alert_sounds()` | the five sounds, and opens the mixer so the first alert does not wait for it |
| `cue_play(c)` / `cue_play_gain(c, g)` | true when it is sounding; false with no device or all eight voices busy |
| `cue_volume(v)` | master volume 0..1, applies to sounds already playing |
| `cue_stop_all()`, `cue_playing()`, `cue_close()` | |
| `cue_from_samples(f32[], rate)` | mono; resampled to 48 kHz once, when made |

**Open the mixer at startup.** Opening the device is the slow part: 415 ms on
PulseAudio, 550 ms on WASAPI under Wine, measured 2026-11-19. Once it is open,
the first mixed frames go out 1–18 ms after `cue_play()`. The device then stays
running, and an idle mixer writes silence every ~10 ms.

**Each voice is one atomic state word, and nothing takes a lock.** The mixer
lives in `native/src/cue_shim.c`, whose header comment documents the state
transitions. A Cue's samples are freed on the Vyto thread only once no voice is
playing them, so dropping a Cue mid-sound is safe: the sound finishes first.

`tests/win/cue_demo.vt` is the listening test. It offers a menu of every alert,
bursts, overlap, volume and WAV files, plus `--auto`, which checks everything
mechanically. It is staged by `make test-win` and also run in `make test`.

## The constraint that shapes the API

**An audio callback runs on a thread the OS owns**, at real-time priority, with
a deadline of a few milliseconds. Vyto's memory model is single-address-space
reference counting with **no atomics on the refcount**, so a foreign thread may
touch C memory only. It must never retain or release a Vyto object.

That rules out the API every audio library reaches for first: a Vyto closure
invoked from the callback. There is no safe way to offer one.

So the sample path is a **lock-free single-producer/single-consumer ring owned
by C** (`ma_pcm_rb`). The callback fills or drains it, bumps two atomic
counters, and touches nothing else. The Vyto side reads and writes it from its
own thread whenever it likes. `write()` and `read()` are that ring, and they are
why this module has no `onAudio(cb)` and will not grow one.

Several consequences look like bugs if nobody states them:

> **A short `write()` is backpressure, not an error.** It returns how many
> samples were accepted, which is fewer than offered when the ring is full. A
> caller that ignores the return value will drop audio. Only whole frames are
> taken: writing 3 samples to a stereo stream takes 2.

> **An underrun is normal, not exceptional.** If the Vyto side is late, the
> callback emits silence rather than blocking. Blocking a real-time thread is
> how one glitch becomes a system-wide stutter. `underruns()` counts every
> period that came up short, **idle periods included**, so a started stream with
> nothing queued climbs steadily. What matters is the count rising while audio
> is being fed.

> **`framesPlayed()` counts what you wrote, not silence.** It is the clock to
> sync video against.

> **deinit is synchronous.** It stops the device, waiting for any callback in
> progress to return, before it frees the ring. A stream that goes out of
> scope stops making noise, and the OS thread can never see freed memory.

## Not here, and likely to stay out

Decoding compressed audio (that is `vyto/media/codec` plus `vyto/media/video`'s
decoder), mixing and effects, MIDI, spatial audio, and any DSP beyond format
conversion. This module moves samples between a program and a device.

Short sound effects, such as UI alerts, are the layer above this one,
`vyto/media/audio/cue`.

`native/src` compiles once per package directory, which is why this is its own
directory: a program that shapes text with `vyto/media/font` must not link an
audio backend.
