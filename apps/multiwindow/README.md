# Multi-window examples

Four patterns for driving more than one OS window from one Vyto process. Each
runs on its own:

```sh
./vytoc run apps/multiwindow/queue.vt        # two-screen hospital queue display
./vytoc run apps/multiwindow/inspector.vt    # master / detail across two windows
./vytoc run apps/multiwindow/spawn.vt        # windows opened and closed on demand
./vytoc run apps/multiwindow/hud.vt          # chromeless floating panel
./vytoc run apps/multiwindow/kiosk.vt        # fullscreen signage + control window
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

## Not covered

Per-monitor DPI: `Monitor.scale` currently reports the global scale rather than
the scale of that particular screen, so a mixed-DPI multi-head setup will size
text by the wrong factor on one of them.
