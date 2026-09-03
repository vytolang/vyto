# Events & input in Vyto

Keyboard, mouse, focus and gestures — how input reaches your code.

## 1. Two layers, pick one

```
vyto/surface          Layer 0: a classified event queue. You write the loop.
     │                          Games, emulators, custom renderers.
     ▼
vyto/ui Widget        Layer 1: the loop is Window.run(). You override
                                on_key / on_click / on_drag / …
```

They are the *same* events — `vyto/ui` runs a `vyto/surface` loop internally
and dispatches to widgets. Use Layer 0 when you own the frame clock; use
Layer 1 when you want focus traversal, hover, double-click, drag and
long-press already solved.

A third, unrelated layer exists for raw hardware: `vyto/hw/input` reads evdev
devices (`/dev/input/eventN`) through a `PollSet`. That is for reading a
gamepad or a barcode scanner as a *device*, not for driving a window — see
`docs/hardware.md` and `examples/54_input.vt`.

## 2. Layer 0 — `vyto/surface`

An event is an `int` class returned by the wait call. The payload is fetched
from accessors afterwards, so there is no event struct to allocate.

```js
import { Surface, EV_TIMER, EV_KEY, EV_CLOSE, EV_EXPOSE,
         KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_ESC } from "vyto/surface";

fn main() {
    let s = new Surface("Snake", 640, 480);
    let running = true;
    while (running) {
        let ev = s.wait_timeout(120);          // ms; EV_TIMER when it elapses
        if (ev == EV_TIMER) {
            tick();
            draw(s);
            s.present();
        } else if (ev == EV_KEY) {
            let k = s.key();
            if (k == KEY_ESC) { running = false; }
            else if (k == KEY_UP) { setDir(0, -1); }
        } else if (ev == EV_CLOSE) {
            running = false;
        } else if (ev == EV_EXPOSE) {
            draw(s);
            s.present();
        }
    }
}
```

That is `apps/snake/snake.vt` in miniature.

### Waiting

| Call | Behaviour |
|---|---|
| `s.wait()` | block until an event arrives |
| `s.poll()` | return `EV_NONE` immediately if nothing is queued |
| `s.wait_timeout(ms)` | block up to `ms`, then deliver `EV_TIMER` |
| `s.waitTimeoutFds(ms, fds)` | as above, also waking on your own file descriptors |

`wait_timeout` is how a game gets a frame clock: input returns early, the
timeout is the tick.

### Event classes

| Constant | Meaning | Read it with |
|---|---|---|
| `EV_NONE` | nothing queued (from `poll()`) | — |
| `EV_EXPOSE` | the window needs repainting | redraw + `present()` |
| `EV_KEY` | a key went down | `key()`, `text_ev()`, `mods()` |
| `EV_KEY_UP` | a key came up | `key()`, `mods()` |
| `EV_MOUSE_DOWN` | left button pressed | `mouse_x()`, `mouse_y()`, `mods()` |
| `EV_MOUSE_UP` | left button released | `mouse_x()`, `mouse_y()` |
| `EV_MOUSE_RDOWN` | **right** button pressed | `mouse_x()`, `mouse_y()` |
| `EV_MOUSE_MOVE` | cursor moved (the hover source) | `mouse_x()`, `mouse_y()` |
| `EV_MOUSE_WHEEL` | wheel scrolled | `wheel()` — see below |
| `EV_RESIZE` | window resized | `width()`, `height()` |
| `EV_TIMER` | `wait_timeout` elapsed with no input | — |
| `EV_CLOSE` | the user closed the window | exit your loop |
| `EV_VSYNC` | the display is about to scan out; present now | see `set_vsync` |

There is **no `EV_MOUSE_RUP`** — the backends do not deliver a right-button
release, so a right-click is a press with no pair.

### Accessors

Payload belongs to the **last delivered event**; read it before calling wait
again.

```js
s.key()          // int: ASCII code, or a KEY_* constant
s.text_ev()      // string: the text this keypress inserts ("" for e.g. arrows)
s.mouse_x()      // int
s.mouse_y()      // int
s.wheel()        // int: scroll delta
s.mods()         // int: MOD_* bitmask at the time of the event
s.now_ms()       // int: monotonic ms — the animation/game clock, not wall time
s.scale()        // float: 1.0 = 96dpi, 2.0 = HiDPI
s.clipboard()    // string, UTF-8; "" when empty or unavailable
s.set_clipboard(t)
```

`text_ev()` is the one with a surprising name, and it is the one you want for
text entry: `key()` gives you a *code*, `text_ev()` gives you the *characters*
to insert, which is what handles multibyte input. A non-ASCII keystroke arrives
with `key() == 0` and the UTF-8 sequence in `text_ev()`.

`wheel()` is **negative for up, positive for down** — the sign follows the
*content*, not the finger. One notch is `±3`, not `±1`: the backend maps a
wheel click to three lines (X11 `Button4`/`Button5`, and Windows normalizes its
120-unit `WHEEL_DELTA` to the same). Treat it as a delta to accumulate, not as a
count of notches.

### Key codes

Printable keys are plain ASCII — compare against `65` for `A`, or use
`KEY_SPACE` (which is just `32`, named for readability). Everything else is a
constant above `1000`:

```
KEY_ENTER 1000   KEY_BACKSPACE    KEY_ESC       KEY_UP     KEY_DOWN
KEY_LEFT         KEY_RIGHT        KEY_DELETE    KEY_TAB    KEY_HOME
KEY_END          KEY_PAGEUP       KEY_PAGEDOWN  KEY_INSERT
KEY_SHIFT        KEY_CTRL         KEY_ALT       KEY_SUPER
KEY_F1 1021 … KEY_F12 1032
```

Modifier keys arrive as their **own** `EV_KEY`/`EV_KEY_UP` events, which is
what held-key game input needs. For "was Ctrl held during *this* event", do not
track those — use the mask:

```js
if ((s.mods() & MOD_CTRL) != 0 && s.key() == 99) { copy(); }   // Ctrl+C
```

`MOD_SHIFT` = 1, `MOD_CTRL` = 2, `MOD_ALT` = 4, `MOD_SUPER` = 8.

## 3. Layer 1 — `vyto/ui` widgets

Override a virtual. **Return `true` when you consumed the event or changed
something that needs a redraw**; return `false` to let it keep travelling. That
return value is the whole dispatch protocol.

```js
class Key extends Widget {
    override fn on_key(k: int, ch: string): bool {
        if (k == KEY_ENTER) { this.commit(); return true; }
        if (ch.len > 0) { this.insert(ch); return true; }
        return false;                       // not ours — let it bubble
    }
}
```

### The virtuals

| Override | Fires when |
|---|---|
| `on_click(x, y)` | left button pressed **down** on this widget |
| `on_release(x, y)` | released over it, having held the press capture |
| `on_double_click(x, y)` | second press within `DOUBLE_CLICK_MS`, inside the slop |
| `on_long_press(x, y)` | press held past `LONG_PRESS_MS` without moving |
| `on_context(x, y)` | right button pressed |
| `on_drag_start(x, y)` | a press first moves past `DRAG_SLOP` |
| `on_drag(dx, dy)` | every move while holding capture — **deltas**, not positions |
| `on_drag_end(x, y)` | release after a drag |
| `on_scroll(delta)` | wheel over this widget; +down, −up |
| `on_key(k, ch)` | key down while focused; `ch` is the insertable text |
| `on_key_up(k)` | key released while focused |
| `on_hover_enter()` / `on_hover_leave()` | cursor entered / left the bounds |
| `on_focus()` / `on_blur()` | gained / lost keyboard focus |
| `on_resize(w, h)` | the window resized |

All coordinates are **absolute** (window space), not widget-relative — compare
against `this.bounds`. `on_drag` is the exception: it takes deltas since the
last move.

Gesture thresholds are constants you can read: `DOUBLE_CLICK_MS` (400),
`LONG_PRESS_MS` (500), `DRAG_SLOP` (4 px).

`on_click` fires on press-*down*. If you want the click-completed semantics a
button has, use `on_release`, which only fires if the cursor is still inside
the widget.

### Focus and typing

A widget takes focus only if it sets `focusable = true` in `init`. Tab moves
focus (`focus_next` / `focus_prev`), and `Window.focus_widget(w)` sets it
directly. Two virtuals steer the traversal:

```js
virtual fn wants_tab(): bool    // false: Tab moves focus past this widget
virtual fn wants_text(): bool   // false: focusing this does not mean "type here"
```

Leave `wants_tab` false unless the widget *is* a set of fields the user walks
through (DataTable's filter row is one box per column) — a widget that swallows
Tab traps the keyboard.

`wants_text` is nothing on a desktop and everything on a phone: it is the only
signal that raises the soft keyboard, because a touch platform must be *told*.
Set it true for anything text-entry.

### Hit testing

`hit(px, py)` returns the deepest widget at a point and is what the dispatcher
walks. Override it to make a widget claim a larger or smaller area than its
bounds; `Rect.has(px, py)` does the standard test.

## 4. Scripted events — testing without a human

Set `VS_HEADLESS=1` and point `VS_EVENTS` at a script; the surface backend
replays it as real events. This is how every UI golden test runs:

```sh
VS_HEADLESS=1 VS_EVENTS=tests/ui/73_dtedit.events ./vytoc run tests/ui/73_dtedit.vt
```

One directive per line, `#` starts a comment, EOF acts as `close`:

```
type hello world     each character becomes a key event (UTF-8 aware)
key Enter            Enter Backspace Esc Up Down Left Right Delete Tab Home End
keyup Enter          same names, delivered as a release
click X Y            mouse down, then up, at X,Y
down X Y             down with no paired up — for drag-hold tests
up X Y               the matching release
rclick X Y           right button (no paired up — matches the real backends)
move X Y             cursor motion — the hover source
scroll N [X Y]       wheel; N is the delta (one real notch is ±3, +down)
resize W H
expose
tick                 a timer tick, for game-loop tests
mods ctrl+shift      modifier state for all following events ("none" clears)
clip some text       seed the clipboard — the paste-test hook
close
```

`mods` and `clip` emit no event of their own; they set state the following
directives read. Each delivered event advances the synthetic clock 16 ms, so
`now_ms()` behaves like one frame per event.

## 5. Android — touch, Back, and the IME

Android delivers the **same event classes**. A finger is translated into the
mouse events above, so a widget written for the desktop works on a phone with
no touch-specific code. The translation lives in
`lib/vyto/surface/native/src/vsurf_android.c`, and the differences below are
the ones that change how you write a widget.

### Touch is the mouse

| Android `MotionEvent` | Vyto event |
|---|---|
| `ACTION_DOWN`, `ACTION_POINTER_DOWN` | `EV_MOUSE_DOWN` |
| `ACTION_UP`, `ACTION_POINTER_UP` | `EV_MOUSE_UP` |
| `ACTION_CANCEL` | `EV_MOUSE_UP` |
| `ACTION_MOVE` | `EV_MOUSE_MOVE` |

So `on_click`, `on_drag`, `on_long_press` and the rest fire from a finger
exactly as from a pointer, and the gesture thresholds (`DRAG_SLOP`,
`LONG_PRESS_MS`) are the same numbers.

Three consequences worth knowing:

**A cancel arrives as an up.** Vyto has no cancel concept, so the distinction
is lost. This is wrong in principle — a cancelled gesture should not fire a
click — but a widget stuck in the pressed state is the worse failure.

**Moves are coalesced.** Consecutive `EV_MOUSE_MOVE` events collapse into the
latest position, the same as the X11 arm does with `MotionNotify`. Each
delivered move drives a hit test and a repaint, so replaying every historical
sample would cost N layout passes for one frame of finger travel. Velocity for
drag-scroll and fling is measured as distance over elapsed time *between
delivered moves*, so coalescing preserves it — it would only break if you
accumulated velocity per event.

**A lifted finger synthesises a move to `(-1, -1)`.** There is no cursor left
hovering where a finger was, but the toolkit's hover state is maintained by
mouse-move and `on_mouse_up` deliberately clears the press and not the hover
(on a desktop the pointer really is still there). X11 sends `LeaveNotify` for
this; Android does not, so the platform arm queues the synthetic move *after*
the up — the widget still sees its release while hovered, then `(-1,-1)`
hit-tests to nothing and hover clears. Without it every hover-tinted control
stays lit after being tapped.

`AndroidWindow` also sets `touch_mode = true`, which reaches plain `vyto/ui`
widgets, and hides scrollbars.

**Single pointer only.** Pointer ids other than 0 are dropped. The event model
cannot express a second: an event is an `int` plus argument-less accessors, so
there is nowhere to put a pointer id. Multitouch needs the per-surface event
struct scoped to the multiwindow work.

### The Back key is Escape

There is no `KEY_BACK`. The system Back key is pushed into the queue as
`KEY_ESC` (down then up), and `Window.on_key_ev` gives `on_back()` first
refusal before Escape means anything else:

```js
override fn on_back(): bool { return this.pop_screen(); }   // AndroidWindow
```

`AndroidWindow` maintains a screen stack — `set_content` (base), `push_screen`,
`pop_screen` — and `on_back` pops it. Returning `false` at the base is what lets
the Activity finish, i.e. what makes Back exit the app.

The mechanism underneath is worth understanding, because it explains a setting
you will otherwise trip over. `Native.back()` must answer the UI thread
synchronously — "did the app consume that press?" — but only the Vyto thread
knows, and it is off in the event loop. So Vyto **publishes the answer in
advance**: `nav_changed()` writes the current depth (screens above the base,
plus one for an open overlay) whenever navigation changes, and the UI thread
reads that integer. Toasts are deliberately not counted; no platform pops a
snackbar with Back.

That published depth can be one frame stale — Back pressed within a frame of
the tap that changed it. Which is why `AndroidWindow` sets:

```js
this.close_on_esc = false;
```

A stale-high depth then synthesises an Escape that falls quietly on the floor
instead of unexpectedly quitting the app. Leave that alone.

### The soft keyboard follows focus

`wants_text()` is the entire mechanism. On a touch platform nothing else can
raise the IME — there is no physical keyboard to just start typing on — so
`AndroidWindow.focus_changed()` shows the keyboard when the newly focused
widget wants text and hides it otherwise:

```js
override fn focus_changed() {
    let w = this.focused_widget();
    let want = 0;
    if (w != null && w.wants_text()) { want = 1; }
    if (want == this.ime_on) { return; }
    this.ime_on = want;
    vta_ime_set(want as i32);
}
```

Hiding when focus moves to a non-text widget is deliberate: a tap on a list
behind the keyboard means "I am done typing", and that tap moves focus.

**A custom text widget that does not override `wants_text()` is decorative on a
device.** It will take focus, draw a caret, and never see a keystroke.

Committed IME text arrives as a key event with **keycode 0 and the text
payload** — the same channel XIM uses on the desktop, which is why `TextField`
already consumes it. This is the multibyte rule from §2 again: read `ch`, not
`k`.

### Insets, rotation and resize

There is no `EV_INSETS`; the event enum is a wire contract shared with three
other backends. An inset change is announced as **`EV_RESIZE`**, which is
already "re-run layout and redraw" — exactly what an inset change needs. Only a
real change pushes one, so the repeated inset callbacks Android issues during a
traversal do not each cost a relayout.

Layout code *reads* insets rather than being told: `vs_android_get_insets`
samples them during layout, which is when they matter. That is what `SafeArea`
in `vyto/mobile/android/ui` does, and why rotation needs no invalidation
protocol — every arrange re-reads.

This ordering was a real bug: `onApplyWindowInsets` fires on the UI thread
*after* the Vyto thread has run its first and only layout, so without the
`EV_RESIZE` a safe-area widget samples zeroes and nothing ever asks again. The
symptom was labels rendering behind the status bar while the insets were being
delivered perfectly correctly.

### Frame pacing

`EV_VSYNC` is Android's, driven by Choreographer — the only clock that is
actually the display's. At most one is ever queued: two would present the same
frame twice, and when the Vyto thread is behind, it is the *older* one that is
stale. Gating follows `animations.len > 0`, so an idle app blocks in `wait()`
with no periodic wakeup rather than burning a 60 Hz timer. Desktop backends
return false from `set_vsync` and present eagerly.

## 6. Traps

**Read payload before waiting again.** `key()`, `mouse_x()` and friends describe
the last delivered event. The next `wait()` overwrites them.

**`text_ev()`, not `key()`, for text.** `key()` is a code and is `0` for
multibyte input. Building a string from key codes breaks on the first accented
character.

**Returning `false` from a handler that changed something.** The return value is
what schedules the redraw. Change state, return `true`.

**A looping `Tween` repaints every frame.** Never use one as a passive timer and
never re-arm one from `render()`. For periodic work that is not animation, use
the timeout in your own loop.

**`Widget.ticker(ms, …)` fires its callback every frame** — `period_ms` is the
tween *duration*, not an interval. Misreading it produces a benchmark that
measures your own timer.

**Right-click has no release event.** Do not wait for one.

**On Android, a custom text widget without `wants_text()` never sees a key.**
It takes focus and draws a caret, and the keyboard never comes up.

**Do not set `close_on_esc = true` on an `AndroidWindow`.** Escape is the wire
the system Back key rides on, and the depth the UI thread reads can be one
frame stale; the stale case is only harmless while a spurious Escape is a
no-op.

**Do not count on a touch cancel.** `ACTION_CANCEL` reaches you as an ordinary
`EV_MOUSE_UP`, so a cancelled gesture completes a click.

## 7. Where to look

| For | See |
|---|---|
| a raw event loop | `apps/snake/snake.vt`, `apps/chip8/chip8.vt` |
| widget event handling | `lib/vyto/ui/form.vt` (TextField, TextArea) |
| the dispatcher itself | `lib/vyto/ui/core.vt`, `Window.run` |
| the constants | `lib/vyto/surface/surface.vt` |
| scripted-event tests | `tests/ui/*.events` |
| raw hardware devices | `docs/hardware.md`, `examples/54_input.vt` |
| Android event translation | `lib/vyto/surface/native/src/vsurf_android.c` |
| Android window, Back, IME | `lib/vyto/mobile/android/ui.vt` (`AndroidWindow`) |
| the Back-depth publish | `lib/vyto/mobile/android/native/src/aback.c` |
