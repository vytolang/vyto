# Build Prompt — VytoChat (WhatsApp Desktop UI Clone)

Build a WhatsApp-desktop UI clone as a single Vyto app at `apps/whatsapp/whatsapp.vt`, using **only** the bundled `vyto/ui` widget toolkit. This is a UI clone: mock/in-memory data, no networking. Match WhatsApp Desktop's visual layout and interaction feel.

## Reference the existing showcase first

Read `apps/gallery/gallery.vt` before writing anything. It is the canonical, working example of every pattern you need: `Window` setup, `GfxPainter` vs `--surface` tiers, `Theme` construction, `MenuBar`/`Toolbar`/`StatusBar`, `TabView`, `ScrollView`, event callbacks (`onClick`, `onSubmit`, `onToggle`, `onSelect`, `onChange`), and custom `Container`/`Column`/`Row` composition. Follow its import style, its `font_path()` helper, and its `main()` shape.

## Toolkit you have (from `vyto/ui`)

- **Layout**: `Column`, `Row`, `Spacer`, `Box`, `Padding`, `Center`, `SizedBox`, `Expanded`, `Container`, `Align`, `Stack`, `Positioned`, `Wrap`, `Grid`, `Flexible`, `Divider`, `ScrollView`
- **Form**: `Label`, `Button`, `TextField`, `Checkbox`, `ListBox`, `TextArea`, `RadioGroup`, `Switch`, `SegmentedControl`, `Stepper`, `Dropdown`, `ProgressBar`
- **Display**: `Text` (with `ALIGN_LEFT/CENTER/RIGHT`), `Card`, `Badge`, `Chip`
- **Nav**: `TabView`, `Accordion`, `Toolbar`, `StatusBar`
- **Menu/overlay**: `MenuItem`, `Menu`, `MenuBar`, `MenuPopup`, `Dialog`, `FilePicker`, `MessageDialog`, `ConfirmDialog`, `InputDialog`, `Popover`
- **Core**: `Window`, `Widget`, `Painter`, `Theme`

Key constructor signatures (verify against source before use):
- `Container(fixedW, fixedH, pad, bg, child)` — `-1` for fixedW/fixedH to hug child, `-1` for bg to skip fill.
- `Card(pad, bg, border, title, child)` — `-1` bg/border to skip.
- `Badge(label, bg, color, child)` — overlays a count/tag on `child`. Use for unread counts.
- `Chip(label, deletable, bg)`.
- `SizedBox(w, h, child)` — `-1` on an axis to leave it free. Use to pin panel widths/heights.
- Colors come from `rgb(r,g,b)` (import from `vyto/surface`).

## Layout to reproduce

Two-pane WhatsApp Desktop shell inside a `Row`, split by a vertical `Divider`:

```
+----------------------+---------------------------------------+
|  SIDEBAR (fixed ~340)|  CONVERSATION (Expanded)              |
|  ------------------- |  ------------------------------------ |
|  [my avatar] [icons] |  [contact avatar] Name / "online"     |
|  [ search TextField ]|  ------------------------------------ |
|  ------------------- |                                       |
|  ScrollView:         |  ScrollView of message bubbles        |
|   chat rows          |   (incoming left, outgoing right)     |
|   - avatar           |                                       |
|   - name + preview   |                                       |
|   - time + unread    |  ------------------------------------ |
|     Badge            |  [ TextField (Expanded) ] [ Send ]    |
+----------------------+---------------------------------------+
```

Root: `win.root = new Row([sidebar, new Divider(false), conversation])`. Pin the sidebar width with `SizedBox(340, -1, sidebarColumn)`; wrap the conversation pane in `Expanded` so it fills the rest.

### Sidebar (`Column`)
1. **Header row** (fixed height `Container`): a circular-ish "my avatar" (a small square `Container` with a color fill + initial `Label` — see avatar note below), a `Spacer`, then a couple of icon `Button`s ("New chat", "Menu"). Wire the menu button to open a `MenuPopup` or `Popover`.
2. **Search**: a `TextField("Search or start new chat")`. On `onSubmit`/typing, filter the chat list (rebuild + `win.layout(); win.redraw()`).
3. **Chat list**: a `ScrollView` wrapping a `Column` of **ChatRow** widgets (see below). Clicking a row selects that conversation and swaps the right pane's content.

### Conversation pane (`Column`)
1. **Header**: `Row` with contact avatar, name `Label` (bold-ish via accent color), and a muted status `Text` ("online" / "last seen ..."). A `Spacer` then optional icon `Button`s (search, menu).
2. **Message list**: `ScrollView` wrapping a `Column` of **MessageBubble** widgets. Set a distinct chat-background fill on the enclosing `Container` (WhatsApp uses a warm textured panel; a flat `rgb` is fine).
3. **Composer**: a `Row` = `Expanded(new TextField("Type a message"))` + a `Send` `Button`. On send (`onSubmit` or button `onClick`): append an outgoing `MessageBubble` to the active conversation, clear the field, `win.layout(); win.redraw()`, and scroll to bottom.

## Custom widgets you must write

`vyto/ui` has no avatar, chat-row, or bubble primitive, so subclass `Widget` and override `measure`, `arrange`, `draw` (and `on_click` for rows). Model them on how the toolkit's own widgets are structured in `lib/vyto/ui/*.vt`.

- **Avatar**: fixed square `Container` fill in a per-contact color with a centered initial `Label`. Note: the `Painter` interface exposes only `fill` / `frame` / `line` / `text` — **there is no rounded-rect primitive**, so avatars and bubbles are squared rectangles. Don't attempt rounded corners unless you first confirm a painter method for it; a clean flat rectangle reads fine.
- **ChatRow**: fixed-height widget laying out `[avatar] [Column(name, last-message-preview muted)] [Spacer] [Column(time, unread Badge)]`. Highlight the selected row with the theme `sel` color. Override `on_click` to select the conversation.
- **MessageBubble**: a `Container` with word-wrapped text (reuse the `Text` widget or the `split_words` wrapping in `display.vt`), a small muted timestamp line, aligned left (incoming) or right (outgoing) via `Align`/`Row`+`Spacer`. Outgoing bubbles get the green fill, incoming get the panel-gray fill.

## Data model

Plain classes, all in-memory:
```
class Message { text: string; time: string; outgoing: bool; }
class Conversation { name: string; avatarColor: int; messages: Message[]; unread: int; lastSeen: string; }
class ChatState { conversations: Conversation[]; active: int; }
```
Seed 5–7 conversations with a handful of messages each so the UI looks alive. Selecting a chat sets `active`, zeroes its `unread`, and rebuilds the right pane.

## Theme (WhatsApp dark)

Build a `Theme` like `gallery.vt`'s `dark_theme()`:
```
th.bg     = rgb(17, 27, 33);    // #111b21 app background
th.white  = rgb(32, 44, 51);    // #202c33 panels (headers, incoming bubbles)
th.ink    = rgb(233, 237, 239); // #e9edef primary text
th.muted  = rgb(134, 150, 160); // #8696a0 secondary text / timestamps
th.accent = rgb(0, 168, 132);   // #00a884 WhatsApp green (send, links)
th.sel    = rgb(42, 57, 66);    // selected chat row
th.pad = 12; th.spacing = 8;
// outgoing bubble fill: rgb(0, 92, 75) #005c4b — keep as a local const.
```

## `main()` wiring (mirror `gallery.vt`)

- `let win = new Window("VytoChat", 980, 660);`
- Parse `--surface`; when absent, `win.use_painter(new GfxPainter(win.surface(), font_path(), 15.0))` for AA text. Keep it working in both tiers.
- Set `win.theme = whatsapp_dark()`.
- Optional `MenuBar` (File → Exit; View → toggle theme) and a `StatusBar` at the bottom showing the active contact / message count — good for cheap feedback and consistency with the gallery.
- Build root, `win.run()`, print a bye line.

## Interactions to implement (minimum)

1. Click a chat row → right pane shows that conversation, unread badge clears, row highlights.
2. Type in composer + Enter or Send → outgoing bubble appends, field clears, view refreshes.
3. Search field filters the chat list by name.
4. Theme toggle (menu) swaps dark/light and relayouts.

## Constraints & acceptance

- **Only** `vyto/ui` (+ `vyto/gfx/painter`, `vyto/surface`) imports. No new engine/runtime code.
- Must compile and run both tiers:
  - Rich: `./vytoc run apps/whatsapp/whatsapp.vt`
  - Lean: `./vytoc run apps/whatsapp/whatsapp.vt -- --surface`
- After any state change that alters layout, call `win.layout()` then `win.redraw()` (see gallery).
- Every interactive control does something observable (updates the pane, the status bar, or prints to stdout).
- Keep the file self-contained and commented in the header block like `gallery.vt` (title, render tiers, build/run commands, one-line architecture summary).

Deliverable: `apps/whatsapp/whatsapp.vt`, plus a short note of any widget limitation you hit (e.g. no rounded corners) and how you worked around it.
