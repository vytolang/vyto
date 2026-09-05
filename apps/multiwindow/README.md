# Multi-window examples

Four patterns for driving more than one OS window from one Vyto process. Each
runs on its own:

```sh
./vytoc run apps/multiwindow/queue.vt        # two-screen hospital queue display
./vytoc run apps/multiwindow/inspector.vt    # master / detail across two windows
./vytoc run apps/multiwindow/spawn.vt        # windows opened and closed on demand
./vytoc run apps/multiwindow/hud.vt          # chromeless floating panel
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

## Not covered

Placing a window on a chosen monitor. `vyto/surface` has no monitor
enumeration yet (MULTIWINDOW.md Part 2), so a second-screen display like
`queue.vt` is dragged there by hand.
