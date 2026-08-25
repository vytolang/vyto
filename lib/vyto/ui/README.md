# vyto/ui — the Vyto UI Toolkit

Retained widgets, two-pass layout, focus routing, and a `Window` that owns
the event loop — the largest package in the tree by a wide margin (over
16,000 lines across 17 modules plus four bundled skins). This README is the
map: what each module gives you, the concepts that hold the whole thing
together, and the gotchas that aren't obvious from reading any one file.

```vyto
import { Window, Column, Row, Label, Button, TextField } from "vyto/ui";

fn main() {
    let win = new Window("My first app", 360, 220);
    win.root = new Column([
        new Label("Hello from vyto/ui"),
        new Row([new TextField("type here"), new Button("Add")]),
        new Button("OK"),
    ]);
    win.run();
}
```

See [`docs/getting-started.md`](../../../docs/getting-started.md) §5 for the
five-minute walkthrough and [`examples/05_widgets.vt`](../../../examples/05_widgets.vt)
for interactivity (click handlers, state).

## Modules

`import { … } from "vyto/ui"` is a barrel (`ui.vt`) re-exporting all of this
from one place; importing a submodule directly (`vyto/ui/form`, say) pulls in
less.

| Module | What it gives you |
|---|---|
| `ui/core` | `Size`, `BoxConstraints`, `Theme`, `Painter`/`SurfacePainter`, the `Widget` base class, `Window`, the pluggable-shape `Chrome` classes + `Skin` |
| `ui/anim` | `Tween`/`Animator`/easing — the animation clock |
| `ui/scrollbar` | `Scrollbar` — edge-snapping, geometry-only, never forms a ref cycle with its owner |
| `ui/layout` | `Column`, `Row`, `Spacer`, `Padding`, `Center`, `Container`, `Align`, `Stack`/`Positioned`, `Wrap`, `Grid`, `Flexible`, `Divider`, `ScrollView`, `ListView` |
| `ui/form` | `Label`, `Button`, `TextField`, `SearchField`, `Checkbox`, `ListBox`, `TextArea`, `RadioButton`/`RadioGroup`, `Switch`, `SegmentedControl`, `Stepper`, `Dropdown`, `ProgressBar`, `Slider` |
| `ui/menu` | `MenuItem`, `Menu`, `MenuBar`, `MenuPopup` |
| `ui/dialog` | `Dialog`, `FilePicker`, `MessageDialog`, `AlertDialog`, `ProgressDialog` |
| `ui/display` | `Text`, `Card`, `Badge`, `Chip`, `Icon`, `Avatar`, `Spinner` |
| `ui/image` | `Image` — split out of `display.vt` (see its file header for why) |
| `ui/selection` | `SelectionModel`, `SelectionToolbar` — the text-selection machinery `Text`/`TextField`/`TextArea` all share |
| `ui/data` | `TreeView` (hierarchical expand/collapse), `Table` (string-cell grid with search/sort/pagination) |
| `ui/chart` | one-liner chart factories: `lineChart`, `barChart`, `areaChart`, `pieChart`, `scatterChart`, `candlestick`, `heatmap`, `radarChart`, `funnel`, `mapChart`, `timeSeries`, … |
| `ui/datatable` | `DataTable` — a virtualized grid over a native columnar `DataFrame` (`vyto/data/frame`); millions of rows, only the visible cells cost anything |
| `ui/gallery` | `ImageGallery` / `GalleryViewer` — thumbnail grid + full-screen zoom/pan viewer |
| `ui/nav` | `TabView`, `Accordion`, `Toolbar`, `StatusBar`, `Breadcrumb`, `NavRail` |
| `ui/feedback` | `ConfirmDialog`, `InputDialog`, `Popover`, `Tooltip`, `Toast`, `ContextMenu`, `AlertBanner` |
| `ui/functions` | terse factory functions (`col`, `row`, `grid`, `label`, `button`, …) over the `new X(...)` constructors |
| `ui/skins/{ios,material,macos,vyto}` | OS-look `Skin` + paired `Theme` presets |

## Two rendering tiers, one `Widget` API

Widgets never draw directly — they draw through a `Painter`, and there are
two of them. `SurfacePainter` (`ui/core`) is the default: it draws straight
to the `vyto/surface` vector API, needs no extra setup, and is what every
widget's `render()` degrades to when nothing richer is available.
`GfxPainter` (`vyto/gfx`) draws through a blend2d canvas for anti-aliased
rendering — swap it in with `win.use_painter(new GfxPainter(...))`.

`Painter` declares 47 `virtual` methods (`fill`, `frame`, `text`, `line`,
gradients, clipping, transforms, …), each with an inert or lean default —
picking a tier is the app's decision, made once, at the `Window`. **`vyto/ui`
itself never imports `vyto/gfx`** — grep the package and every mention is a
comment, never an `import` — so an app that never asks for the rich tier pays
nothing for blend2d, not even a link dependency. See
[`docs/graphics.md`](../../../docs/graphics.md) for the full rendering stack
and what each tier can and can't draw.

## Widget lifecycle

Every widget goes through the same `virtual` surface, overridden as needed:

- **Sizing**: `measure(painter, theme)` → intrinsic `Size`; `layout(...,
  constraints)` resolves it against what the parent offers; `arrange(...,
  rect)` commits the final `bounds`; `place(...)` positions children that
  don't participate in normal flow (`Stack`/`Positioned`).
- **Drawing**: `render(painter, theme)` draws content, `draw_chrome(...)`
  draws the pluggable shape (see Skins below), `render_within(..., dirty)` is
  the partial-repaint entry point.
- **Input**: `on_click`, `on_drag`/`on_drag_start`/`on_drag_end`, `on_key`/
  `on_key_up`, `on_scroll`, `on_release`, `on_double_click`, `on_long_press`,
  `on_hover_enter`/`on_hover_leave`, `on_focus`/`on_blur`, `wants_text`
  (claims the keyboard), `on_context` (right-click/long-press menu).
- **Tree**: `hit(x, y)` finds the deepest widget under a point;
  `collect_focus(out)` builds the tab order.

A plain (non-`virtual`) method on `Widget` is called directly with no
indirection; only fields you actually override cost a vtable slot. See
[`docs/classes.md`](../../../docs/classes.md) §3 for what `virtual`/
`override` cost and enforce in general.

## `Window` owns the event loop

`Window` holds `root`, an optional `menubar` and `overlay` (modal), a list of
`toasts`, the active `skin`/`theme`, the focus ring (`focusables`/`focus_i`),
and a per-window reactive domain (`rt`, see below). `run()` blocks on the
platform surface's event wait, laying out and painting the first frame, then
dispatching each event to the right widget by hit-testing.

**Vsync gating follows the animation clock, not a fixed policy**: `run()`
only turns on the surface's vsync-paced present while
`this.animations.len > 0`; the instant nothing is animating it drops back to
a plain blocking `wait()`, so a static window costs nothing between events.
This is also why a passive `Tween` used as a polling timer is expensive by
default — see the ticker callout below.

Escape hatches, all first-class and documented in `ui.vt`'s own header:

- `win.surface()` — the underlying `vyto/surface` `Surface`, if you need to
  drop below the widget layer for something.
- `win.use_painter(p)` — swap the renderer (e.g. a `GfxPainter`).
- `win.skin` / `win.theme` — swap widget *shape* and *color* live (§ below).
- A plain `Widget` parent arranges children at their manually set `.bounds`
  — absolute placement, for when the layout containers don't fit.

## Layout: `Column`/`Row` + `BoxConstraints`

Two-pass, constraint-down/size-up, the familiar Flutter-flavoured model:
`Column`/`Row` are flex stacks (`grow` on a child, `JUSTIFY_*`/`CROSS_*` on
the container); `Padding`, `Center`, `SizedBox`, `Expanded`, `Container`,
`Align`, `Stack`/`Positioned`, `Wrap`, `Grid`, `Flexible`, `Divider` round
out the container catalog. `ScrollView`/`ListView` add scrolling — `ListView`
specifically is virtualized (it only measures/draws visible rows), the
non-DataTable equivalent of the datatable module's approach to scale.

## Styling: `Skin` (shape) vs `Theme` (color) — two independent axes

A `Skin` bundles the pluggable-shape `Chrome` classes (`ButtonChrome`,
`FieldChrome`, `ToggleChrome`, `PanelChrome`, `TrackChrome`, `FocusChrome`)
that `draw_chrome()` delegates to — how a button, field, or toggle is
*shaped*. A `Theme` is colors, type scale, and spacing — orthogonal to skin,
swappable independently (`theme_light`/`theme_dark`, or a skin's own paired
`theme_*()`/`theme_*_dark()`). Four skins ship under `ui/skins/`: three
OS-look (iOS, Material/Android, macOS) plus `vyto`, the toolkit's own
flagship style:

```vyto
import { skin_ios, theme_ios } from "vyto/ui";

win.skin = skin_ios();
win.theme = theme_ios();
win.layout();
win.redraw();
```

`apps/skingallery` renders the whole widget set across all four, in both
themes, switchable live — the fastest way to see what changing either one
actually does.

## Reactive binding

A `Window` owns a `vyto/reactive` domain (`win.rt`) and `win.watch(body)`
runs `body` once immediately and again whenever a signal it reads changes —
disposed automatically when the window closes, so a `body` that captures
widgets can't leak the way a hand-wired subscription could:

```vyto
import { Window, Column, Label, Button } from "vyto/ui";
import { sig_int } from "vyto/reactive";

let win = new Window("Counter", 300, 200);
let count = sig_int(win.rt, 0);
let display = new Label("count = 0");

win.watch(() => {
    display.text = "count = " + str(count.get());
    display.invalidate();
});

let inc = new Button("+");
inc.onClick = (b) => count.set(count.get() + 1);
win.root = new Column([display, inc]);
win.run();
```

(`apps/reactive_demo/reactive_demo.vt`) Compare with `apps/uidemo`, which
updates the same kind of state by hand in a `refresh()` method — both styles
are fully supported.

## Builder chains and terse factories

Every fluent setter on `Widget` (`.flex(n)`, `.selfAlign(n)`,
`.animate_layout(on)`, …) is a `builder` method — see
[`docs/classes.md`](../../../docs/classes.md) §5 for what that guarantees
about the chain's type across subclasses. `ui/functions` wraps the
constructors themselves so a whole layout reads as one expression:

```vyto
win.root = col([
    label("Tasks"),
    row([ input, button("Add") ]).gap(6),
    list,
]).gap(4).pad(8);
```

## Things worth knowing before you use these

> **A looping `Tween` used as a passive timer repaints every frame, by
> default.** `Animator.advance()` reports "moved" unconditionally unless the
> animator is marked `passive`, and a full repaint every ~16ms will pin a
> core on an otherwise idle window. `Widget.ticker(period_ms, apply)` exists
> specifically for this — it returns a `Tween` with `passive = true` already
> set — so a poll-until-done or "run every N ms" callback should always go
> through `ticker()`, never a hand-rolled looping `Tween`.

> **`Widget.parent` and `Window.win` are `weak` by design, and any custom
> back-reference you add should be too.** Nothing detects a reference cycle
> — see [`docs/memory.md`](../../../docs/memory.md) §3 for the general rule
> and [`docs/classes.md`](../../../docs/classes.md) §7 for the mechanics.
> `Widget.parent: weak Widget` and `Widget.win: weak Window` are the
> toolkit's own reference examples to copy from.

> **`Window.repaint()` does partial *drawing*, not just partial presenting.**
> It clips to the dirty rect and redraws only intersecting widgets, on the
> assumption that everything outside is still correct in the backbuffer. A
> widget that sets `custom_paint = true` (it both has children *and* paints
> its own content in `render()`) opts its whole subtree out of that pruning
> — set it only where it's actually needed, since it costs the partial-repaint
> optimization for everything under it.

> **`drop_child` tears the subtree DOWN; `detach_child` only un-parents it.**
> `drop_child` calls `release_all()`, which recurses the subtree nulling every
> `Button.onClick`, every `TextField.onSubmit`, every `DataTable.onSort` — the
> only thing that breaks the closure cycles a widget tree accumulates, and
> exactly right when the subtree is being discarded. It is the wrong call for
> an app that SWAPS between screens it keeps: detach with `drop_child`, re-add
> later, and what comes back draws perfectly and answers no clicks. Nothing
> reports it, because a dead handler has no appearance. Use `detach_child` for
> a subtree you still own and call `release_all()` yourself when you genuinely
> drop it.

> **`TextField`, `TextArea` and any subclass answer a right-click with a
> Cut / Copy / Paste / Select all menu**, built from the same `clip_copy` /
> `clip_cut` / `clip_paste` / `select_all` methods `Ctrl+C/X/V/A` call — one
> implementation, two triggers, so they cannot drift. Items that cannot act
> are **absent** rather than greyed (`MenuItem` has no disabled state): a
> read-only `TextArea` offers no Cut and no Paste, and an empty clipboard
> offers no Paste at all. A subclass that watches every keystroke — `SearchField`
> does — should override `fire_edit()`, which is what the menu path calls in
> place of a keystroke.

> **`vyto/ui` never imports `vyto/gfx`, and that's load-bearing, not
> incidental.** It's what lets an app that only uses the lean tier skip
> linking blend2d entirely. Don't add a `vyto/gfx` import to anything under
> this package, even behind a conditional — route rich-tier-only behavior
> through the `Painter` virtuals instead, the way `GfxPainter` itself does
> from the outside.

## Testing headlessly

UI code is tested without ever opening a real window: `VS_HEADLESS=1` swaps
in a headless surface backend, and `VS_EVENTS=<file>` scripts synthetic input
from a plain-text command file instead of a real display server — so the
whole suite runs in CI with no X server, no GPU, and fully deterministic
timing. A scripted `.events` file:

```
# focus starts on a; Tab -> b, fire; Tab -> c, fire; Tab wraps -> a, fire
key Tab
key Enter
key Tab
key Enter
click 150 50
key Enter
close
```

run against its matching `.vt`:

```sh
VS_HEADLESS=1 VS_EVENTS=tests/ui/02_focus.events ./vytoc run tests/ui/02_focus.vt
```

`tests/ui/*.vt` + `*.events` + `*.expected` are the whole golden suite — read
a few side by side (`tests/ui/02_focus.*`, `tests/ui/53_dragscroll.*`) before
writing a new one; `tests/run_tests.sh` runs all of them as part of
`make test`.

## Showcase apps

| App | What it demonstrates |
|---|---|
| `apps/uidemo` | the whole widget set in ~40 lines, hand-updated state |
| `apps/reactive_demo` | the same shape, driven by `vyto/reactive` instead |
| `apps/skingallery` | every widget across all four skins, light and dark, switchable live |
| `apps/charts` | a 15-chart gallery over `ui/chart` |
| `apps/datagrid` | a spreadsheet-grade `DataTable` over real columnar data |
| `apps/gallery` | `ImageGallery`/`GalleryViewer` |
| `apps/vytopad` | a real text editor — menus, dialogs, file I/O, and the widget set together |
