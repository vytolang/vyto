# vyto/desktop

The operating system's own file dialogs, message boxes, "open this in the
default app", and the per-user folders an app keeps its files in.

Running as a Windows or systemd service is the sibling package
[`vyto/desktop/service`](service/README.md).

```vyto
import { FileDialog, message, confirm, openUrl, configDir, MSG_INFO } from "vyto/desktop";

let p = new FileDialog()
    .title("Open image")
    .filter("Images", "*.png;*.jpg")
    .owner(win.surface().native_window())   // keep it on top of the app
    .open();                                // string?, null = cancelled
if (p != null) { load(p); }

if (confirm("Quit", "Discard changes?")) { quit(); }
openUrl("https://example.com");
let settings = configDir("myapp") + "/settings.ini";
```

## API

| Call | Returns | |
|---|---|---|
| `new FileDialog()` + `.title(t)`, `.startIn(dir)`, `.filter(name, "*.a;*.b")`, `.owner(id)` | builder | The first filter added is selected. `owner` is `Surface.native_window()`, or leave it out |
| `.open()` | `string?` | one existing file; null when cancelled |
| `.openMany()` | `string[]` | empty when cancelled |
| `.save(suggested)` | `string?` | asks before overwriting. Only **chooses** a path — writing the file is up to the app, as with every OS save dialog |
| `.folder()` | `string?` | an existing folder |
| `message(title, text, kind)` / `messageFor(owner, ...)` | `bool` | `MSG_INFO`, `MSG_WARN`, `MSG_ERROR`; false only when nothing could be shown |
| `confirm(title, text)` / `confirmFor(owner, ...)` | `bool` | true **only** for Yes — No, closing it, and "no dialog available" are all false |
| `openUrl(url)` | `bool` | `http:`, `https:`, `mailto:` only |
| `openPath(path)` | `bool` | like a double-click in the file manager |
| `configDir(app)`, `dataDir(app)`, `cacheDir(app)` | `string` | not created; `""` for a bad app name |
| `sharedDataDir(app)` | `string` | for services: `%ProgramData%\<app>`; `/var/lib/<app>` as root on Linux, else `dataDir(app)` |
| `desktop_available()` | `bool` | false on Linux with neither zenity nor kdialog, and in freestanding/Android builds |

| | Linux | Windows |
|---|---|---|
| `configDir` | `$XDG_CONFIG_HOME/<app>`, else `~/.config/<app>` | `%APPDATA%\<app>` (Roaming) |
| `dataDir` | `$XDG_DATA_HOME/<app>`, else `~/.local/share/<app>` | `%LOCALAPPDATA%\<app>` |
| `cacheDir` | `$XDG_CACHE_HOME/<app>`, else `~/.cache/<app>` | `%LOCALAPPDATA%\<app>\cache` |

A relative `XDG_*_HOME` is ignored, as the XDG spec requires.

## How it works

**Windows:** `GetOpenFileNameW`/`GetSaveFileNameW` (the Explorer-style dialog
on Windows 7 and later), `IFileOpenDialog` with `FOS_PICKFOLDERS` for folders,
`MessageBoxW`, `ShellExecuteW`, `SHGetKnownFolderPath`. Every function is in
the audited Windows 7 export list, so `vytoc build --audit` passes. All text
goes through the UTF-16 APIs: Unicode titles and file names work on Windows 7,
not only where the manifest's UTF-8 code page applies.

**Linux:** runs `zenity` (GNOME and most others) or `kdialog` (preferred under
KDE), and `xdg-open`. They are separate programs, not linked libraries, so an
app that shows one dialog does not carry GTK or Qt. zenity 4 (GTK 4) dropped
`--attach` and `--confirm-overwrite`; the version is checked once and the flags
follow it.

**Programs are started from an argument list, never through a shell.** A
title, a path or a URL is passed to the program exactly as given — no quoting,
no `$(...)`, no word splitting. Values are passed as `--option=value` so one
that starts with `-` cannot be read as an option, and a positional path that
starts with `-` gets a `./` in front. `tests/run_tests.sh` checks the exact
argv each program receives, including a title written to run a command, and
was fault-injected with a shell-joined command line to make sure it fails.

**freestanding and Android:** the shim's stub arm answers "unavailable".
Android apps use `vyto/mobile/android` for the system UI.

## Caveats

- **Dialogs are modal and block** the calling thread until answered. On X11
  the app's own window does not repaint meanwhile; Win32's modal loop keeps
  painting it.
- **`openPath` runs programs.** Given an `.exe` on Windows (or an executable on
  Linux, depending on the file manager) it starts it, exactly as a
  double-click would. Pass only paths the app chose or the user picked.
- **`openUrl` refuses everything but http, https and mailto** — on Windows,
  `ShellExecute` on an arbitrary string can run a program.
- kdialog renders message text as Qt rich text when it looks like HTML; zenity
  is passed `--no-markup`. Neither can run anything from it.
- zenity's `--attach` (keep on top of a window) works on X11 with zenity 3
  only.
- **GTK multi-select quirk (zenity):** entering a folder auto-selects its
  first row, even when that row is a folder, and Ctrl+click *adds* to it. With
  a folder in the selection, GTK refuses OK — the dialog flashes and stays
  open. Plain-click the first file, then Ctrl+click the rest (or Shift+click a
  range). This is GTK's file chooser, not something zenity lets a caller
  change; reproduced with Mutter on Xvfb, 2026-11-12.

## Tests

- `make test`: `desktop_dialogs_{zenity,kdialog,none}` (argv goldens against a
  fake dialog program), `desktop_zenity4_flags`, `desktop_open_url`,
  `desktop_dirs`, `freestanding_desktop_stub`.
- `make test-desktop`: the **real** zenity (or kdialog) on a private Xvfb
  display, every dialog answered with xdotool, with and without an owner
  window (`--attach`); the saved file is checked on disk. Needs Xvfb, xdotool
  and a dialog program, SKIPs otherwise. Run from a snap terminal (VS Code's)
  it still works: the dialog program gets a scrubbed environment, since the
  snap's `GTK_PATH`/`LOCPATH` crash any GTK program started from it.
- `make test-wine`: `wine_desktop_dialogs` answers each dialog of
  `tests/win/desktop_demo.vt` with xdotool. `desktop_dirs_win` runs in the
  staged Windows suite.
- On a real Windows box: run `desktop_demo.exe window` from the staging dir and
  answer the dialogs (checklist in its README).
