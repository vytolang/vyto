# Getting started with Vyto

> **Status: experimental.** Vyto is a young language — the compiler, runtime,
> and standard library are all evolving, and breaking changes happen. Great for
> learning, prototyping, and open-source apps; not yet for mission-critical or
> production business systems.

This guide takes you from a clean checkout to your first running program and a
small GUI app, in about five minutes.

## 1. Prerequisites

- A **C99 compiler** — `cc`/`gcc`/`clang`. The compiler itself is plain C99 with
  zero dependencies.
- **make** and **git**.
- For **GUI apps**: the platform's windowing libraries (on Linux, `libX11`).
- Optional: **tcc** for faster debug builds (Vyto uses it automatically when
  present; falls back to `cc -O0`).

## 2. Get the source and build the compiler [Linux Only]

Clone the repo and build:

```sh
git clone https://github.com/vytolang/vyto.git
cd vyto
make                 # builds ./vytoc (and ./vytobind)
```

That produces `./vytoc`, the Vyto compiler, in the repo root. Verify it:

```sh
./vytoc run examples/01_hello.vt
```

You should see `hello, vyto` and a few counter lines.

## 3. Your first program

Create `hello.vt`:

```js
fn main() {
    print("hello, vyto");

    let n = 3;
    for (let i in 0..n) {
        print("tick " + i);
    }

    let x: float = 2.5;
    print("x*2 = " + x * 2.0);
}
```

Run it:

```sh
./vytoc run hello.vt
```

## 4. The two commands

- **`vytoc run <file.vt>`** — compile and run in one step. Use this while
  developing.
- **`vytoc build <file.vt>`** — compile to a native executable. Add `-o <path>`
  to choose the output, `--release` for the optimized (`cc -O2`) build:

  ```sh
  ./vytoc build hello.vt --release -o hello
  ./hello
  ```

The generated C is human-readable — look inside the `.vyto-cache/` directory
created next to your source.

### The two caches

Builds use two caches, and knowing which is which saves confusion:

| | Where | Holds |
|---|---|---|
| **Local** | `.vyto-cache/` next to the file you ran | the generated C, its objects, and the executable |
| **Shared** | `.vyto-cache/obj/` at the root of the Vyto tree | objects compiled from the runtime and from native packages' C |

The shared one exists because a native package's C does not depend on the
program being built. `vyto/regex` vendors 30 translation units of PCRE2; without
sharing, every directory you ran a program from compiled its own copy. Entries
are keyed on a hash of the source contents **and** the full compile command, so
switching branches or touching a file cannot serve a stale object — and reverting
a change makes the previous objects valid again instantly.

Generated C stays local on purpose: a generic instantiated with your types is
emitted into the module that *declared* the generic, so a stdlib module's C
depends on your program.

Delete either at any time — they only cost a rebuild. `make clean-cache` removes
every one in the tree, including the shared one. `--clean` clears only the local
cache for that file. `VYTO_OBJ_CACHE=<dir>` moves the shared cache;
`VYTO_OBJ_CACHE=off` disables sharing entirely.

## 5. A small GUI app

Vyto ships a native UI toolkit, `vyto/ui`. It renders on two tiers: a lean X11
surface, and a rich anti-aliased tier over blend2d — the *same* widget code runs
on both.

Create `app.vt`:

```js
import { Window, Column, Row, Label, Button, TextField } from "vyto/ui";

fn main() {
    let win = new Window("My first Vyto app", 360, 220);
    win.root = new Column([
        new Label("Hello from vyto/ui"),
        new Row([new TextField("type here"), new Button("Add")]),
        new Button("OK"),
    ]);
    win.run();
}
```

Run it:

```sh
./vytoc run app.vt
```

> **Rich (blend2d) tier.** The example above runs on the lean X11 tier, which
> needs no extra setup. The anti-aliased rich tier (`vyto/gfx` / `GfxPainter`)
> is backed by **blend2d**, a large C++ library that is *not* vendored in git —
> build it once before using that tier:
>
> ```sh
> lib/vyto/gfx/native/build-blend2d.sh          # defaults to linux-x64
> ```
>
> It clones and compiles blend2d, then drops the headers and
> `libblend2d.{so,a}` the package needs. Requires **git, cmake (≥3.22), a C++
> compiler, and ninja**.

To wire up interactivity (button clicks, text input, state), see
[examples/05_widgets.vt](../examples/05_widgets.vt). One thing to know up front:
**closures capture by value**, so a click handler can call methods on a captured
widget (widgets are references) but cannot mutate a captured primitive and have
it persist — hold changing state in an object or a top-level structure instead.

A handler does not have to be a closure literal. Anywhere a `fn(...)` type is
expected you can also pass a named function, a generic function (instantiated
from the target type), or a **bound method** — `obj.method`, which carries its
receiver, so the handler can reach that object's state without capturing
anything:

```js
class App {
    hits: int;
    fn health(req: ServerRequest, res: ServerResponse) {
        this.hits = this.hits + 1;
        res.text(200, "ok");
    }
}

let app = new App();
s.get("/health", app.health);      // no arrow, and it still sees app.hits
```

See [examples/98_fn_values.vt](../examples/98_fn_values.vt) for all four forms.

**A bound method holds its receiver alive.** Storing one back onto an object the
receiver owns — `this.btn.onPress = this.handle` — makes a reference cycle, and
Vyto has no cycle collector, so it leaks with nothing failing. Clear the field
during teardown, or keep the back-reference `weak`.

## 6. Imports and the standard library

- Your own files: `import { thing } from "other";` — a bare module stem,
  resolved local-first (an `other.vt` beside your source shadows anything else).
- The standard library lives under `lib/vyto/` and is imported as
  `vyto/<path>` — e.g. `vyto/util/json`, `vyto/io/file`, `vyto/util/fmt`,
  `vyto/ui/chart`, `vyto/ui/datatable`.
- Third-party packages live in a **package root** — a directory that *contains*
  packages, exactly as `lib/` contains `vyto/`.

An import is resolved by probing, in order:

1. the importing file's own directory (so a local file always wins, and a
   package's internal imports keep working wherever the package is dropped)
2. each `--modpath <dir>`, in the order given — repeatable, relative to the
   current directory
3. `$VYTO_PATH`, a `:`-separated list of the same, **ignored entirely if any
   `--modpath` was passed**
4. `vyto_modules/`, found by walking up from the entry file's directory
5. `$VYTO_HOME/lib` — last, so nothing can shadow the standard library

Pointing at a single checkout means naming its *parent*: for a package at
`/src/vytoweb`, pass `--modpath /src`.

**A file under a package root is named by its root-relative path**, not by its
bare stem — `vyto_modules/vytoweb/router/router.vt` is the module
`vytoweb_router`, the same rule that makes `lib/vyto/ui/ui.vt` into `vyto_ui`.
Because the package directory is always part of the name, two packages may each
ship a `config.vt` and both still load. A file outside every root keeps its bare
stem, and two of those sharing a stem is an error: a module name prefixes every
emitted C symbol, so the collision is unbuildable.

### Getting a package

A package root is just a directory, so **unzipping a download into
`vyto_modules/<name>/` is all it takes** — no manifest and no tool:

```
myapp/
  main.vt                        import { kv_get } from "vyto-kv/store";
  vyto_modules/vyto-kv/store/store.vt
```

`vytopack` automates the bookkeeping when you want it — resolving a tag to an
exact commit, recording it in `vyto.lock`, hashing what landed, and sharing a
cache across projects:

```sh
vytopack install --url=https://github.com/vytolang/vyto-kv --rev=v0.3.1
```

Both routes produce the same tree and the same module names. See
`src/vytopack/README.md`.

Each `.vt` file is one compilation unit, content-hash cached, with automatic
dead-code stripping at link time — an unused import costs essentially nothing in
the shipped binary. See the **Standard library** table in the
[README](../README.md#standard-library) for the full module list.

## 7. Shipping a binary

`vytoc build` produces a **self-contained native executable** — the Vyto runtime
is statically compiled in, so there is no `libvyto.so` to ship. It depends only
on base system libraries (libc, plus e.g. libX11 for a GUI app), so you can copy
it and run it without `vytoc` installed.

```sh
./vytoc build app.vt --release -o app          # native binary
./vytoc build app.vt --release --target linux-arm64 -o app-arm64   # cross-compile
```

Apps that use a prebuilt native library (e.g. `vyto/gfx`, which links blend2d)
ship the executable plus that `.so` beside it by default; add `--bundle` to
statically link everything into one file.

## 8. Cross-compiling for Windows

> **Shipping a Windows desktop app?** [desktop.md](desktop.md) walks through it
> step by step: the exe, its icon and version, the Windows 7 check, an
> installer, signing, and background services. This section is the reference
> behind it.

`windows-x64` builds are produced on Linux with the mingw-w64 toolchain:

```sh
sudo apt-get install -y gcc-mingw-w64-x86-64      # provides x86_64-w64-mingw32-gcc
./vytoc build app.vt --release --target windows-x64 -o app.exe
```

`vytoc` picks `x86_64-w64-mingw32-gcc` for that triple automatically, and the
resource compiler `x86_64-w64-mingw32-windres` from the same binutils package.

### What every Windows exe gets

With no extra flags, a `windows-x64` exe:

- **needs no mingw DLLs.** `libgcc` is linked statically. The CRT is
  `msvcrt.dll`, which every Windows since XP ships, so nothing needs installing
  on Windows 7 either.
- **is a GUI app if it opens windows.** A program that imports `vyto/surface`
  (directly, or through `vyto/ui`, `vyto/gfx` or `vyto/win/d2d`) is linked for
  the GUI subsystem, so double-clicking it shows no console window. Anything
  else is a console program. Override with `--console` (a command-line tool that
  also happens to draw) or `--gui`. A GUI exe started from `cmd` still prints
  there: it attaches to the parent's console. Because cmd does not wait for a
  GUI program, that output can arrive after the prompt returns; a tool meant for
  the command line should be built with `--console`.
- **carries an application manifest**, which:
  - lists Windows 7 through 11 as supported, so newer Windows does not misreport
    its version to the app
  - asks for no elevation (`asInvoker`), so an exe named like an installer does
    not trigger a UAC prompt
  - turns on UTF-8 for the ANSI APIs on Windows 10 1903 and later, for any C
    code of your own that calls them (Vyto's runtime and stdlib do not need
    it, see below)
  - declares per-monitor DPI awareness, so text renders at the display's
    scale instead of being stretched (125%, 150%), and a `vyto/ui` window
    rescales itself when it moves to a display with another scale (Windows
    8.1 and later; Windows 7 and 8 read it as system-wide awareness)
- **has ASLR and DEP on**, and under `--release` is stripped of debug info.
- `--shared` builds `<name>.dll`, exporting `vyto_app_main`.

**Unicode works on every version, Windows 7 included.** File names, directory
listings, command-line arguments, environment variables, `run`/`capture`
command lines, window titles and typed text all reach the program as UTF-8:
the runtime and stdlib convert at the boundary and call the Windows "W" APIs,
so a profile like `C:\Users\Zoë` or a file named `日本語.txt` just works. (A
console program's *output* captured by `capture()` is still the bytes it
wrote, which on Windows is usually the OEM code page.) Files larger than 2 GB
can be positioned and sized in `vyto/io/file`, and `readfile`/`writefile`
read and write them in one pass as on Linux.

`appDir()` is the directory holding the exe, so an app shipped as
`app.exe` plus an `assets/` folder finds its files from any working directory.
`libDir()` is `<exe dir>/lib`, else `<exe dir>/../lib`.

### Name, version and icon: `--manifest`

An exe's icon, its version info (Explorer's Properties → Details tab) and its
DPI mode come from a small Vyto program, the same way an Android app's
`AndroidManifest.xml` comes from its `manifest.vt`:

```js
// win_manifest.vt
import { WinManifest } from "vyto/win/manifest";

fn main() {
    let m = new WinManifest("Vyto Files")
        .version("1.2.0")
        .icon("assets/icon.png")        // a .png up to 256x256, or an .ico
        .company("Vyto")
        .description("A two-pane file manager")
        .copyright("(c) 2026 Vyto");
    m.emit();
}
```

```sh
./vytoc build app.vt --release --target windows-x64 --manifest win_manifest.vt -o app.exe
```

`vytoc` runs that program on the build machine and embeds what it prints.
`emit()` validates first: a bad version, an unknown DPI mode or a non-image
icon fails the build with the reason instead of producing a broken exe. The
icon path is relative to the manifest program. `.dpi("system")` keeps one
scale for the whole session, for a `Surface` program that cannot redraw at a
new one; `.dpi("unaware")` opts an app out of DPI awareness, for one that does
not scale its own drawing yet. These
settings belong to the build, not the running program, because Explorer and
the Windows loader read them from the file without starting it. A window's icon
can still change while the program runs: `Window.set_icon(pixels, w, h)`.

### Will it run on Windows 7?

```sh
./vytoc build app.vt --target windows-x64 --release --audit
./vytoc audit app.exe libsomething.dll       # any PE file, built by anything
```

`--audit` checks every function the exe imports — and every DLL vytoc ships
beside it — against the exports of a real Windows 7 install
(`lib/vyto/win/win7_exports.txt`), and fails the build if one is missing. It
also refuses a Universal CRT dependency (`api-ms-win-crt-*`), which Windows 7
lacks without KB2999226, a DLL that is neither a Windows 7 system DLL nor
shipped beside the exe, and an import by ordinal. A newer API used without a
runtime check is caught here instead of as "entry point not found" on a
user's machine. It reads the PE file itself, so it needs no objdump and works
for an exe from any toolchain; `make test-win` runs it over everything it
stages.

### Signing: `--sign`

```sh
VYTO_SIGN_PASS=... ./vytoc build app.vt --target windows-x64 --release \
    --sign cert.pfx --timestamp http://timestamp.digicert.com
```

Authenticode-signs the exe and each DLL shipped beside it with a PKCS#12
certificate, through [osslsigncode](https://github.com/mtrojnar/osslsigncode)
(`apt install osslsigncode`). A signed exe shows its publisher in the UAC and
SmartScreen prompts instead of "Unknown publisher"; SmartScreen's reputation
still has to build up per certificate. The password is read from
`$VYTO_SIGN_PASS` and handed over in a private file, never on a command line.
`--timestamp` (needs the network) keeps the signature valid after the
certificate expires — use it for anything you ship. Signing runs after the
strip and the audit, and is part of the link stamp: dropping `--sign` gives an
unsigned exe on the next build, and an unchanged rebuild does not re-sign.
Don't pack signed exes with UPX: Defender treats packed binaries as suspect.

### An installer: `--installer`

```sh
./vytoc build app.vt --target windows-x64 --release --audit \
    --manifest win_manifest.vt --installer            # -> App-1.2.0-setup.exe
./vytoc build app.vt ... --installer --per-user       # no admin, %LOCALAPPDATA%\Programs
```

Produces `<Name>-<version>-setup.exe` beside the exe: one file holding the
app, everything vytoc ships beside it (DLLs, `cacert.pem`) and an uninstaller,
compressed. It installs to `Program Files\<company>\<name>` (asking for
administrator rights) or, with `--per-user`, to the user's own programs folder
(asking for nothing); adds a Start menu shortcut and, if ticked, a desktop
one; and registers in Programs and Features, whose Uninstall removes exactly
what was installed. Running a newer setup upgrades in place and asks for a
running app to be closed first. `setup.exe /S` installs silently (exit status
0 on success), `/D=<folder>` picks the folder and `/NODESKTOP` skips the
desktop shortcut; `uninstall.exe /S` removes silently.

The setup program is Vyto too (`lib/vyto/win/setup`), so it needs nothing but
Windows 7, passes `--audit`, and `--sign` signs it with the app.

If the `--manifest` declares `.service(...)`, the installer also registers
the exe as a Windows service and starts it; see `vyto/desktop/service`. A
service needs a per-machine install, so `--per-user` is refused then.

### App suites: a GUI and its services, one installer

A product is often a GUI plus programs that run in the background — checking
stock, taking payments, holding a database connection. Declare them in the
manifest; `--installer` builds every program and installs them **into one
folder**, registers the services (in dependency order), stops them for an
upgrade and removes them on uninstall:

```vyto
new WinManifest("Vyto Café POS Suite").version("0.2.0").company("Vyto")
    .program("payments", "payments.vt")          // -> payments.exe beside pos.exe
    .program("stock", "stock.vt")
    .serviceFor("payments", "VytoCafePayments", "Vyto Café Payments", "...", "auto")
    .serviceFor("stock", "VytoCafeStock", "Vyto Café Stock", "...", "auto")
    .dependsOn("VytoCafePayments", "VytoCafeStock")
    .emit();
```

The GUI and the services talk over `vyto/desktop/channel`: loopback only, a
port the OS picks (never a fixed number, so nothing collides), held
exclusively, found through a discovery file with a token. Requests get
replies, services push events, and the GUI's window loop sleeps until one
arrives. `apps/pos_suite` is the worked example: the till asks the payments
service for a (simulated) M-Pesa payment of the sale's total and gets the
payment record back; the stock service tracks what is left and pushes low-stock
warnings. Either service can stop and the till keeps selling for cash.

### What is portable today

The **core language, the runtime, and the stdlib packages with no POSIX
dependency**: `vyto/math`, `vyto/reactive`, `vyto/util/*` (fmt, json, sort, text,
time, date, and the data-format modules — csv, xml, toml, ini, markdown, html,
url, mime, log), `vyto/io/file`, `vyto/data/frame`, `vyto/os/os`, `vyto/anim`,
`vyto/cli`, `vyto/geom/*`, `vyto/geo/*`, `vyto/desktop`, plus FFI, native
packages with in-tree C, and prebuilt `native/windows-x64/*.dll` packages.

`vyto/desktop` gives the OS's own file dialogs, message boxes, "open in the
default app" and per-user folders (`configDir`, `dataDir`, `cacheDir`);
`vyto/desktop/service` runs a program as a Windows or systemd service from the
same code. On
Windows that is comdlg32, `IFileOpenDialog`, `MessageBoxW`, `ShellExecuteW`
and `SHGetKnownFolderPath`, all present on Windows 7; on Linux it runs zenity
or kdialog and xdg-open. See `lib/vyto/desktop/README.md`.

`vyto/util/uuid` is portable too, though unlike its `vyto/util` siblings it does
carry a native shim: `getrandom(2)` on Linux, `BCryptGenRandom` on Windows (so it
`#link`s `bcrypt` there), `/dev/urandom` otherwise.

`vyto/terminal` is portable on the same terms — termios on POSIX, the console
API on Windows, and a stub arm that reports "not a terminal" under
`--freestanding`. It needs no `#link` at all, since everything it binds is
libc. The not-a-terminal answer is a normal state rather than a failure: under
a pipe, a CI runner or a freestanding target, `raw_mode()` returns null and the
ANSI helpers return empty strings, so output stays byte-stable.

`vyto/regex` is portable and needs no provisioning, which is worth calling out
because it carries by far the most in-tree C of any module: PCRE2 and its JIT
are vendored under `lib/vyto/regex/native/src/pcre2/` and compiled from source
like any other shim. That is exactly why it sits here rather than in the "not
portable" list with `vyto/intl` — there is no system library to be missing. Where
the JIT cannot run it falls back to PCRE2's interpreter with identical results.

**GUI and graphics**: `vyto/surface` (the Win32 GDI backend), `vyto/ui`, and
`vyto/gfx` all build for `windows-x64` — see the next section for the one
prerequisite `vyto/gfx` has.

**Networking**: `vyto/net/socket` builds on Winsock2 (`ws2_32`, linked
conditionally), covering TCP, UDP and `PollSet`. `vyto/net/http` and
`vyto/net/websocket` build too, with **HTTPS working on Windows 7 through 11**:
Windows has no system libcurl, so vyto/net carries curl and mbedTLS as
vendored source (`lib/vyto/net/native/windows-x64/`, compiled for that target
only) and they link statically into your exe — no DLL, `--bundle` works, and
the only system DLLs it adds are `ws2_32` and `bcrypt`. TLS 1.2 and 1.3 are
done by mbedTLS, independent of the OS's Schannel. The first Windows build
compiles about 270 files (around 25 s on 8 cores, `VYTO_JOBS` to change the
parallelism); after that they come from the object cache.

Certificates are trusted from the Windows ROOT store **plus** `cacert.pem`, the
Mozilla bundle vytoc copies beside the exe. Ship that file with the app:
Windows installs most roots lazily, only when its own TLS stack first needs
one, so on a machine that rarely browses the ROOT store alone can miss one as
common as Let's Encrypt's. `http_set_ca_file(path)` replaces both with your own
bundle (any path, Unicode included). Verification cannot be turned off. TLS
fixes now arrive by updating the vendored copy (`refresh-curl.sh`), not
through Windows Update.

**Parallelism**: `vyto/os/worker` builds, but it is a **re-exec pool rather than
`fork()`** — Windows has no `fork`. Workers are the same executable relaunched
with `VYTO_WORKER_*` in the environment, re-running `main` until they divert
into serve mode, over a loopback socket with a `BCryptGenRandom` token
handshake. The consequence is real and worth designing around: **anything
`main()` does before that point runs once per worker.**

The fix is one line, and it is worth taking on every platform:

```js
fn main() {
    workerEntry((input) => heavyCompute(input));   // returns instantly in the parent
    ...
}
```

A re-launched worker serves jobs there and never returns, so nothing above it
can run twice. It also decouples the pool from startup — build it after a window
is open, or call `respawn()` to replace a worker that died.

**Memory mapping**: `vyto/mmap` builds everywhere. POSIX `mmap` covers Linux and
Android; Windows goes through `CreateFileMapping`/`MapViewOfFile`. One thing the
shim handles that a caller would otherwise get wrong: `mmap_at`'s offset is
rounded down to the platform's granularity, which is 4 KiB on POSIX and **64 KiB
on Windows** — you always get a base at the byte you asked for. `advise()` is
honoured on POSIX; on Windows only `WILLNEED` does anything (via
`PrefetchVirtualMemory`, resolved at runtime so the binary still loads on Win7)
and the rest are successful no-ops.

**Not portable**, and excluded from the Windows suite: `vyto/net/link`, `wifi`
and `raw` (Linux netlink/AF_PACKET), and `vyto/intl` (ICU). Of `vyto/hw/*`,
serial, device, ioctl, input, uevent, power, usb (listing), location and sdr work
on Windows; camera, sensors, gpio, i2c and spi return empty lists or `null` there
([`lib/vyto/hw/README.md`](../lib/vyto/hw/README.md#platform-support)).

Three Windows-specific behaviours worth knowing: `vyto/os`'s `run()` and
`capture()` go through `cmd.exe`, not a POSIX shell, so shell built-ins differ;
`vyto/util/date`'s `parse()` runs on a strptime written for this port (Windows
ships none), covering the conversions `format()` can round-trip — anything else
returns the invalid-date sentinel rather than guessing; and `PollSet` uses
`WSAPoll`, which never reports a *failed* non-blocking connect, so a refused
`connectAsync` stays un-ready instead of surfacing an error. Put your own
timeout around it there.

### Native rendering: D2DPainter

A `vyto/ui` app can draw through Direct2D instead of blend2d with
`win.use_painter(new D2DPainter(win.surface(), 15.0))` from `vyto/win/d2d`.
Text then uses the system's fonts and ClearType, and the exe needs no
blend2d DLL and no `-posix` C++ toolchain, so the section below does not
apply. Keep the painter choice in a Windows-only entry file: importing
`vyto/gfx` as well would link blend2d anyway. See
[Graphics, §5](graphics.md#d2dpainter-native-rendering-on-windows).

### Graphics apps: blend2d and fonts

`vyto/gfx` is the one stdlib package backed by a **prebuilt** library rather than
in-tree C, so it needs a `windows-x64` build of blend2d before anything importing
it will link. blend2d is C++, so this needs the mingw C++ compiler too:

```sh
sudo apt-get install -y g++-mingw-w64-x86-64
./lib/vyto/gfx/native/build-blend2d.sh --dry-run windows-x64   # confirm the plan
./lib/vyto/gfx/native/build-blend2d.sh windows-x64
```

Every triple is built from **one** clone at **one** revision, stamped in
`native/blend2d.commit`. The headers in `native/src/blend2d/` are
shared by all platforms, so adding a triple checks that exact commit out rather
than taking current master — otherwise the headers would describe a newer
blend2d than the libraries already on disk were compiled from. `--refresh` is
the deliberate way to move the revision forward, and it rebuilds every triple.

Forgetting this step is not mysterious; `vytoc` says so:

```
package 'vyto_gfx' ships prebuilt native libraries, but none for windows-x64
  (has: linux-x64) — build one with lib/vyto/gfx/native/build-blend2d.sh windows-x64
```

**Fonts.** A graphics app that hardcodes a system font path will not find one on
Windows. Vendor the typeface into the app instead and embed it with
`--with-assets` — `apps/charts` and `apps/motiondemo` both do this:

```js
import { appDir } from "vyto/os";

fn font_path(): string {
    return appDir() + "/assets/Inter-Regular.ttf";
}
```

```sh
./vytoc build apps/charts/charts.vt --release --with-assets \
    --target windows-x64 -o charts.exe
```

One path serves both cases. Unbundled, `appDir()` is the directory holding
the exe, so ship `assets/` beside it and the path resolves from any working
directory; bundled, the runtime VFS matches it by suffix and the font comes out
of the binary. `Canvas.loadFont` and
`GfxPainter` both consult the VFS before the disk. If you use `GfxPainter`,
ship the `-Medium` and `-Bold` siblings alongside the regular face — it derives
their paths from it.

By default the app ships as an exe plus `libblend2d.dll`, which `vytoc` copies
next to it. Adding `--bundle` links blend2d statically instead, for a single
file with nothing beside it — combine it with `--with-assets` and the whole app,
fonts included, is one exe that depends only on system DLLs.

### Testing on Windows

There is no Windows CI and no emulator in the loop — the binaries are checked on
a real machine:

```sh
make test-win        # cross-builds and stages tests/tmp/win-x64/
```

That stages one `.exe` per portable example and fixture, each with its golden, a
`manifest.txt`, and a generated `run.ps1`. It also runs one host-side test:
`tests/win/strptime_test.c` compiles `date_shim.c`'s `_WIN32` branch natively so
the hand-written strptime is verified without a Windows box.

Copy `tests/tmp/win-x64/` (or the `vyto-win-x64.zip` beside it) to a Windows
machine and run:

```powershell
powershell -ExecutionPolicy Bypass -File run.ps1
```

Then copy `results.txt` back. Goldens are resolved as
`<name>.expected.windows-x64` when present, falling back to `<name>.expected` —
that is how a program whose correct output genuinely differs per platform (such
as `examples/45_os.vt`) is handled.

## 9. Where to go next

- **[examples/](../examples/)** — `01_hello` … `53_datatable`, a guided tour of
  the language and stdlib (structs, classes, closures, FFI, generics, reactive
  state, files, time, JSON, HTTP, sockets, worker pools, and the UI toolkit).
- **Showcase apps** — [apps/charts](../apps/charts/charts.vt) (15-chart gallery),
  [apps/datagrid](../apps/datagrid/datagrid.vt) (spreadsheet-grade DataTable),
  [apps/vytopad](../apps/vytopad/vytopad.vt) (a text editor).
- **[Windows desktop apps](desktop.md)**: building, installing, signing and
  shipping a Windows app, step by step.
- **[Strings & regular expressions](strings.md)** — every string operation in
  one place: the built-in methods and their exact edge cases, `vyto/util/text`,
  the `vyto/regex` engine, and the Unicode-aware operations in
  `vyto/intl/unicode`.
- **[Types](types.md)** — every type in the language, value vs. reference,
  the fixed-width FFI family, arrays, `Map`, `struct`, and casts.
- **[Classes](classes.md)** — construction and `init` inheritance,
  `virtual`/`override`, `super`, `builder` methods, `deinit`, `weak`, checked
  downcasts, duck-typed `for-in`, and generic/arena-allocated classes.
- **[Memory](memory.md)** — the refcounting model, what it costs, the
  reference-cycle gotcha and `weak`, arenas, and the bulk-data allocation
  trap.
- **[Generating native bindings](native-bindings.md)** — turning a C header
  into a Vyto `extern "C"` module with `vytobind`.
- **[Math](math.md)** — `vyto/math` (constants, libm, integer helpers),
  `vyto/math/stats` and `vyto/math/random` (statistics and seeded
  randomness), and `vyto/math/algebra` (matrices, linear systems).
- **[Graphics](graphics.md)** — the drawing stack top to bottom: `vyto/geom`
  math, the `vyto/surface` and `vyto/gfx` rendering tiers, and what Vyto can
  and can't currently draw.
- **[Multiple windows](multiwindow.md)** — driving several OS windows from one
  process: the event loop that covers them all, monitors and placement,
  chromeless windows, and previewing one window inside another.
- **[Hardware & peripherals](hardware.md)** — cameras, sensors, GPIO, serial,
  USB and GPS through `vyto/hw/*`.
- **[README](../README.md)** — language highlights, why it's fast, the standard
  library, native packages, and platform status.
- **Run the tests** — `make test` builds every example and checks it against
  golden output; `make test-win` stages the Windows-portable slice for a real
  Windows machine (see section 8).
