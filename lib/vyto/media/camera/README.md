# vyto/media/camera — capture, on any platform

```vyto
import { camera_open } from "vyto/media/camera";

let cam = camera_open(640, 480);
if (cam == null) { print("no camera"); return; }
cam.start();
let img = cam.grab();                // an Image, or null if none ready yet
```

## Status: stub. Every entry point panics.

Nothing here is implemented. The signatures in `camera.vt` are real and settled
so the shape can be reviewed and imported against, and so this list is checkable
rather than a promise — the `vyto/crypto/openssl` convention.

### Implemented

Nothing yet.

### Not implemented — these still `panic`

`camera_devices`, `CameraDevice`, `camera_open`, `camera_open_device`,
`Camera` (`start`, `stop`, `grab`, `hasFrame`, `dropped`), `camera_available`.

### Also out of scope, and likely to stay there

Encoding or recording what is captured (compose with `vyto/media/image` or
`vyto/media/video`), autofocus and exposure control beyond what a mode
selection implies, and any image processing.

## The relationship to `vyto/hw/camera`, which ships and works

**This does not replace `vyto/hw/camera`. If it ever becomes a second way to
open `/dev/video0`, it should be deleted instead.**

`vyto/hw/camera` is the Linux V4L2 device interface. It is deliberately a
*device*: it exposes a poll-able fd, hands back YUYV or RGBA buffers, follows
the `vyto/hw` contract in `docs/hardware.md`, and returns null on every platform
that is not Linux. That is the right shape for a program already driving
hardware — it folds into the same `PollSet` as a serial port.

This module is the façade above it: one API that also reaches AVFoundation on
macOS/iOS, Media Foundation on Windows, and Camera2/NDK on Android, and that
produces `vyto/media/image` `Image`s rather than raw device buffers. **On Linux
its backend is `vyto/hw/camera`** — there is no second V4L2 implementation.

| Want | Use |
|---|---|
| a camera on Linux, in a `PollSet`, as an fd | `vyto/hw/camera` |
| a camera on whatever the user happens to have | this |

The division is platform reach and output type, not functionality.

## Why it is not just a thin wrapper

The three non-Linux backends are not device files and do not resemble one.
AVFoundation and Camera2 are both **callback-driven** — frames arrive on a
thread the OS owns — which runs into the same constraint `vyto/media/audio`
documents at length: a foreign thread may touch C memory only, never a Vyto
object. So the frame path here is the same shape as the audio one, a C-owned
buffer the Vyto side drains on its own thread.

Two consequences, both of which look like bugs otherwise:

> **`grab()` returning null means "no frame ready", not "failed".** Frames
> arrive at the sensor's rate on a thread the caller does not control. A caller
> polls from its own loop and draws when it gets something.

> **There is no fd in this API.** Only one of the four backends has one, and
> exposing it on Linux alone would make every program written against it
> Linux-only by accident.

Also worth knowing:

> **The requested size is a request.** Cameras offer a fixed list of modes, not
> arbitrary sizes, so the backend picks the nearest — check `width`/`height`
> afterwards rather than assuming.

> **Opening a camera can be refused by a human.** On macOS, iOS and Android it
> is a runtime permission prompt. `camera_open` returning null is an ordinary
> outcome, not a broken install.
