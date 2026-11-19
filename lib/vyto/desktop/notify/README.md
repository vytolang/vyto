# vyto/desktop/notify

The operating system's own notifications: a GNOME or KDE banner on Linux, a
tray balloon on Windows (shown as a toast from Windows 10 on).

```vyto
import { notify, notify_app, notify_replace, notify_close, notify_quiet,
         NOTIFY_INFO, NOTIFY_WARNING } from "vyto/desktop/notify";

notify_app("SpeccalPOS");                // once, before the first notice
notify_quiet(true);                      // the app plays its own alert sounds

let id = notify("Printer offline", "Receipts are queued until it is back", NOTIFY_WARNING);
// ... later, the same notice updated rather than a second one:
notify_replace(id, "Printer back", "3 queued receipts printed", NOTIFY_INFO);
```

**A notice is for someone who is not looking at the app**: the window is
minimised, behind another, or on a screen nobody faces. For someone who is
looking at it, `vyto/ui`'s `Toast` and `AlertBanner` are the right tools.

## API

| Call | Returns | |
|---|---|---|
| `notify(title, body, kind)` | `int` | an id, or 0 when nothing was shown. `kind` is `NOTIFY_INFO`, `NOTIFY_WARNING` or `NOTIFY_ERROR` |
| `notify_replace(id, title, body, kind)` | `int` | the same id. 0, or an id this process never handed out, makes a new notice |
| `notify_for(title, body, kind, timeout_ms)` | `int` | gone after `timeout_ms`; Windows from Vista on ignores it |
| `notify_close(id)` | `bool` | false for an unknown id, and on Windows for one a later notice replaced |
| `notify_app(name)` | | the app name on Linux, the tray tooltip on Windows. Default `"Vyto"` |
| `notify_quiet(on)` | | asks the OS not to play its own sound. A hint: some Linux servers ignore it |
| `notify_available()` | `bool` | a notification service answered (Linux), or the tray icon was added (Windows) |
| `notify_server()` | `string` | `"gnome-shell"`, `"Plasma"`, `"dunst"`, `"Shell_NotifyIcon"`, or `""` |

`NOTIFY_ERROR` is "critical" urgency on Linux, and most desktops keep a critical
notice on screen until someone dismisses it. INFO and WARNING are both
"normal", and differ by icon.

The body is **plain text**. On a server that reads markup, `& < >` are escaped,
so `Tea & <milk>` arrives as written. Malformed UTF-8 becomes U+FFFD before it
reaches D-Bus, because libdbus aborts the process on an invalid string.

## How it works

**Linux and the BSDs**: the freedesktop `org.freedesktop.Notifications`
service on the session bus, spoken directly over D-Bus with `libdbus-1.so.3`
loaded at run time. It does not use `notify-send`, which would mean a process
per notice, a dependency on libnotify-bin, and no id back. It does not use
`gdbus`, whose arguments are GVariant text, so every title would become a
quoting problem. Every argument travels typed. A machine without libdbus, or
without a session bus, reports `notify_available()` false; libdbus is never
allowed to autolaunch a bus.

**Windows**: one tray icon per process, through `Shell_NotifyIconW`, owned by a
hidden window. Its balloon carries the notice. The icon is the exe's own
(resource 1, which `vytoc --icon` writes), else the stock application icon. It
is removed at exit so no ghost icon is left in the tray. There is one balloon,
so any notice replaces the one showing.

**macOS, Android, `--freestanding`**: every call answers "unavailable".
`vyto/mobile/android` has its own notifications.

## No call waits for the server

GNOME 42 shows a banner at once but answers `Notify` **2.5 to 6 seconds later**
(measured 2026-11-19; `notify-send` stalls the same way). A UI thread that
waited would freeze for seconds per notice.

So on Linux a call only queues a job, a copy of its strings in C memory, and a
single C worker thread talks to the bus:

- It **sends at once** and collects replies as they arrive, so a burst of
  notices goes out together, not one GNOME reply apart.
- It **waits for a reply only when a later replace or close needs that
  notice's server id**. The ids Vyto sees are local, counting from 1, and the
  worker maps them to the server's. Jobs run in order, so a replace always
  follows the notice it refers to.
- **At exit** the process waits, at most 2 s, until every queued notice has been
  *written* to the bus. It does not wait for replies, so a tool that notifies and
  exits at once still shows its notice.
- **A burst beyond 64 unsent notices is dropped.** Those calls return 0.

Only the first call in a process asks the service anything directly: who it is,
and whether it reads markup. Every server answers those in under a millisecond,
bounded at one second.

## Testing

`VYTO_NOTIFY=off` makes everything answer "unavailable". `make test` runs
`tests/fixtures/notify_demo.vt` against `tests/fixtures/desktop/fake_notifyd.py`
on a private bus (`dbus-run-session`). The fake logs every typed argument it
receives, and holds each reply back 400 ms, the way GNOME does. That gives three
checks:

- the golden is what really crossed D-Bus
- the fixture asserts that no call waited for a reply
- a second run advertises `body-markup` and must see the escaping

These tests need `dbus-run-session` and `python3-gi`, and SKIP without them. The
same fixture runs in the Windows slice (`make test-win`), with a per-target
golden for the server name.

Not yet: clicking a notice, or action buttons. Both need the click to reach the
app's event loop, which neither backend is wired to.
