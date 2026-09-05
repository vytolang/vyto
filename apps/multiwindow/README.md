# Multi-window examples

Four patterns for driving more than one OS window from one Vyto process. Each
runs on its own:

```sh
./vytoc run apps/multiwindow/queue.vt        # two-screen hospital queue display
./vytoc run apps/multiwindow/inspector.vt    # master / detail across two windows
./vytoc run apps/multiwindow/spawn.vt        # windows opened and closed on demand
./vytoc run apps/multiwindow/hud.vt          # chromeless floating panel
./vytoc run apps/multiwindow/kiosk.vt        # fullscreen signage + control window
./vytoc run apps/multiwindow/videowall.vt    # control room with live preview panels
```

They need a display (X11 today). There are no `.expected` files: these are
demos, and the deterministic checks live in `tests/ui/96_multiwindow` and
`tests/ui/97_window_controls`.

## What each one shows

| File | Pattern |
|---|---|
| `queue.vt` | A console window advances a counter; a second window shows it in large type for a waiting room. Uses the rich tier for the big digits. |
| `inspector.vt` | Selection in one window, detail in another — an inspector or preview pane the user can park on a second monitor. |
| `spawn.vt` | `new Surface(...)` inside an event handler. Each note is a peer with its own input, closable on its own. |
| `hud.vt` | An undecorated always-there panel that draws its own titlebar and hands dragging back to the window manager. |
| `kiosk.vt` | Fullscreen signage on one screen, a control window on another. Picks its monitor from `surface_monitors()`. |
| `videowall.vt` | Three signage screens, each shown live as a thumbnail inside the controller. |

## The three things worth knowing

**Sharing data between windows is not a thing you have to arrange.** They are
objects on one thread in one process, so a counter is a variable and both
windows draw from it. There is no IPC, no serialization, and nothing to keep in
sync. Every example here is built that way.

**Wait on all of them at once, or the app burns a core.** Two windows are two X
connections, hence two descriptors:

```vyto
let ev = a.waitTimeoutFds(40, [b.eventFd()]);
```

That blocks until *either* speaks. Polling both in a bare loop spins.
`waitTimeoutFds` **returns** the woken window's event rather than leaving it
queued — discard the return value and that window's input silently disappears
while the other keeps working.

**A chromeless window owns every pixel.** With decorations off the server paints
no background, so an unhandled `EV_EXPOSE` leaves the damaged area showing
whatever was behind the window. `hud.vt` handles expose for exactly this
reason. It also has to supply its own close affordance and call `drag_start`,
since the titlebar it gave up is where those lived.

## Putting a window on a chosen screen

`surface_monitors()` reports every attached display in the same coordinate
space `move_to` uses, so placement is geometry rather than a screen index:

```vyto
let ms = surface_monitors();          // never empty; one entry is primary
let m = ms[ms.len - 1];               // the last screen, say
s.set_decorated(false);
s.set_size(m.w, m.h);
s.move_to(m.x, m.y);
s.set_fullscreen(true);               // fullscreens where the window IS
```

`set_fullscreen` returns false where there is no window manager to ask, so a
headless or embedded target degrades honestly instead of pretending.

**A fullscreen chromeless window must offer its own way out.** There is no [x]
and no titlebar; `kiosk.vt` quits on ESC from either window. Skip that on a real
kiosk and the only exit is killing the process.

## Previewing a screen inside the controller

`videowall.vt` shows each display live inside the control window. It is not a
screen capture and not an IPC channel — each screen renders into a blend2d
`Canvas` this process owns, and that one buffer is blitted twice:

```vyto
// full size, into its own window
dst.blitPtr(canvas.pixels(), w, h, Rect(0.0, 0.0, w as float, h as float));
// and scaled into a panel on the controller
ctl.blitPtr(canvas.pixels(), w, h, Rect(16.0, 62.0, 200.0, 120.0));
```

`blitPtr` scales to whatever destination rect it is given, so a thumbnail costs
one extra blit and no extra rendering. Render once, present to both.

**Pick canvas widths that are a multiple of 4.** blend2d row-aligns its buffer,
and `blitPtr` requires it tightly packed (stride == w*4). 1920 and 1280 are
packed; 1366 pads to a stride of 1368 and the preview would shear. Use
`blitRect` where a stride must be respected — but note it does not scale.

Each preview is a scaled full-frame CPU copy, so redraw them when content
changes rather than every frame. A monitoring thumbnail does not need 60fps.

This only works for windows **this process renders** — and it is the right way
to do it, because the canvas is already there.

For a window you did **not** draw, `capture_window(native_xid)` reads its pixels
back into a `Capture` whose `pixels` array blits like any other:

```vyto
let c = capture_window(xid as culong);
if (c != null) { ctl.blit(c.pixels, c.w, c.h, panel); }
```

It returns null when the window cannot be read, and **that is the ordinary
answer, not an error** — it is what Wayland gives (capture there is behind a
portal with user consent), what Android gives, and what any unmapped window
gives. Code the null path first. X11 and Win32 are the platforms where it
works.

One X11 caveat it cannot detect for you: without a compositing manager, an
obscured region reads back as whatever is drawn over it, because the server
keeps no offscreen copy. Most desktops composite, so it is usually right.

## Not covered

Per-monitor DPI: `Monitor.scale` currently reports the global scale rather than
the scale of that particular screen, so a mixed-DPI multi-head setup will size
text by the wrong factor on one of them.
