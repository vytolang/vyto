# VytoPad

A multi-tab notepad built on `vyto/ui`, rendered through `vyto/gfx`/`GfxPainter` for anti-aliased text and shapes.

```sh
../../vytoc run vytopad.vt          # from apps/vytopad/
VYTO_PAD_FILE=README.md ../../vytoc run vytopad.vt
```

## Features

- **Multi-tab editor** — open several files in one window, switch by clicking the tab strip.
- **Scrollable editor** — each tab’s editor fills the window and is wrapped in a custom two-axis `ScrollBox` (mouse wheel, Shift+wheel for horizontal, arrow/page keys, scrollbars); the view also follows the caret on edits so typed text stays visible.
- **Line-number gutter** — logical line numbers drawn next to the wrapped text area.
- **Find / Replace** — case-sensitive or case-insensitive search, find next, replace, replace all.
- **Go to Line** — jump to any line number.
- **Status bar** — file name, modified flag, line/column, and line count.
- **Autosave** — every modified document is written to `~/.vytopad/autosave/` on a 10-second timer.
- **File pickers** — Open / Save As with directory navigation.
- **Menu bar** — File, Edit, Help.

## Environment

- `VYTO_PAD_FILE=/path/to/file` — open this file at startup.
- `VYTO_PAD_DIR=/path` — starting directory for the file picker.
- `VYTO_PAD_FONT=/path/to/Font.ttf` — override the default DejaVuSans font (default: `/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf`).

## Architecture

- `Document` — one open file: `TextArea`, line-number gutter, scroll box, file path, modified flag, autosave state.
- `DocTabs` — a dynamic `TabView` subclass that adds/removes document panels and refreshes tab labels.
- `ScrollBox` — custom two-axis scrollable container that wraps the gutter + editor row and drives the base-Widget scrollbars.
- `LineNumberGutter` — custom `Widget` that reads the active `TextArea` wrap map and draws line numbers.
- `PadStatusBar` — custom `Widget` that reads the active document state and draws the status line.
- `FindReplaceDialog` — custom modal overlay with search/replace fields and action buttons.
- `VytoPad` — app state, menu actions, file I/O, find/replace logic, autosave timer.

The app uses `vyto/gfx/painter.GfxPainter` for AA rendering, and the stdlib helpers `vyto/os` (`getenvOr`, `homeDir`) and `vyto/io/file` (`file_exists`, `file_mkdirs`, `path_basename`) instead of raw libc FFI.

## Building

```sh
# dynamic link to blend2d (smaller exe, needs libblend2d.so next to binary)
../../vytoc build vytopad.vt -o vytopad --release
./vytopad

# fully self-contained binary
../../vytoc build vytopad.vt -o vytopad --release --bundle
./vytopad
```
