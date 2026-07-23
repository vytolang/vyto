# Hardware & peripherals in Vyto

> **Status: experimental.** The `vyto/hw/*` packages are young and Linux-first.
> They cover real devices today, but APIs may shift and some backends
> (Windows/macOS) are unfinished. Great for prototyping device tools and
> embedded/MCU control; validate on your hardware before relying on it.

Vyto talks to cameras, sensors, input devices, GPIO, serial, USB, and GPS through
small standard-library packages under `vyto/hw/*`. Every one follows the same two
shapes, so once you learn one you know them all.

## 1. The two shapes

Every peripheral reduces to one of two forms:

1. **A poll-able fd** — for anything that *streams*: serial bytes, key/gamepad
   events, GPIO edges, sensor sample buffers. The device's file descriptor joins a
   `PollSet` (from `vyto/net/socket`), so hardware readiness arrives in the **same**
   `wait()` loop as your sockets and windows — one thread, no callbacks.
2. **An owned handle** — for request/response: enumerate a bus, read one register,
   grab one sample, transfer a buffer. The handle lives in an object and its
   `deinit` releases the device automatically when the object goes out of scope.

Both cross the C boundary as just **fd + handle + `byte[]`**. Two rules hold
everywhere:

- **Failures are soft.** A missing device, a denied permission, or an unplugged
  cable returns `null` / `false` / an empty result — never a crash. Check for it.
- **Resources free themselves.** The fd or handle closes in `deinit`; drop the
  object (let it leave scope) and the port closes, the light goes off, the line
  releases. No manual cleanup needed, no garbage-collector pause.

```js
// The universal streaming loop — identical for serial, input, gpio, sensors:
import { PollSet, POLL_READ } from "vyto/net/socket";

let ps = new PollSet();
ps.addFd(dev.pollFd(), POLL_READ);        // dev = any streaming hw object
while (true) {
    if (ps.wait(-1) > 0) {                 // -1 = block forever
        handle(dev.read(256));             // read only after it reports readable
    }
}
```

Everything below is Linux-first. On other platforms a package returns an empty
list or `null` rather than failing to compile.

## 2. Permissions (read this first)

Most device nodes are group-restricted. If a package returns an empty list or a
`null` handle on hardware you know is present, it is almost always permission.

| Package | Node | Typical group |
|---|---|---|
| `hw/serial` | `/dev/ttyUSB*`, `/dev/ttyACM*` | `dialout` |
| `hw/input` | `/dev/input/event*` | `input` |
| `hw/gpio` | `/dev/gpiochip*` | `gpio` |
| `hw/i2c` | `/dev/i2c-*` | `i2c` |
| `hw/usb` (transfers) | `/dev/bus/usb/*` | a udev rule, or root |

Add yourself to a group once, then re-login:

```sh
sudo usermod -aG dialout $USER      # or input / gpio / i2c
```

USB **enumeration** and **sensor** reads are world-readable — no group needed.

## 3. Serial ports — `vyto/hw/serial`

A serial port is a non-blocking fd. Open it, then either read/write directly or
fold it into a `PollSet`.

```js
import { PollSet, POLL_READ } from "vyto/net/socket";
import { serial_open } from "vyto/hw/serial";

let port = serial_open("/dev/ttyUSB0", 115200);   // raw 8N1
if (port == null) { print("cannot open port"); return; }

port.writeText("AT\r\n");

let ps = new PollSet();
ps.addFd(port.pollFd(), POLL_READ);
if (ps.wait(1000) > 0) {                            // 1s timeout
    print(port.readText(256));                      // whatever is buffered
}
// port closes automatically when it leaves scope
```

Reads are non-blocking: `read`/`readText` return whatever is available (empty if
nothing yet), so only read after the `PollSet` reports the fd readable. Also:
`setBaud(n)`, `drain()` (wait until sent), `flush()` (discard buffers).

## 4. Input devices — `vyto/hw/input`

Keyboards, mice, gamepads, and touchscreens via evdev. List them, open one, and
stream decoded `{type, code, value}` events through a `PollSet`.

```js
import { PollSet, POLL_READ } from "vyto/net/socket";
import { input_devices, input_open, EV_KEY } from "vyto/hw/input";

for (let d in input_devices()) {
    print(d.path + "  " + d.name);                  // "/dev/input/event3  Logitech K120"
}

let dev = input_open("/dev/input/event3");
if (dev == null) { print("no device / not in input group"); return; }

let ps = new PollSet();
ps.addFd(dev.pollFd(), POLL_READ);
while (true) {
    if (ps.wait(-1) > 0) {
        for (let e in dev.drain()) {                 // all buffered events
            if (e.type == EV_KEY) {                   // value 1=press 0=release 2=repeat
                print("key " + e.code + " = " + e.value);
            }
        }
    }
}
```

Event types: `EV_KEY` (keys/buttons), `EV_REL` (mouse motion), `EV_ABS`
(joystick/touch position), `EV_SYN` (packet separator). Common codes are exported
(`BTN_LEFT`, `BTN_SOUTH`, `REL_X`, `ABS_X`, …).

## 5. GPIO — `vyto/hw/gpio`

General-purpose I/O over the modern gpiochip character device. Drive an output, or
watch an input for edges (an edge line is a poll-able fd).

```js
import { PollSet, POLL_READ } from "vyto/net/socket";
import { gpio_open_chip, EDGE_BOTH } from "vyto/hw/gpio";

let chip = gpio_open_chip("/dev/gpiochip0");
if (chip == null) { print("no chip / not in gpio group"); return; }
print(chip.name() + ", " + chip.lines() + " lines");

let led = chip.requestOutput(17, 0);                // line 17, start low
led.set(1);
print("line 17 is now " + led.get());

let button = chip.requestInput(27, EDGE_BOTH);      // watch line 27
let ps = new PollSet();
ps.addFd(button.pollFd(), POLL_READ);
if (ps.wait(-1) > 0) {
    print("edge: " + button.readEvent());           // 1 rising, 2 falling
}
```

No hardware to test with? The kernel's `gpio-sim` module creates virtual chips.

## 6. I2C — `vyto/hw/i2c`

Talk to I2C sensors and peripherals. The address travels with every transfer, so a
combined write-then-read (the standard "set register pointer, read register" idiom)
is one atomic transaction.

```js
import { i2c_open_bus } from "vyto/hw/i2c";

let bus = i2c_open_bus("/dev/i2c-1");
if (bus == null) { print("no bus / not in i2c group"); return; }

// Read the WHO_AM_I register (0x75) of an MPU-6050 at address 0x68:
let id = bus.readReg(0x68, 0x75, 1);                // reg, byte count
if (id.len > 0) { print("chip id = " + id[0]); }

// Or the primitives:
let cmd = bytes(1); cmd[0] = 0x6B as byte;
bus.write(0x68, cmd);                                // write bytes
let raw = bus.read(0x68, 6);                          // read bytes
let combined = bus.writeRead(0x68, cmd, 6);          // atomic write-then-read
```

Test without wiring via the `i2c-stub` kernel module.

## 7. Sensors (IIO) — `vyto/hw/sensors`

Accelerometers, gyroscopes, light/temperature/pressure sensors, and ADCs through
the kernel's Industrial I/O subsystem. Two modes: read one sample on demand, or
stream a triggered buffer.

```js
import { iio_devices } from "vyto/hw/sensors";

for (let dev in iio_devices()) {
    print(dev.name);                                 // "bmi160", "als", ...
    for (let ch in dev.channels()) {                 // "accel_x", "temp", ...
        print("  " + ch + " = " + dev.value(ch));    // processed: (raw+offset)*scale
    }
}
```

`value(ch)` gives the reading in real units; `raw(ch)` is the unscaled integer.
For continuous capture, enable channels + a trigger and stream the buffer fd:

```js
let dev = iio_devices()[0];
dev.enableChannel("accel_x", true);
dev.setTrigger("my-trigger");
dev.enableBuffer(true);
let s = dev.stream();                                // /dev/iio:deviceN as a fd
// register s.pollFd() in a PollSet; s.read(n) returns packed scan bytes
```

The packed record layout is device-specific (see its `scan_elements/`), so the
buffer hands back raw `byte[]` for you to decode.

## 8. USB — `vyto/hw/usb`

Enumerate the bus (root-free), and optionally open a device to move bytes.

```js
import { usb_devices } from "vyto/hw/usb";

for (let d in usb_devices()) {
    print(d.id() + "  " + d.name());                 // "0bda:5520  Integrated Webcam"
}
```

Transfers need device-node write access (a udev rule or root):

```js
let dev: UsbDevice = null;
for (let d in usb_devices()) { if (d.id() == "1234:5678") { dev = d; } }

let h = dev.open();                                   // null if no permission
if (h != null && h.claim(0)) {                        // claim interface 0
    let data = h.bulkIn(0x81, 64, 500);              // endpoint, max bytes, timeout ms
    print("read " + data.len + " bytes");
    h.release(0);
}                                                     // h closes on scope exit
```

## 9. Location / GPS — `vyto/hw/location`

A GPS is just a socket (the `gpsd` daemon) or a serial NMEA stream, so this package
is pure Vyto — no device shim. Parse NMEA directly, or stream fixes from `gpsd`.

```js
import { nmea_parse, gpsd_connect } from "vyto/hw/location";

// Parse a raw NMEA sentence (e.g. read from a serial GPS via vyto/hw/serial):
let fix = nmea_parse("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,,,*47");
if (fix.valid) { print(fix.lat + ", " + fix.lon + " @ " + fix.alt + "m"); }

// Or stream from a running gpsd daemon:
let gps = gpsd_connect("localhost", 2947);
if (gps != null) {
    gps.start();
    let f = gps.poll();                               // Fix, or null until one arrives
    if (f != null && f.valid) { print(f.lat + ", " + f.lon); }
}
```

A `Fix` carries `lat`, `lon` (degrees, negative = S/W), `alt` (metres), `speed`
(m/s), and `valid` — false when there is no satellite fix yet.

## 10. Cameras — `vyto/hw/camera`

Webcam capture over V4L2. A camera is the fullest form of the pattern — a poll-able
fd for frame-ready *and* a `byte[]` frame — so it drives from a `PollSet` like any
other stream, then hands back pixels to draw.

```js
import { PollSet, POLL_READ } from "vyto/net/socket";
import { camera_open } from "vyto/hw/camera";

let cam = camera_open("/dev/video0", 640, 480);
if (cam == null) { print("no camera / not in video group"); return; }
print("capturing " + cam.width() + "x" + cam.height() + " " + cam.format());

let ps = new PollSet();
ps.addFd(cam.pollFd(), POLL_READ);
if (ps.wait(2000) > 0) {
    let rgba = cam.grabRgba();          // width*height*4, ready to blit to a Surface
    if (rgba.len > 0) { /* draw rgba */ }
}
```

Or stream continuously — `capture()` drives its own `PollSet` and calls a handler per
frame, stopping when the handler returns false (`captureRgba()` for converted frames):

```js
let n: int[] = [0];                     // box: closures capture by value
cam.capture((frame) => {
    n[0] += 1;
    process(frame);
    return n[0] < 100;                  // false stops the stream
}, -1);                                 // -1 = block until each frame
```

Record straight to disk with `record(path, frames)` — it writes each raw frame to a
flat `.yuv` file (no encoder involved) that ffmpeg muxes into a real video:

```js
let n = cam.record("out.yuv", 150);     // ~5s at 30fps, uncompressed
```
```sh
ffmpeg -f rawvideo -pix_fmt yuyv422 -s 640x480 -i out.yuv out.mp4
```

Captures uncompressed **YUYV** (the common webcam format) and converts to RGBA on
demand; `grab()` gives the raw frame, `grabRgba()` the blit-ready pixels. A device
that only offers MJPEG reports its fourcc via `format()` so you can detect it (MJPEG
decoding is not built in). Dropping the `Camera` turns the capture off — the light
goes out. No webcam? The `vivid` kernel module provides a virtual one.

## 11. Any other device — `vyto/hw/device` + `vyto/hw/ioctl`

For a device with no typed package, two generic layers reach it directly.

**`hw/device`** — byte I/O for any node whose contract is `read()`/`write()`
(hidraw, tty, `/dev/random`, `/dev/cec*`, pipes):

```js
import { device_open, READ } from "vyto/hw/device";

let d = device_open("/dev/urandom", READ, false);
let seed = d.read(16);                              // 16 bytes
```

**`hw/ioctl`** — the escape hatch for the *ioctl-driven* majority (the ones a plain
`read()` can't touch). Give it an fd, a request number, and a `byte[]` mirroring the
C struct. Build request numbers with `io/ior/iow/iowr` — they call the kernel's own
`_IOC` macro, so they are correct on every architecture, no hardcoded magic:

```js
import { device_open, RDWR } from "vyto/hw/device";
import { ioctl, ior, leInt } from "vyto/hw/ioctl";

let d = device_open("/dev/random", RDWR, false);
let RNDGETENTCNT = ior(0x52, 0x00, 4);              // _IOR('R', 0, int)
let buf = bytes(4);
if (ioctl(d.rawFd(), RNDGETENTCNT, buf) >= 0) {
    print("entropy: " + leInt(buf) + " bits");
}
```

This is exactly the pattern the typed packages (`hw/gpio`, `hw/i2c`) wrap — use it
for one-off control calls, or as the foundation for a new pure-Vyto device module.
Its one limit: `ioctl` can't `mmap`, so devices needing a mapped ring (V4L2
`/dev/video*`) still want a typed package like `hw/camera`.

## 12. SPI — `vyto/hw/spi`

SPI master over `/dev/spidev*`, the sibling of I2C. SPI is **full-duplex** — every
transfer clocks bytes out and in at the same time, so `transfer()` returns what was
read while your bytes were written.

```js
import { spi_open_bus } from "vyto/hw/spi";

let spi = spi_open_bus("/dev/spidev0.0", 0, 500000);   // mode 0, 500 kHz
if (spi == null) { print("no bus / not in spi group"); return; }

let tx = bytes(2); tx[0] = 0x9F as byte;               // e.g. flash JEDEC-ID read
let rx = spi.transfer(tx);                             // rx.len == tx.len
```

`write()` sends without keeping the reply; `configure(mode, bits, speedHz)` changes the
clock on the fly. No easy kernel mock — a MISO→MOSI jumper loops `tx` back to test.

## 13. Power & thermal — `vyto/hw/power`

Battery, AC adapter, and temperatures — pure sysfs reads, exactly what a status bar or
a thermal-aware daemon needs.

```js
import { power_supplies, thermal_zones } from "vyto/hw/power";

for (let s in power_supplies()) {
    if (s.isBattery()) { print(s.name + ": " + s.capacity() + "% " + s.status()); }
    if (s.isMains())   { print(s.name + ": plugged in = " + s.online()); }
}
for (let z in thermal_zones()) { print(z.kind() + ": " + z.tempC() + "C"); }
```

No permission needed (world-readable). `PowerSupply` also gives `voltage()`;
`ThermalZone` gives `tempC()` as a float.

## 14. Networking control plane — `vyto/net/link`, `net/wifi`, `net/raw`

Sending data is `vyto/net/socket` (the kernel abstracts ethernet/WiFi away). These
packages manage the *interfaces themselves* — a different job, so they live under
`net/`, not `hw/`.

**Interface status** (`net/link`) — up/down, MAC, speed, carrier, byte counters, via
sysfs:
```js
import { link_interfaces } from "vyto/net/link";
for (let n in link_interfaces()) {
    print(n.name + "  " + n.operstate() + "  " + n.mac() + "  rx=" + n.rxBytes());
}
```

**WiFi scan/connect** (`net/wifi`) — the onboarding flow, over the wpa_supplicant
control socket (same channel as `wpa_cli`, far less code than raw nl80211):
```js
import { wifi_open } from "vyto/net/wifi";
let w = wifi_open("wlan0");                 // null without the netdev group / daemon
if (w != null) {
    w.scan();
    for (let ap in w.scanResults()) { print(ap.ssid + "  " + ap.signal + "dBm"); }
    w.connect("MyNetwork", "hunter2");       // "" psk for an open network
}
```

**Raw frames** (`net/raw`) — sniff/inject whole Ethernet frames below IP, via
`AF_PACKET` (needs `CAP_NET_RAW`/root). Folds into a `PollSet`; `dstMac`/`srcMac`/
`etherType` helpers decode the header.

## 15. Software-defined radio — `vyto/hw/sdr`

Detects RTL-SDR / HackRF / Airspy / bladeRF / SDRplay front-ends on the USB bus (built
on `vyto/hw/usb`, no new shim):
```js
import { sdr_devices } from "vyto/hw/sdr";
for (let d in sdr_devices()) { print(d.model + "  " + d.id()); }
```
Detection today; **IQ streaming** (tuner config + bulk transfers) is a later increment —
via librtlsdr/SoapySDR, or raw `hw/usb` bulk. LoRa/nRF-style radios instead sit on
`hw/spi` + `hw/gpio`; a cellular modem is `hw/serial` (AT commands).

## 16. Events — knowing when something *changes*

Most status packages (`hw/power`, `net/link`) are snapshots — you read them. To learn
the moment something changes there are two paths:

**Thresholds are always your code.** There is no kernel "battery below 20%" or "rate
below Y" event — you sample (on a timer or an event) and compare. `PollSet.wait(ms)`
already returns on timeout, so a sample loop needs nothing new:

```js
let ps = new PollSet();
while (true) {
    ps.wait(1000);                              // wake each second (or on any fd)
    for (let s in power_supplies()) {
        if (s.isBattery() && s.capacity() < 20) { print("battery low!"); }
    }
}
```

**State changes are push events** on a poll-able fd — fold into the same `PollSet`:

- **`hw/uevent`** — one netlink fd for *every* kernel device change: AC unplugged,
  battery level changed, cable pulled, USB attached. Unprivileged.
  ```js
  import { uevent_open_monitor } from "vyto/hw/uevent";
  let mon = uevent_open_monitor();
  ps.addFd(mon.pollFd(), POLL_READ);
  // on wake:
  let e = mon.next();
  if (e != null && e.subsystem() == "power_supply") {
      print(e.action() + " " + e.name() + " online=" + e.get("POWER_SUPPLY_ONLINE"));
  }
  ```
- **`net/wifi` monitor** — wpa_supplicant *pushes* `CTRL-EVENT-DISCONNECTED` /
  `-CONNECTED` the instant the link changes — the right answer to "how does my app know
  WiFi dropped," far better than polling `operstate`:
  ```js
  import { wifi_monitor } from "vyto/net/wifi";
  let m = wifi_monitor("wlan0");
  ps.addFd(m.pollFd(), POLL_READ);
  let ev = m.next();
  if (ev != null && ev.isDisconnect()) { print("WiFi dropped!"); }
  ```
- **`net/raw`** is already push (each frame is an event).

All three are just more fds in your one `PollSet.wait()` — hardware changes, WiFi
state, packets, sockets, camera frames, and GUI input arrive in a single loop.

## 17. Runnable examples

Each package ships a working example. They print machine-specific output, so run
them directly:

```sh
./vytoc run examples/51_usb.vt              # list USB devices
./vytoc run examples/52_serial.vt -- /dev/ttyUSB0 115200
./vytoc run examples/54_input.vt            # list input devices
./vytoc run examples/57_gpio.vt             # list gpiochips
./vytoc run examples/58_i2c.vt              # list i2c buses
./vytoc run examples/56_sensors.vt          # list IIO sensors
./vytoc run examples/59_location.vt         # parse sample GPS fixes
./vytoc run examples/60_device.vt           # read /dev/urandom bytes
./vytoc run examples/61_ioctl.vt            # generic ioctl (kernel entropy count)
./vytoc run examples/62_camera.vt           # capture webcam frames
./vytoc run examples/63_netlink.vt          # network interface status
./vytoc run examples/64_power.vt            # battery / AC / thermal
./vytoc run examples/65_spi.vt              # list spidev buses
./vytoc run examples/67_wifi.vt             # parse a WiFi scan (+ event)
./vytoc run examples/68_sdr.vt              # detect SDR dongles
./vytoc run examples/69_uevent.vt           # watch kernel device events
```

## 18. Platform support

Everything above is **Linux-first and verified there**. Current state elsewhere:

- **macOS** — serial works (POSIX). USB enumeration has an IOKit backend that is
  **untested** and opt-in. Other packages return empty/`null`.
- **Windows** — a serial COM-port backend exists but is **untested** and does not
  yet integrate with `PollSet`. Other packages return empty/`null`.
- **Displays / external monitors** are *not* part of `hw/*` — that is the
  `vyto/surface` layer (windowing), and multi-monitor support is not yet built.

## 19. Where to go next

- [Getting started](getting-started.md) — build the compiler, run your first app.
- Each package's source under `lib/vyto/hw/` documents its full API in the header
  comment (`serial.vt`, `input.vt`, `gpio.vt`, `i2c.vt`, `sensors.vt`, `usb.vt`,
  `location.vt`, `camera.vt`, `device.vt`, `ioctl.vt`).
- With `--freestanding`, these userspace device-control packages make Vyto a
  legitimate small MCU / device-control language: deterministic, no GC, tiny.
