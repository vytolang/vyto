# vyto/desktop/menu

A desktop app's menu bar, built once: the **real Win32 menu bar** on Windows,
and `vyto/ui`'s drawn `MenuBar` on Linux — where X11 has no menu bar of its own
and linking GTK or Qt for one is what `vyto/desktop` exists to avoid.

```vyto
import { AppMenu } from "vyto/desktop/menu";

let menus = new AppMenu(win);
let file = menus.file();
file.item("&New", "Ctrl+N", () => doc.reset());
file.item("&Open…", "Ctrl+O", () => openDoc());
let recent = file.submenu("Open &Recent");
file.separator();
menus.addQuit(file);
menus.addClipboard(menus.edit());
menus.search().item("&Find…", "Ctrl+F", () => find.show());
let dark = menus.settings().check("&Dark Theme", "", false, (on) => setDark(on));
menus.addFullScreen(menus.window());
menus.addAbout(menus.help(), "About Notes", "Notes 1.0");
menus.attach();          // native where available; true when it was
win.run();
```

## API

| | |
|---|---|
| `new AppMenu(win)` | |
| `.file()` `.edit()` `.search()` `.settings()` `.window()` `.help()` | the conventional menus, made on first call (bar order = first-call order), the same `Menu` after. Titles: File, Edit, Search, Se**t**tings (Alt+S is Search's), Window, Help |
| `.menu("&Tools")` | any other top-level menu |
| `.addQuit(m)` | **E**xit with Alt+F4 *shown* on Windows (the OS owns the key); **Q**uit bound to Ctrl+Q elsewhere. Closes the window |
| `.addClipboard(m)` | Cut, Copy, Paste, —, Select All for the focused `TextField`/`TextArea`. Shortcuts are hints: Ctrl+X/C/V/A already reach the focused widget, and binding them here would take Ctrl+C from a `DataTable` |
| `.addFullScreen(m)` | check item, F11 |
| `.addAbout(m, title, text)` | `messageFor` box owned by the window |
| `.drawn()` | use the drawn bar even on Windows (a dark-themed app: Windows 7 has no dark menu bar). Before `attach()` |
| `.attach()` | `bool` — installs the bar; true when the OS's own went in. Also sets `win.close_on_esc = false` (below) |
| `.isNative()` | |
| `nativeMenusAvailable()` | `bool`; false off Windows |

The menus are `vyto/ui` `Menu`s ([`ui/menu.vt`](../../ui/menu.vt)):

| `Menu` | |
|---|---|
| `item(label, shortcut, fn)` | `MenuItem`. `shortcut` `""` for none |
| `check(label, shortcut, on, fn(bool))` | a toggle; the callback gets the new state |
| `submenu(label)` | the new `Menu` |
| `separator()` | |
| `add(label)` | literal text, no `&` markup — for labels built from data |

| `MenuItem` | |
|---|---|
| `set_enabled(b)`, `set_checked(b)`, `set_label(text)` | live: they reach the native menu at once |
| `hint(spec)` | show a shortcut without binding it |
| `bind(spec)` | bind (what `item()` does with its shortcut) |

**Markup.** `&` marks the access key — `"Save &As…"` is A — and `&&` is a
literal `&`. `add()` and `new Menu(title)` take text literally, because a
context menu made from a table called `R&D` must not lose the ampersand.

**Shortcuts** are `Ctrl`, `Shift`, `Alt`, `Super` joined with `+`, then a key:
a character (`Ctrl+,`), `F1`–`F12`, `Del`, `Ins`, `Enter`, `Esc`, `Tab`,
`Space`, `Backspace`, `Home`, `End`, `PgUp`, `PgDn`, the arrows, `Plus`,
`Minus`; `Ctrl++` works too. A shortcut beside an item **is bound**: it fires
from anywhere in the window, asked before the app's `onShortcut`, and a
disabled item's shortcut does nothing. One that does not parse is shown and not
bound. Shortcuts are matched in Vyto on every platform, so native and drawn
behave the same.

## How it works

**Windows:** `CreateMenu`/`AppendMenuW`/`SetMenu`; live state through
`SetMenuItemInfoW` by command id (it finds items in submenus). Each item gets a
16-bit id from 100 up; a choice comes back as `WM_COMMAND`, which vsurf turns
into `EV_COMMAND` with the id, and `Window` hands to `MenuBar.on_command`.
`SetMenu` takes the bar's height out of the client area, so the window grows by
that much — the app keeps the size it asked for, unless maximized. The bar then
takes no client space (`strip_height` 0) and draws nothing. Alt+letter, F10 and
the keyboard inside an open menu are Windows' own. Every function is in the
audited Windows 7 export list, so `--audit` passes. The window destroys its
menu.

**Linux (and Windows with `.drawn()`):** the `vyto/ui` `MenuBar` draws itself:
Alt+letter and F10 open it; Up/Down skip separators and disabled rows;
Right/Left cascade submenus and move between titles; a letter picks the item
with that access key; pointing at another title while one is open switches
menus; submenus open on hover; a click outside closes without firing (and is
swallowed, as on every desktop). Access keys are underlined while the menu is
driven from the keyboard.

**freestanding, Android:** the stub arm; `attach()` uses the drawn bar.

## Caveats

- **Structure is read once.** Add every menu and item before `attach()`.
  Items added after it are not in the Windows menu.
- **`attach()` turns `close_on_esc` off.** Under a native menu the OS may
  already have closed the menu by the time an Escape arrives — on a disabled
  item, say — and that Escape then reaches the window. Left on, it quits.
- **While a Windows menu is open, the app is inside the OS's modal menu loop:**
  animations pause until it closes. Same as every Win32 program.
- **The Windows bar is light** whatever the app's theme — there is no
  documented dark menu bar, and none at all on Windows 7. Use `.drawn()` if
  that clashes.
- Context menus (`ContextMenu`, the text widgets' right-click menu) stay drawn.

## Tests

- `make test`: `ui_104_appmenu_drawn` (the drawn bar and the builder: the
  shortcut parser and `&` markup dumped from the model, then shortcuts,
  Alt+letter, F10, arrows across separators and disabled rows, submenus by
  keyboard and by hover, menu switching by pointer, check items, the clipboard
  items, click-outside) and `ui_105_menubar_native_seam` (a fake
  `MenuBackend` and `command N` events standing in for `WM_COMMAND`: no strip,
  ids reach items, live state reaches the backend, disabled and unknown ids do
  nothing, Alt+letter left to the OS). `notepad_e2e` drives an app using it.
- `make test-wine`: `wine_menu_native` drives `tests/win/menu_demo.vt` through
  Wine's real menus with xdotool — keys and clicks, submenu, check, live
  disabling, Escape not quitting, the client size, Exit.
- On a real Windows box: run `menu_demo.exe` from the staging dir (checklist
  in its header).
