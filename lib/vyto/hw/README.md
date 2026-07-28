# vyto/hw — peripherals: serial, input, GPIO, I2C, SPI, sensors, USB, camera, GPS

> **Status: experimental, Linux-first.** These packages drive real devices today,
> but the APIs may still shift and the macOS/Windows backends are unfinished. See
> [Platform support](#platform-support) before you depend on one.

Fourteen packages, one shape. Every peripheral here reduces to a **poll-able fd**
(anything that streams) or an **owned handle** (request/response), and both cross
the C boundary as nothing but `fd` + `handle` + `byte[]`.

This file is the API reference. [`docs/hardware.md`](../../../docs/hardware.md)
is the guide — how the pieces fit together, wiring notes, and how to test each
one without the hardware in front of you.

```vyto
import { PollSet, POLL_READ } from "vyto/net/socket";
import { serial_open } from "vyto/hw/serial";

let port = serial_open("/dev/ttyUSB0", 115200);   // null if absent or denied
if (port == null) { print("cannot open port"); return; }
port.writeText("AT\r\n");

let ps = new PollSet();
ps.addFd(port.pollFd(), POLL_READ);               // the same PollSet as your sockets
if (ps.wait(1000) > 0) { print(port.readText(256)); }
// the port closes when `port` leaves scope — deinit does it
```

## Modules

| Module | What it gives you | Native |
|--------|-------------------|:------:|
| `vyto/hw/serial` | UART/TTY ports: `serial_open`, read/write, baud, drain, flush | ⚙ |
| `vyto/hw/input` | evdev keyboards, mice, gamepads, touchscreens | ⚙ |
| `vyto/hw/gpio` | gpiochip lines: outputs, and inputs with edge events | ⚙ |
| `vyto/hw/i2c` | I2C master, including atomic write-then-read | ⚙ |
| `vyto/hw/spi` | SPI master, full-duplex `transfer` | ⚙ |
| `vyto/hw/sensors` | IIO sensors: one-shot values, or a triggered buffer stream | ⚙ |
| `vyto/hw/usb` | bus enumeration, and bulk transfers when permitted | ⚙ |
| `vyto/hw/camera` | V4L2 capture, YUYV frames and RGBA conversion | ⚙ |
| `vyto/hw/power` | batteries, AC adapters, thermal zones (sysfs) | ⚙ |
| `vyto/hw/uevent` | one netlink fd for *every* kernel device change | ⚙ |
| `vyto/hw/device` | byte I/O for any node with a read/write contract | ⚙ |
| `vyto/hw/ioctl` | the generic `ioctl` escape hatch, with correct `_IOC` numbers | ⚙ |
| `vyto/hw/location` | GPS: NMEA parsing and a `gpsd` client | — |
| `vyto/hw/sdr` | SDR front-end detection, over `vyto/hw/usb` | — |

⚙ = backed by a native shim in `native/src`. `location` and `sdr` are pure Vyto:
a GPS is a socket or a serial stream, and an SDR is a USB device, so neither
needed one.

**Not here, deliberately.** `vyto/net/link`, `vyto/net/wifi` and `vyto/net/raw`
manage network *interfaces* — a control plane, not a peripheral — so they live
under `net/`. `docs/hardware.md` §14 covers them because they belong to the same
conversation.

---

## The two shapes

**Streaming — a poll-able fd.** Everything that produces events over time
(`Serial`, `InputDevice`, `GpioLine` with an edge, `IioStream`, `Camera`,
`UEventMonitor`, `Device`) exposes `pollFd(): int`. Hand it to a `PollSet` from
`vyto/net/socket` and hardware readiness lands in the *same* `wait()` as your
sockets and window events — one thread, no callbacks.

**Request/response — an owned handle.** `GpioChip`, `I2cBus`, `SpiBus`,
`UsbHandle`, `IioDevice`, `PowerSupply` answer one question at a time. The fd or
handle lives in the object and closes in `deinit`.

## Two rules that hold everywhere

> **Failures are soft — until you use the result.** A missing device, a denied
> permission or an unplugged cable gives `null`, `false`, or an empty array. It
> never crashes. But calling a method on a *closed* handle **panics**: every
> package has an internal `ensure()` that fails loudly, because reading from a
> port you already closed is a bug in your program, not a condition to handle.
> This is the library-wide "panic hard, sentinel soft" rule. Check the `null`
> from the opener; do not re-check afterwards.

> **Resources free themselves.** `deinit` closes the fd. Drop the object and the
> port closes, the camera light goes out, the GPIO line releases. `close()`
> exists for when you need it *now* and is idempotent.

---

## `vyto/hw/serial`

```vyto
export fn serial_open(path: string, baud: int): Serial   // null on failure
```

| `Serial` | |
|---|---|
| `pollFd(): int` | for a `PollSet` |
| `read(n: int): byte[]` · `readText(n: int): string` | non-blocking — empty when nothing is buffered |
| `write(data: byte[]): int` · `writeText(s: string): int` | bytes written |
| `setBaud(baud: int): bool` | reconfigure without reopening |
| `drain(): bool` | block until the output buffer has actually gone out |
| `flush(): bool` | discard both buffers |
| `isValid(): bool` · `close()` | |

Opens raw 8N1. Reads never block, so read only after the `PollSet` says readable.

## `vyto/hw/input`

```vyto
export fn input_devices(): InputInfo[]                  // empty if none/denied
export fn input_open(path: string): InputDevice         // null on failure
```

| Type | |
|---|---|
| `InputInfo` | fields `path`, `name`, `vendor`, `product`; `id(): string` → `"046d:c31c"` |
| `InputEvent` | fields `type`, `code`, `value` |
| `InputDevice` | `pollFd()`, `poll(): InputEvent` (null when nothing is queued), `drain(): InputEvent[]`, `isValid()`, `close()` |

Constants: `EV_SYN` `EV_KEY` `EV_REL` `EV_ABS`; `BTN_LEFT` `BTN_RIGHT`
`BTN_MIDDLE` `BTN_SOUTH` `BTN_EAST`; `REL_X` `REL_Y`; `ABS_X` `ABS_Y`.

For `EV_KEY`, `value` is 1 press, 0 release, 2 autorepeat. Prefer `drain()` — one
physical action emits several events plus an `EV_SYN` separator.

## `vyto/hw/gpio`

```vyto
export fn gpio_open_chip(path: string): GpioChip        // null on failure
```

| `GpioChip` | |
|---|---|
| `name(): string` · `lines(): int` | chip label and line count |
| `requestOutput(offset: int, value: int): GpioLine` | claim a line as an output at an initial level |
| `requestInput(offset: int, edge: int): GpioLine` | claim a line as an input, watching `edge` |
| `isValid()` · `close()` | |

| `GpioLine` | |
|---|---|
| `get(): int` · `set(value: int): bool` | level |
| `pollFd(): int` | readable when an edge arrives (edge lines only) |
| `readEvent(): int` | 1 rising, 2 falling |
| `isValid()` · `close()` | releasing the line |

Edges: `EDGE_NONE` `EDGE_RISING` `EDGE_FALLING` `EDGE_BOTH`.

## `vyto/hw/i2c`

```vyto
export fn i2c_open_bus(path: string): I2cBus            // null on failure
```

| `I2cBus` | |
|---|---|
| `write(addr: int, data: byte[]): bool` | |
| `read(addr: int, n: int): byte[]` | short or empty on failure |
| `writeRead(addr: int, wdata: byte[], n: int): byte[]` | **one atomic transaction** |
| `readReg(addr: int, reg: int, n: int): byte[]` | the write-pointer-then-read idiom |
| `isValid()` · `close()` | |

The address travels with every call rather than being latched on the bus, so two
devices can be interleaved without a mode switch.

## `vyto/hw/spi`

```vyto
export fn spi_open_bus(path: string, mode: int, speedHz: int): SpiBus
```

| `SpiBus` | |
|---|---|
| `transfer(tx: byte[]): byte[]` | full-duplex — the result is exactly `tx.len` long |
| `write(tx: byte[]): bool` | send, discarding what came back |
| `configure(mode: int, bits: int, speedHz: int): bool` | change clock/mode on the fly |
| `isValid()` · `close()` | |

SPI clocks bytes out and in simultaneously; `transfer` returns what arrived
*while* your bytes were leaving, which is why a register read is one call.

## `vyto/hw/sensors` (IIO)

```vyto
export fn iio_devices(): IioDevice[]                    // empty if none
```

| `IioDevice` | |
|---|---|
| fields `index`, `name` | e.g. `"bmi160"` |
| `channels(): string[]` | e.g. `["accel_x", "accel_y", "temp"]` |
| `value(chan): float` | **real units** — `(raw + offset) * scale` |
| `raw(chan): float` · `scale(chan): float` · `offset(chan): float` | the parts |
| `attr(name, fallback: float): float` | any sysfs attribute |
| `writeAttr(attr, val: string): bool` | |
| `enableChannel(chan, on: bool): bool` | for buffered capture |
| `setTrigger(name: string): bool` · `enableBuffer(on: bool): bool` | |
| `stream(): IioStream` | `/dev/iio:deviceN` |

| `IioStream` | `pollFd(): int`, `read(n: int): byte[]`, `isValid()`, `close()` |
|---|---|

`value()` is what you want for a one-shot read. Buffered capture hands back
**raw packed bytes**: the record layout is per-device (see its `scan_elements/`),
so decoding it is your job.

## `vyto/hw/usb`

```vyto
export fn usb_devices(): UsbDevice[]                    // enumeration needs no permission
```

| `UsbDevice` | |
|---|---|
| fields `vid`, `pid`, `bus`, `addr`, `cls`, `manufacturer`, `product`, `serial`, `speed` | `speed` is Mbit/s as text: `"480"`, `"5000"` |
| `id(): string` | `"0bda:5520"` |
| `name(): string` | manufacturer + product, falling back to the id |
| `open(): UsbHandle` | **null without write access to the device node** |

| `UsbHandle` | |
|---|---|
| `claim(iface: int): bool` · `release(iface: int): bool` | |
| `bulkIn(ep: int, n: int, timeoutMs: int): byte[]` | |
| `bulkOut(ep: int, data: byte[], timeoutMs: int): int` | |
| `isValid()` · `close()` | |

## `vyto/hw/camera`

```vyto
export fn camera_open(path: string, width: int, height: int): Camera
```

| `Camera` | |
|---|---|
| `width(): int` · `height(): int` | what the driver actually gave you, not what you asked for |
| `fourcc(): int` · `format(): string` | `"YUYV"`, `"MJPG"`, … |
| `pollFd(): int` | readable when a frame is ready |
| `grab(): byte[]` | one raw frame in the native format |
| `grabRgba(): byte[]` | `width*height*4`, ready to blit to a Surface |
| `capture(handler: fn(byte[]): bool, timeoutMs: int)` | drives its own `PollSet`; return false to stop |
| `captureRgba(handler: fn(byte[]): bool, timeoutMs: int)` | same, converted |
| `record(path: string, frames: int): int` | raw frames to a flat file; returns the count |
| `isValid()` · `close()` | |

`FOURCC_YUYV` is exported. Uncompressed YUYV is what most webcams offer and the
only format converted to RGBA — a MJPEG-only camera reports so through
`format()`, and decoding it is not built in.

> `record()` writes **raw** frames with no container and no encoder. Mux with
> ffmpeg: `ffmpeg -f rawvideo -pix_fmt yuyv422 -s 640x480 -i out.yuv out.mp4`.

## `vyto/hw/power`

```vyto
export fn power_supplies(): PowerSupply[]
export fn thermal_zones(): ThermalZone[]
```

| `PowerSupply` | field `name` (`"BAT0"`, `"AC"`); `kind()`, `isBattery()`, `isMains()`, `online(): bool`, `capacity(): int` (percent, -1 unknown), `status(): string`, `voltage(): float` |
|---|---|
| `ThermalZone` | field `index`; `kind(): string`, `tempC(): float` |

World-readable — no group needed. These are **snapshots**; for change
notifications use `hw/uevent`.

## `vyto/hw/uevent`

```vyto
export fn uevent_open_monitor(): UEventMonitor          // null on failure
export fn parse_uevent(msg: string): UEvent             // pure, so it is testable offline
```

| `UEventMonitor` | `pollFd(): int`, `next(): UEvent` (null when nothing is queued), `isValid()`, `close()` |
|---|---|
| `UEvent` | **fields** `action`, `subsystem`, `devpath`; `get(key: string): string`, `name(): string` |

> `action` and `subsystem` are **fields, not methods** — `e.action`, not
> `e.action()`. `name()` and `get()` are methods.

One unprivileged netlink fd carries every kernel device change: AC unplugged,
battery level moved, USB attached, cable pulled.

## `vyto/hw/location`

Pure Vyto — a GPS is a socket (`gpsd`) or a serial NMEA stream, so no shim.

```vyto
export fn nmea_parse(sentence: string): Fix
export fn gpsd_connect(host: string, port: int): Gpsd   // null if no daemon
```

| `Fix` | fields `lat`, `lon` (degrees, negative = S/W), `alt` (metres), `speed` (m/s), `valid`, `source` (`"gpsd"` or `"nmea"`) |
|---|---|
| `Gpsd` | `start(): bool`, `poll(): Fix` — null until a fix arrives |

`valid` is false until the receiver has satellites, so check it before believing
`lat`/`lon`. Pair `nmea_parse` with `vyto/hw/serial` for a directly-attached
receiver.

## `vyto/hw/sdr`

Pure Vyto over `vyto/hw/usb`.

```vyto
export fn sdr_devices(): SdrDevice[]
```

| `SdrDevice` | fields `vid`, `pid`, `bus`, `addr`, `model`; `id(): string` |
|---|---|

Recognises RTL-SDR, HackRF (One/Jawbreaker/rad1o), Airspy, Airspy HF+, bladeRF
and SDRplay. **Detection only** — IQ streaming is not built. FTDI-based front
ends are deliberately *not* matched, since their ids are the same generic
`0403:6010` every FTDI serial adapter uses.

## `vyto/hw/device` — anything with a read/write contract

```vyto
export fn device_open(path: string, mode: int, nonblock: bool): Device
```

| `Device` | `read(n): byte[]`, `readText(n): string`, `write(data): int`, `writeText(s): int`, `seek(off, whence): int`, `pollFd(): int`, `rawFd(): int`, `isValid()`, `close()` |
|---|---|

Modes `READ` `WRITE` `RDWR` `CREATE`; whences `SEEK_SET` `SEEK_CUR` `SEEK_END`.

For hidraw, ttys, `/dev/urandom`, `/dev/cec*`, pipes — any node where `read()`
and `write()` are the whole contract. `rawFd()` is what you pass to `ioctl`.

## `vyto/hw/ioctl` — the escape hatch

```vyto
export fn ioctl(fd: int, req: int, arg: byte[]): int
export fn ioctlInt(fd: int, req: int, val: int): int
export fn io(type, nr): int · ior(type, nr, size) · iow(...) · iowr(...)
export fn leInt(buf: byte[]): int
```

Request numbers come from the kernel's own `_IOC` macro through the shim, so
they are correct on every architecture — never hardcode a magic constant. Pass a
`byte[]` laid out like the C struct.

> **`ioctl` cannot `mmap`.** A device needing a mapped ring buffer (V4L2
> `/dev/video*`) still needs a typed package like `hw/camera`. Everything else
> ioctl-driven is reachable from here — this is the layer `hw/gpio` and `hw/i2c`
> are built on, and the right foundation for a new pure-Vyto device module.

---

## Permissions

If a package returns an empty list or a `null` handle on hardware you know is
present, it is almost always this.

| Package | Node | Group |
|---|---|---|
| `hw/serial` | `/dev/ttyUSB*`, `/dev/ttyACM*` | `dialout` |
| `hw/input` | `/dev/input/event*` | `input` |
| `hw/gpio` | `/dev/gpiochip*` | `gpio` |
| `hw/i2c` | `/dev/i2c-*` | `i2c` |
| `hw/spi` | `/dev/spidev*` | `spi` |
| `hw/camera` | `/dev/video*` | `video` |
| `hw/usb` **transfers** | `/dev/bus/usb/*` | a udev rule, or root |

```sh
sudo usermod -aG dialout $USER      # then re-login
```

USB *enumeration*, IIO sensors, `hw/power` and `hw/uevent` need no group.

## Testing without the hardware

| Package | Stand-in |
|---|---|
| `hw/gpio` | `gpio-sim` kernel module — virtual chips |
| `hw/i2c` | `i2c-stub` kernel module |
| `hw/camera` | `vivid` kernel module — a virtual webcam |
| `hw/spi` | a MISO→MOSI jumper loops `transfer` back |
| `hw/uevent` | `parse_uevent()` is pure and takes a literal message |
| `hw/location` | `nmea_parse()` is pure and takes a literal sentence |

The examples (`examples/51_usb.vt` … `examples/69_uevent.vt`) print
machine-specific output, so they carry **no `.expected` golden** and the test
suite reports them as `SKIP`. Run them directly against your own hardware.

## Platform support

| | State |
|---|---|
| **Linux** | everything above, verified |
| **macOS** | `hw/serial` works (POSIX). `hw/usb` has an IOKit backend that is **untested**. The rest return empty/`null`. |
| **Windows** | a serial COM-port backend exists, **untested**, and does not yet join a `PollSet`. The rest return empty/`null`. |

Nothing fails to *compile* off Linux — a package returns an empty list or `null`
instead, so a cross-platform program keeps building.

Displays and external monitors are not `hw/*`; that is `vyto/surface`.

## See also

- [`docs/hardware.md`](../../../docs/hardware.md) — the guide: how the shapes fit
  together, one worked example per package, the event story, wiring notes.
- Each package's own header comment documents its full API and its quirks.
- `vyto/net/socket` for `PollSet`/`POLL_READ`, which every streaming package
  above feeds into.
