# Multiple windows in Vyto

How to drive more than one OS window from one Vyto process — control panels,
inspectors, kiosks, digital signage, video walls.

The short version: a window is a `Surface`, you can open as many as you like,
they share state the boring way (a variable), and the one thing you must get
right is **waiting on all of them at once**. Everything else follows from that.

Every claim below is grounded in `lib/vyto/surface/surface.vt` and
`lib/vyto/surface/native/src/vsurf.c`, with file:line pointers where the
behaviour isn't visible from `.vt` source. Working code for every pattern is in
[`apps/multiwindow/`](../apps/multiwindow/).

## 1. Opening a second window

There is no special call. `new Surface(...)` works anywhere — at startup, or
inside an event handler:

```js
import { Surface, Rect, rgb } from "vyto/surface";

let a = new Surface("Main", 400, 300);
let b = new Surface("Inspector", 300, 400);
```

Each surface is independent: its own size, its own title, its own input. A
window opened from a handler is a **peer**, not a child in any OS sense — there
is no parent/child relationship, no modality, and closing one does not close the
other. Dropping the last reference to a `Surface` closes its window
(reference counting does it; see [`memory.md`](memory.md)).

> **On X11, each `Surface` opens its own connection to the X server**
> (`vsurf.c`, `XOpenDisplay` per `vs_open`). That is why input never crosses
> between windows — but it also means N windows cost N sockets. Fine at two or
> three; a smell at twenty.

## 2. Sharing data between windows

This is the part people expect to be hard. It isn't: the windows are objects on
one thread in one process, so shared state is a variable.

```js
class Model {
    count: int;
    fn init() { this.count = 0; }
}

let m = new Model();
// ... a click in window A:
m.count += 1;
draw_a(m);      // both windows draw from the same object
draw_b(m);
```

No IPC, no serialization, no message queue, nothing to keep in sync. There is
also no locking to think about, because there is no second thread.

The corollary is that a change is not automatically visible: **you redraw the
windows that need it.** Nothing observes the model for you.

## 3. The event loop — the one thing to get right

A single-window app blocks in `wait()` and costs nothing while idle. With two
windows that stops working: blocking on A leaves B's events unserved, and
polling both in a bare loop spins a core.

The fix uses descriptors. Every X11 surface exposes the socket its events
arrive on, and `waitTimeoutFds` blocks on **this window's events or readability
on any other descriptor you name**:

```js
while (live) {
    // blocks until A has an event, B's connection is readable, or 40ms passes
    let ea = a.waitTimeoutFds(40, [b.eventFd()]);

    while (ea != EV_NONE) {
        if (ea == EV_CLOSE) { live = false; }
        else if (ea == EV_MOUSE_DOWN) { /* … */ }
        ea = a.poll();          // drain the rest of A's queue
    }

    let eb = b.poll();          // then drain B's
    while (eb != EV_NONE) {
        if (eb == EV_CLOSE) { live = false; }
        eb = b.poll();
    }
}
```

### The trap that costs an hour

**`waitTimeoutFds` and `wait_timeout` RETURN the woken window's event. They do
not leave it queued.**

```js
a.waitTimeoutFds(40, [b.eventFd()]);   // ← A's event consumed HERE
let ea = a.poll();                     // ← ...so this finds nothing
```

Window A silently stops responding while B keeps working perfectly — a
confusing failure, because A's window still looks alive and still repaints.
Assign the return value and handle it, as in the loop above.

The other window's fd is only *watched for readability*, so B's events do stay
queued for its own `poll()`. Only the window you call the method on is drained.

### The other trap: EV_CLOSE names ONE window

`EV_CLOSE` says *this* window was closed. It does not mean "quit". The obvious
loop is wrong:

```js
if (eb == EV_CLOSE) { live = false; }      // ← closes EVERY window
```

Because dropping out of the loop drops every `Surface`, closing one child takes
the whole app down with it — which looks like a library bug and is not. Decide
per window what its closing means:

```js
// a secondary window: it goes, the app stays
if (eb == EV_CLOSE) { b_open = false; }

// the primary window: closing it is closing the app
if (ea == EV_CLOSE) { live = false; }
```

Once a window is gone, stop polling it and stop passing its `eventFd()` to
`waitTimeoutFds` — the descriptor is dead:

```js
let wfds: int[] = [];
if (b_open) { wfds.push(b.eventFd()); }
let ea = a.waitTimeoutFds(40, wfds);
```

For a variable number of windows, filter the list instead of tracking flags —
`apps/multiwindow/spawn.vt` and `videowall.vt` both keep an array and rebuild
it, which also releases the closed window's `Surface`.

### Scaling to N windows

Collect the descriptors:

```js
let fds: int[] = [];
for (let w in others) { fds.push(w.eventFd()); }
let ev = main.waitTimeoutFds(40, fds);
```

### Where this degrades

`canWaitFds()` (`surface.vt:665`) reports whether the backend really waits on
your descriptors. **It is true on X11 and false on Win32, Android, fbdev and
headless** — those fall back to a bounded poll, which is the same ~60Hz wakeup
you would have written by hand. Correct everywhere, efficient on X11.

### Folding in sockets and workers

A window's descriptor is an ordinary fd, so it composes with everything else
that has one. `Reactor` (`vyto/os/reactor`) can own the loop and treat windows,
sockets, timers and forked workers uniformly:

```js
rx.watch(a.eventFd(), POLL_READ, (fd, ev) => { /* drain a.poll() */ return true; });
rx.watch(b.eventFd(), POLL_READ, (fd, ev) => { /* drain b.poll() */ return true; });
for (let wfd in pool.workerFds()) { rx.watch(wfd, POLL_READ, ...); }
rx.run();
```

That is how a control window can hand work to a separate process and paint the
result into a second window. See `examples/107_reactor.vt` for the
socket/timer/worker half.

> **Reactor caveat:** `rx.tick()` returns immediately unless the loop is
> running, so driving it from your own window loop silently does nothing — call
> `rx.run()` and let it own the loop. On backends with no descriptor (Win32,
> Android) invert it with `rx.set_waiter(...)` instead.

## 4. Windows and monitors

```js
let ms = surface_monitors();          // never empty; exactly one is primary
for (let m in ms) {
    print(m.w + "x" + m.h + " at " + m.x + "," + m.y + " scale " + m.scale);
}
```

Monitors are reported in the **same coordinate space `move_to` uses**, so
placing a window on a particular screen is geometry rather than a screen index:

```js
let m = ms[ms.len - 1];               // the last screen
s.set_size(m.w, m.h);
s.move_to(m.x, m.y);
s.set_fullscreen(true);               // fullscreens where the window IS
```

`set_fullscreen` returns `false` where there is no window manager to ask, so a
headless or embedded target degrades honestly instead of pretending.

| Call | Notes |
|---|---|
| `surface_monitors(): Monitor[]` | Never empty — a backend that cannot enumerate reports the one screen it can describe |
| `s.position(): int[]` | `[x, y]` of the top-left corner, in screen coordinates |
| `s.move_to(x, y)` | Move the top-left corner |
| `s.set_size(w, h)` | Resize |
| `s.set_min_size(w, h)` | Floor the WM will honour while dragging |
| `s.set_fullscreen(on): bool` | Targets the monitor the window is currently on |

### DPI

`s.scale()` gives the UI scale of the monitor the window is on (1.0 = 96dpi).
`Monitor.scale` gives the same per entry.

**Per-monitor scale is only real on Windows.** X11 has one global `Xft.dpi` and
no per-output equivalent — per-monitor scaling on Linux desktops is a compositor
feature implemented outside X — so every X11 window reports the desktop's scale.
Physical DPI from XRandR is deliberately *not* used as a substitute: a laptop
panel measuring 142dpi is routinely run at 96, and treating physics as the scale
oversizes everything by ~48%.

`$VYTO_SCALE` overrides it, and takes either one value (`1.5`, `150`) or a comma
list naming monitors in order (`100,200`) — the list form exists to test
mixed-DPI behaviour on a single-screen machine.

> **Scale is read once**, at `Window` construction, and applied once in `run()`.
> Dragging a window to a different-DPI monitor does not re-scale it. That needs
> a Theme rebuild path that does not exist yet.

## 5. Chromeless windows

Drop the system titlebar and border for a kiosk, a custom-chrome app, or a
floating panel:

```js
s.set_decorated(false);
```

**An undecorated window must supply what the decorations did.** There is no
titlebar to grab, no border to pull, and no [x] — all of which were the window
manager's. Three consequences, and forgetting any one of them is the usual bug:

**Dragging.** Hand the drag back to the window manager from your own title area:

```js
if (ev == EV_MOUSE_DOWN && s.mouse_y() < BAR_H) {
    s.drag_start(EDGE.MOVE, s.mouse_x(), s.mouse_y());
}
```

Prefer this over moving the window yourself on each motion event: the WM applies
its own snapping, edge-tiling and multi-monitor rules, which a hand-rolled drag
does not get. `EDGE` also has the eight resize edges (`TOPLEFT` … `LEFT`), so a
few pixels of rim can drive resizing. Returns `false` when no WM took it — then
fall back to `move_to` on `EV_MOUSE_MOVE`.

**Repainting.** A chromeless window **owns every pixel**. The server paints no
background, so an unhandled `EV_EXPOSE` leaves the damaged area showing whatever
was behind the window — it looks *semi-transparent*, which reads as a rendering
bug rather than a missing handler:

```js
else if (e == EV_EXPOSE || e == EV_RESIZE) { draw(s); }
```

**Closing.** There is no [x]. A fullscreen chromeless window with no exit path
means killing the process — on a kiosk, a reboot. Give it a key or a hit region.

## 6. Previewing one window inside another

For a control room or a video wall — thumbnails of each display inside the
controller — do **not** capture the screen. If your process renders the window,
you already have its pixels: render once into a `Canvas` and blit it twice.

```js
// full size, into its own window
disp.blitPtr(canvas.pixels(), w, h, Rect(0.0, 0.0, w as float, h as float));
// and scaled into a panel on the controller
ctl.blitPtr(canvas.pixels(), w, h, Rect(16.0, 62.0, 200.0, 120.0));
```

`blitPtr` scales to whatever destination rect it is given, so a thumbnail costs
one extra blit and no extra rendering.

> **Pick canvas widths that are a multiple of 4.** blend2d row-aligns its
> buffer, and `blitPtr` requires it tightly packed (stride == `w*4`). 1920 and
> 1280 are packed; **1366 pads to a stride of 1368 and the preview shears** —
> a plausible-looking bug that only appears at certain resolutions. `blitRect`
> respects stride but does not scale, so it is not a substitute here.

Each preview is a scaled full-frame CPU copy, so redraw them when content
changes rather than every frame. A monitoring thumbnail does not need 60fps.

### Capturing a window you did *not* render

```js
let c = capture_window(xid as culong);      // XID on X11, HWND on Windows
if (c != null) { ctl.blit(c.pixels, c.w, c.h, panel); }
```

`Capture.pixels` is `0x00RRGGBB` row-major, the layout `blit` already takes.

**A null return is the ordinary answer, not an error.** It is what Wayland gives
(capture there is behind a portal with user consent), what Android gives, and
what any unmapped window gives. Write the null path first.

On X11 without a compositing manager, an obscured region reads back as whatever
is drawn over it, because the server keeps no offscreen copy. Most desktops
composite, so this is usually correct — but it cannot be detected from here.

## 7. Using `vyto/ui` on a second window

`vyto/ui`'s `Window` binds to exactly one `Surface` and drives its own `run()`
loop, which owns the thread. **There is no supported way today to run the
toolkit on two windows at once.**

Practical shapes:

- **Toolkit on the control window, raw `Surface` on the display.** A signage or
  preview window needs pixels, not widgets — this is what
  `apps/multiwindow/kiosk.vt` does, and it is the recommended split.
- **Raw `Surface` on both**, driving your own loop, as in every example here.

## 8. Memory

Measured on the examples in `apps/multiwindow/`, RSS shortly after launch:

| Shape | RSS | Binary |
|---|---|---|
| Two lean-tier windows | ~5.9 MB | ~79 KB |
| Lean + one small `Canvas` | ~9.7 MB | ~99 KB |
| Three 480×280 canvases | ~12.5 MB | ~108 KB |
| One 1920×1080 canvas | ~24.7 MB | ~100 KB |

**Memory scales with pixels, not with windows.** A 1920×1080 ARGB canvas is
8.3 MB by itself, which is why the single-fullscreen case costs more than three
small ones. Adding screens is cheap; raising resolution is what costs.

## 9. API reference

Everything below is `vyto/surface`. Only the multi-window-relevant members are
listed — drawing, text and clipboard are covered in
[`graphics.md`](graphics.md) and [`events.md`](events.md).

### Creating and closing

```js
new Surface(title: string, w: int, h: int): Surface
```

There is no `close()`. A window closes when its `Surface` is released —
assigning `null` over the last reference is how you close one deliberately.

### Geometry and placement

```js
s.width(): int
s.height(): int
s.position(): int[]                 // [x, y] of the top-left, in screen coords
s.move_to(x: int, y: int)
s.set_size(w: int, h: int)
s.set_min_size(w: int, h: int)      // floor the WM honours while dragging
s.set_title(t: string)
s.set_fullscreen(on: bool): bool    // false where there is no WM to ask
```

`set_fullscreen` targets the monitor the window is **currently on**, so choose a
screen by moving there first.

### Monitors

```js
surface_monitors(): Monitor[]       // never empty; exactly one is primary

class Monitor {
    x: int; y: int; w: int; h: int;
    primary: bool;
    scale: float;                   // 1.0 = 96dpi
}

s.scale(): float                    // scale of the monitor this window is on
```

Monitor geometry is in the same coordinate space `move_to` uses.

### Decorations and dragging

```js
s.set_decorated(on: bool)
s.drag_start(edge: EDGE, x: int, y: int): bool

enum EDGE {
    TOPLEFT = 0, TOP = 1, TOPRIGHT = 2, RIGHT = 3,
    BOTTOMRIGHT = 4, BOTTOM = 5, BOTTOMLEFT = 6, LEFT = 7,
    MOVE = 8,
}
```

`drag_start` returns `false` when no window manager took the drag; fall back to
`move_to` on `EV_MOUSE_MOVE`. Values match `_NET_WM_MOVERESIZE`.

### The event loop

```js
s.wait(): int                       // blocks until an event
s.poll(): int                       // EV_NONE when nothing is queued
s.wait_timeout(ms: int): int        // EV_TIMER if ms elapses
s.waitTimeoutFds(ms: int, fds: int[]): int
s.eventFd(): int                    // -1 where the backend has no descriptor
s.eventsPending(): int
s.canWaitFds(): bool                // true on X11; false elsewhere
```

`wait`, `wait_timeout` and `waitTimeoutFds` **return** the event — they do not
leave it queued. See §3.

Event constants: `EV_NONE`, `EV_EXPOSE`, `EV_KEY`, `EV_MOUSE_DOWN`,
`EV_MOUSE_UP`, `EV_RESIZE`, `EV_CLOSE`, `EV_MOUSE_MOVE`, `EV_TIMER`,
`EV_KEY_UP`, `EV_MOUSE_WHEEL`, `EV_MOUSE_RDOWN`, `EV_VSYNC` (0–12), and the
`EVENT` enum with the same members for an exhaustive `switch`.

### Event payload

Read after the event that produced it — each names *this* window's last event:

```js
s.key(): int
s.text_ev(): string
s.mouse_x(): int
s.mouse_y(): int
s.wheel(): int
s.mods(): int
s.damage(): Rect        // region the last EV_EXPOSE lost; zero-sized = repaint all
```

### Presenting pixels

```js
s.blit(pixels: i32[], srcw: int, srch: int, dst: Rect)
s.blitPtr(p: rawptr, srcw: int, srch: int, dst: Rect)
s.blitRect(p: rawptr, stridePx: int, srcx: int, srcy: int,
           w: int, h: int, dstx: int, dsty: int)
s.present()
s.present_rect(r: Rect)
```

`blit` and `blitPtr` **scale** to the destination rect — that is what makes a
preview thumbnail one call. Both require tightly packed pixels
(stride == `srcw*4`); `blitRect` takes an explicit stride but does **not**
scale. Pixels are `0x00RRGGBB`, row-major.

### Capturing another window

```js
capture_window(native_win: culong): Capture      // null when unreadable

class Capture {
    w: int; h: int;
    pixels: i32[];                  // 0x00RRGGBB, row-major, w*h
}

s.native_window(): culong           // this window's XID / HWND
```

### Escape hatches

```js
s.raw_handle(): rawptr
s.native_display(): rawptr          // X11 Display*
s.native_gc(): rawptr
```

For reaching past the API — `set_decorated` was originally done this way, with
`_MOTIF_WM_HINTS` set directly on `native_window()`.

## 10. Worked examples

All in [`apps/multiwindow/`](../apps/multiwindow/); each runs on its own and
needs a display.

| File | Pattern |
|---|---|
| `queue.vt` | Hospital queue: a console advances a counter, a second window shows it in large type |
| `inspector.vt` | Master/detail — selection in one window, detail in another |
| `spawn.vt` | Windows opened and closed on demand from an event handler |
| `hud.vt` | Chromeless floating panel with its own titlebar and drag |
| `kiosk.vt` | Fullscreen signage picking its monitor, plus a control window |
| `videowall.vt` | Three screens, each previewed live inside the controller |

Smaller, single-purpose demos: `examples/113_multiwindow.vt` (two windows, input
routing) and `examples/114_chromeless.vt` (decorations, drag, resize edges).

## 11. What is not supported

- **Running `vyto/ui` on more than one window** — see §7.
- **Live re-scaling** when a window moves to a different-DPI monitor (§4).
- **Child, embedded or modal windows.** Every window is a peer.
- **Drag-and-drop between windows.**
- **Wayland.** X11 covers Linux today; a Wayland backend is separate work, and
  `capture_window` already returns null there.
- **Placing a window on a monitor before it is mapped.** `move_to` after
  creation is the way, which can show a brief jump at startup.
