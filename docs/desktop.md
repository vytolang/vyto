# Building and shipping Windows desktop apps

This guide takes you from nothing to a Windows app that you can hand to other
people: one `.exe`, an installer, and a signature. It assumes you know how to
use a terminal and nothing else. Every command here was run while writing it.

The short version: you write the app in Vyto on a **Linux** machine, `vytoc`
cross-compiles it into a Windows `.exe`, and the same command can wrap it in a
setup program. The result runs on **Windows 7 SP1 and later, 64-bit**, needs
no runtime or DLLs installed, and is about 1 MB.

## Contents

1. [What you will end up with](#1-what-you-will-end-up-with)
2. [Before you start](#2-before-you-start)
3. [Step 1: Write a small app](#3-step-1-write-a-small-app)
4. [Step 2: Run it on Linux while you work](#4-step-2-run-it-on-linux-while-you-work)
5. [Step 3: Build the Windows exe](#5-step-3-build-the-windows-exe)
6. [Step 4: Give it a name, a version and an icon](#6-step-4-give-it-a-name-a-version-and-an-icon)
7. [Step 5: Check that it will load on Windows 7](#7-step-5-check-that-it-will-load-on-windows-7)
8. [Step 6: Try it on Windows](#8-step-6-try-it-on-windows)
9. [Step 7: Make an installer](#9-step-7-make-an-installer)
10. [Step 8: Sign it](#10-step-8-sign-it)
11. [Files your app reads and writes](#11-files-your-app-reads-and-writes)
12. [Dialogs, message boxes and opening links](#12-dialogs-message-boxes-and-opening-links)
13. [Networking and HTTPS](#13-networking-and-https)
14. [Background services and app suites](#14-background-services-and-app-suites)
15. [Choosing how the app draws](#15-choosing-how-the-app-draws)
16. [Release checklist](#16-release-checklist)
17. [Troubleshooting](#17-troubleshooting)
18. [What is not available yet](#18-what-is-not-available-yet)
19. [Reference: the Windows flags of `vytoc build`](#19-reference-the-windows-flags-of-vytoc-build)

---

## 1. What you will end up with

| | |
|---|---|
| **The app** | `hello.exe`, about 1 MB. No DLLs beside it, nothing to install first. The Vyto runtime is compiled into it. |
| **Runs on** | Windows 7 SP1, 8, 8.1, 10 and 11, 64-bit. |
| **The installer** | `Hello-App-1.0.0-setup.exe`, about 2 MB: installs, upgrades, adds Start menu and desktop shortcuts, shows up in Programs and Features, and uninstalls cleanly. |
| **Optional** | An Authenticode signature, so Windows shows your company name instead of "Unknown publisher". |

**What has been tested, and where.** Everything in this guide was checked on
real Windows 7 SP1 hardware, and the installer was also checked under Wine.
The exe declares support for Windows 7 through 11 (so newer Windows treats it
correctly), but the project has not yet run its test suite on a Windows 10 or
11 machine. Test on the versions your users have before you ship.

---

## 2. Before you start

### 2.1 You need a Linux machine to build on

`vytoc` builds Windows apps **on Linux**. Building on Windows or macOS is not
supported, and the installer step relies on Linux specifically. This guide was
written on **Ubuntu 22.04**; the package names below are Ubuntu/Debian ones.
WSL (Linux inside Windows) is a Linux machine too, but it has not been tested.

You also need a **Windows machine to test on**: a real PC or a virtual
machine, 64-bit, Windows 7 SP1 or newer. 32-bit Windows and Windows on ARM are
not supported: the only Windows target is `windows-x64`.

### 2.2 Install the tools

Open a terminal and run:

```sh
sudo apt-get update
sudo apt-get install -y build-essential git make gcc-mingw-w64-x86-64
```

| Package | Why |
|---|---|
| `build-essential`, `make` | to build `vytoc` itself |
| `git` | to download the Vyto source |
| `gcc-mingw-w64-x86-64` | the C compiler that produces Windows programs. It also brings `binutils-mingw-w64-x86-64`, which contains `x86_64-w64-mingw32-windres`, the tool that puts the icon and version info into the exe |

Optional, only if you need them:

| Package | When |
|---|---|
| `osslsigncode` | to sign your app ([Step 8](#10-step-8-sign-it)) |
| `wine` | to start your exe on Linux for a quick look ([Step 6](#8-step-6-try-it-on-windows)). Not a replacement for testing on Windows |
| `g++-mingw-w64-x86-64` | only if your app draws with `vyto/gfx` (blend2d) instead of Direct2D ([Section 15](#15-choosing-how-the-app-draws)) |

Check the compiler is there:

```sh
x86_64-w64-mingw32-gcc --version
```

It should print a version line such as `x86_64-w64-mingw32-gcc (GCC) 10-win32`.

### 2.3 Build `vytoc`

```sh
git clone https://github.com/vytolang/vyto.git ~/vyto
cd ~/vyto
make
./vytoc --help
```

`make` builds `./vytoc` inside `~/vyto`. The last command should print the
usage text.

This guide writes `vytoc` on its own, so that the commands work from your
app's folder. For that, add the folder to your `PATH`, for this terminal and
for new ones:

```sh
echo 'export PATH="$HOME/vyto:$PATH"' >> ~/.bashrc
source ~/.bashrc
vytoc --help
```

Add the **folder**, as above. `vytoc` finds the standard library relative to
where it lives, so do not copy the `vytoc` file somewhere else on its own.

---

## 3. Step 1: Write a small app

Make a folder for the app. Everything from here on happens inside it.

```sh
mkdir -p ~/hello/assets
cd ~/hello
```

A Vyto desktop app is usually **three small files**:

```
hello/
  app.vt          the app itself: windows, buttons, logic. Shared by every platform
  main.vt         the Linux entry point: what you run while developing
  main_win.vt     the Windows entry point: what you build the exe from
```

The two entry points exist because each platform draws differently, and Vyto
has no "if Windows, import this" switch: whatever a file imports is linked in.
Keeping the platform choice in a tiny entry file keeps Linux-only code out of
the Windows exe, and the other way round. The app itself lives once, in
`app.vt`.

**`app.vt`**:

```js
// The app itself: the same on every platform.
import { Window, Column, Label, Button } from "vyto/ui";
import { message, MSG_INFO } from "vyto/desktop";

export fn buildApp(win: Window) {
    let hello = new Button("Say hello");
    hello.onClick = (b) => {
        message("Hello", "Hello from a Vyto app!", MSG_INFO);
    };
    win.root = new Column([
        new Label("My first Windows app"),
        hello,
    ]);
}
```

**`main.vt`** (Linux):

```js
// Linux entry: the toolkit's built-in renderer, nothing to install.
import { Window } from "vyto/ui";
import { buildApp } from "app";

fn main() {
    let win = new Window("Hello", 400, 240);
    buildApp(win);
    win.run();
}
```

**`main_win.vt`** (Windows):

```js
// Windows entry: draw with Direct2D (Windows' own text and ClearType).
import { Window } from "vyto/ui";
import { D2DPainter, d2d_usable } from "vyto/win/d2d";
import { buildApp } from "app";

fn main() {
    let win = new Window("Hello", 400, 240);
    if (d2d_usable(win.surface())) {
        win.use_painter(new D2DPainter(win.surface(), 15.0));
    }
    buildApp(win);
    win.run();
}
```

What the Windows entry does:

- `D2DPainter` draws through **Direct2D and DirectWrite**, which are part of
  Windows 7 and later. Text uses Segoe UI and the user's ClearType settings,
  so the app looks like a Windows app. `15.0` is the font size in pixels at
  100% display scaling; the painter adjusts it for 125%, 150% and so on by
  itself.
- `d2d_usable(...)` checks that Direct2D can actually draw on this machine.
  On a broken graphics driver, some remote-desktop sessions, or Wine, it
  cannot. When the check fails, the app keeps the toolkit's built-in renderer:
  plainer, but every button still works. **Keep this check.** Without it,
  `new D2DPainter` stops the app with an error on those machines.

---

## 4. Step 2: Run it on Linux while you work

```sh
vytoc run main.vt
```

A window opens with a label and a button; the button shows a message box
(through zenity or kdialog on Linux; if neither is installed, nothing appears).
Edit `app.vt`, run again. This is the fast loop: there is no need to build for
Windows after every change.

`vytoc` creates a `.vyto-cache/` folder next to your files. It holds generated
C and compiled objects so the next build is faster. It is safe to delete, and
it does not belong in version control. If you use git, add it to `.gitignore`:

```sh
echo ".vyto-cache/" >> .gitignore
```

---

## 5. Step 3: Build the Windows exe

```sh
vytoc build main_win.vt --target windows-x64 --release -o hello.exe
```

| Part | Meaning |
|---|---|
| `build main_win.vt` | compile, starting from the Windows entry file |
| `--target windows-x64` | produce a 64-bit Windows program instead of a Linux one |
| `--release` | optimise it and strip debug information. Always use it for anything you give to someone |
| `-o hello.exe` | the output file name. Without `-o`, the exe is named after the entry file |

The first build takes about 15 seconds on an 8-core machine; later builds
reuse the cache and take a few seconds. When it finishes, the last line it
prints is the file it wrote:

```
hello.exe
```

That `hello.exe` is the whole app. What `vytoc` did for you automatically:

- **No console window.** Because the app opens windows (it imports `vyto/ui`),
  it was linked as a GUI program, so double-clicking it does not open a black
  console window. A program that opens no windows is built as a console
  program instead. You can force either with `--gui` or `--console`.
- **No DLLs to ship.** The C runtime it uses (`msvcrt.dll`) is part of every
  Windows version, and everything else is linked in.
- **A Windows application manifest** is embedded. It tells Windows that the app
  supports Windows 7 to 11, that it needs no administrator rights (so no UAC
  prompt when it starts), and that it scales its own drawing on high-DPI
  displays (so text is sharp at 125% or 150%).
- **Unicode works everywhere.** File names, folder names, command-line
  arguments and typed text with any characters (`C:\Users\Zoë`, `日本語.txt`)
  reach your code as normal strings, Windows 7 included.
- **Security features** ASLR and DEP are switched on.

---

## 6. Step 4: Give it a name, a version and an icon

Right now Windows shows the exe with a generic icon and no details. You set
these with a **manifest program**: a small Vyto file that describes the app.
`vytoc` runs it while building and puts what it describes into the exe.

Put an icon in `assets/icon.png`. It can be a **PNG up to 256×256 pixels**
(`vytoc` converts it) or a ready-made **`.ico`** file. A square 256×256 PNG
gives the best results everywhere.

Create **`win_manifest.vt`**:

```js
import { WinManifest } from "vyto/win/manifest";

fn main() {
    let m = new WinManifest("Hello App")
        .version("1.0.0")
        .company("Example Ltd")
        .description("My first Windows app")
        .copyright("(c) 2026 Example Ltd")
        .icon("assets/icon.png");
    m.emit();
}
```

Build with `--manifest`:

```sh
vytoc build main_win.vt --target windows-x64 --release \
    --manifest win_manifest.vt -o hello.exe
```

Each setting, and where Windows shows it:

| Setting | Rules | Shown in |
|---|---|---|
| `new WinManifest("Hello App")` | the product name; must not be empty | Properties → Details, the installer, Programs and Features |
| `.version("1.0.0")` | 1 to 4 numbers separated by dots, each 0 to 65535 (`1`, `1.2`, `1.2.3`, `1.2.3.4`) | Properties → Details, the installer file name, Programs and Features. Without it, the installer uses `1.0.0` |
| `.company("Example Ltd")` | any text | Properties → Details, Programs and Features ("Publisher"). Also names the folder in Program Files (see [Step 7](#9-step-7-make-an-installer)) |
| `.description("...")` | any text | Task Manager's process list and Properties → Details |
| `.copyright("...")` | any text | Properties → Details |
| `.icon("assets/icon.png")` | a `.png` (up to 256×256) or an `.ico`. The path is **relative to the manifest file**, not to where you run `vytoc` | Explorer, the taskbar, the window's title bar, Start menu shortcuts |
| `.dpi("system")` | `"permonitor"` (the default), `"system"` or `"unaware"` | `"permonitor"`: sharp on every display; a `vyto/ui` app rescales itself when its window moves to a display with another scale (Windows 8.1 and later; Windows 7 and 8 treat it as `"system"`). `"system"`: sharp on the main display, stretched by Windows on others, for a `Surface` program that cannot redraw at a new scale. `"unaware"`: stretched everywhere, which looks blurry, for an app that draws pixel-sized content that breaks when scaled |

None of the text values may contain line breaks. A mistake does not produce a
broken exe: `m.emit()` checks everything first and the build stops with a
list of what is wrong, for example:

```
version "1.2.x" is not 1 to 4 numbers separated by dots, each 0..65535
```

These details are fixed when the exe is built because Windows reads them from
the file without starting the program. A window can still change its own icon
while running, with `win.set_icon(pixels, width, height)`.

---

## 7. Step 5: Check that it will load on Windows 7

A Windows program can build fine and still refuse to start on an older
Windows, with an error like "The procedure entry point ... could not be
located". That happens when it uses a system function the older Windows does
not have. `--audit` catches this at build time:

```sh
vytoc build main_win.vt --target windows-x64 --release \
    --manifest win_manifest.vt --audit -o hello.exe
```

It compares every system function the exe uses (and every DLL shipped beside
it) with a list taken from a real Windows 7 installation. When it passes, you
see a line like:

```
PASS pe_audit hello.exe (227 imports)
```

When it fails, it names each missing function and the build stops with:

```
hello.exe would not load on Windows 7 (see above); the import audit failed
```

You can also check any Windows exe or DLL, including ones not built by Vyto:

```sh
vytoc audit hello.exe something.dll
```

If all your users have Windows 10 or later, a failure here is less serious,
but it is worth knowing about. Using `--audit` on every release build costs
nothing.

---

## 8. Step 6: Try it on Windows

Copy `hello.exe` to the Windows machine: a USB stick, a shared folder, or
email all work. Double-click it.

You should see your window, with Windows' own font and no console window.
Clicking **Say hello** shows a standard Windows message box.

If Windows shows a blue **"Windows protected your PC"** box (SmartScreen),
that is normal for an unsigned program from the internet: click **More info**,
then **Run anyway**. Signing ([Step 8](#10-step-8-sign-it)) is what removes
this over time.

**A quick look with Wine (optional).** Wine runs Windows programs on Linux:

```sh
wine hello.exe
```

It is useful to catch an exe that does not start at all. It is **not** a
replacement for real Windows: Wine 6's Direct2D cannot draw, so under Wine the
app falls back to the built-in renderer (this is the `d2d_usable` check from
Step 1 doing its job). What you see there is not what Windows users see.

---

## 9. Step 7: Make an installer

You can ship `hello.exe` as it is: copying it anywhere and double-clicking it
works. Most users and IT departments expect an installer, though, and `vytoc`
makes one with **`--installer`**:

```sh
vytoc build main_win.vt --target windows-x64 --release --audit \
    --manifest win_manifest.vt --installer -o hello.exe
```

Next to `hello.exe` you now also have **`Hello-App-1.0.0-setup.exe`**: the
name from the manifest (spaces become dashes), the version, and `-setup.exe`.
That one file is what you give people. It contains the app, everything
`vytoc` ships beside it, and an uninstaller, compressed.

`--installer` only works with `--target windows-x64` and `vytoc build`.
Always give it a `--manifest`, so that the installer has a proper name and
version.

### 9.1 For all users, or just one

| | `--installer` | `--installer --per-user` |
|---|---|---|
| Installs to | `C:\Program Files\<company>\<name>`, or `C:\Program Files\<name>` without a company | `%LOCALAPPDATA%\Programs\<name>`, inside the user's own profile |
| Asks for administrator rights | yes, a UAC prompt when setup starts | no |
| Available to | every user of the PC | only the user who installed it |
| Can install a Windows service | yes | no: a service needs a per-machine install, so `vytoc` refuses the combination |

Use the default for most apps. Use `--per-user` when people cannot get
administrator rights (locked-down office PCs), or for tools that only one
person on a PC uses:

```sh
vytoc build main_win.vt --target windows-x64 --release --audit \
    --manifest win_manifest.vt --installer --per-user -o hello.exe
```

### 9.2 What the user sees

1. **Welcome:** the app's name, version and company, the install folder, and:
   - **Change…** to pick another folder. The app goes into its own subfolder
     there.
   - **Create a desktop shortcut**, ticked by default.
2. **Install:** a progress bar while the files are copied.
3. **Done:** a **Launch Hello App now** box (ticked by default), and Close.

The installer:

- copies the app into the install folder, plus `uninstall.exe` and
  `uninstall.ini` (the list of what it installed)
- adds a **Start menu** shortcut, and a desktop one if the box was ticked
- registers the app in **Programs and Features** (Settings → Apps on newer
  Windows), with its name, version, publisher, icon and size

### 9.3 Upgrades

Build a new version (raise `.version(...)` in the manifest) and run its setup
on a PC that has the old one. It installs **over the old copy, in the same
folder**. If the app is running, setup stops with:

```
Hello App is running. Close it, then run setup again.
```

Nothing is changed in that case. The user closes the app and runs setup
again.

### 9.4 Uninstalling

From Programs and Features (or by running `uninstall.exe` in the install
folder). The uninstaller asks for confirmation, then removes **exactly what
the installer put there**: the files, the shortcuts and the Programs and
Features entry. It never deletes the whole folder blindly, so a file the user
saved there stays. The app's own data in `%APPDATA%` / `%LOCALAPPDATA%`
([Section 11](#11-files-your-app-reads-and-writes)) is not touched either, so
reinstalling keeps the user's settings.

If the app is running, the uninstaller refuses in the same way as an upgrade.

### 9.5 Silent installs, for IT departments

Setup and the uninstaller accept switches, so admins can deploy your app
without clicking:

| Command | Does |
|---|---|
| `Hello-App-1.0.0-setup.exe /S` | install silently: no window, no questions |
| `... /S /D=D:\Apps\Hello` | install silently into that folder |
| `... /S /NODESKTOP` | install silently, without the desktop shortcut (silent installs create it otherwise) |
| `uninstall.exe /S` | uninstall silently |

A silent setup returns **exit status 0** when it succeeded and **1** when it
failed, printing the reason. It never launches the app. A silent uninstall
returns 0 as soon as the removal **has started**, not when it is finished
(the uninstaller has to delete itself, so it hands over to a copy of itself
in `%TEMP%`). That is the same behaviour as NSIS installers, which deployment
tools expect.

The per-machine setup still needs administrator rights when silent, so run it
from an elevated prompt or a deployment tool that has them.

### 9.6 What goes into the installer, exactly

- the exe you built (`-o hello.exe`)
- every file `vytoc` copied beside it, such as `cacert.pem` for HTTPS apps
  ([Section 13](#13-networking-and-https)) or `libblend2d.dll` for apps that
  draw with `vyto/gfx`
- the other programs of a suite, if the manifest declares any
  ([Section 14](#14-background-services-and-app-suites))

**Nothing else.** In particular, your `assets/` folder is **not** copied in.
If the app reads files from `assets/`, build with `--with-assets`, which puts
those files inside the exe itself. See [Section 11](#11-files-your-app-reads-and-writes).

To see the list, add `--verbose` and look for the `installer payload:` lines:

```
installer payload: hello.exe
```

The setup program is itself written in Vyto (`lib/vyto/win/setup`), so it
needs nothing but Windows 7 or later, passes `--audit`, and is signed along
with the app when you use `--sign`.

---

## 10. Step 8: Sign it

Windows warns about programs it cannot attribute to anyone. A **code-signing
certificate** attaches your verified company name to the exe:

- UAC prompts and SmartScreen show your name instead of **"Unknown
  publisher"**.
- SmartScreen's "Windows protected your PC" warning stops once the
  certificate has built up a reputation. That takes downloads over time; a
  signature alone does not remove it on day one.
- Antivirus programs are less likely to flag an unknown exe.

### 10.1 Get a certificate

Buy a code-signing certificate from a certificate authority (for example
DigiCert, Sectigo or SSL.com). There are two kinds:

| | OV (organisation validated) | EV (extended validation) |
|---|---|---|
| Cost | lower | higher |
| SmartScreen | reputation builds over time | historically trusted faster |
| Delivery | often a file, or a hardware token | a hardware token or cloud signing |

`vytoc` signs with a **PKCS#12 file** (`.pfx` or `.p12`) that contains the
certificate and its private key. If your certificate lives on a hardware
token or in a cloud signing service, the CA's own tools sign instead; this
`vytoc` flag cannot use those.

### 10.2 Install the signing tool

```sh
sudo apt-get install -y osslsigncode
```

### 10.3 Sign while building

```sh
export VYTO_SIGN_PASS='the-certificate-password'
vytoc build main_win.vt --target windows-x64 --release --audit \
    --manifest win_manifest.vt --installer \
    --sign /path/to/certificate.pfx \
    --timestamp http://timestamp.digicert.com \
    -o hello.exe
```

| Part | Meaning |
|---|---|
| `VYTO_SIGN_PASS` | the `.pfx` password. `vytoc` reads it from this environment variable and passes it on in a private file, never on a command line where other users could see it. Leave it unset if the file has no password |
| `--sign <file.pfx>` | signs `hello.exe`, every DLL shipped beside it, the setup exe and the uninstaller |
| `--timestamp <url>` | asks a timestamp server to record **when** it was signed, so the signature stays valid after the certificate expires. **Use it for everything you release.** It needs an internet connection. Your CA tells you its timestamp URL; the DigiCert one above works for any certificate |

Check a signature:

```sh
osslsigncode verify -in hello.exe
```

On Windows: right-click the exe → Properties → **Digital Signatures** tab.

### 10.4 Practising without a real certificate

A self-signed certificate lets you try the whole flow. Windows does **not**
trust it, so it gives users no benefit; never ship with one.

```sh
openssl req -x509 -newkey rsa:3072 -keyout key.pem -out cert.pem -days 365 -nodes \
    -subj "/CN=Example Ltd" -addext "extendedKeyUsage=codeSigning"
openssl pkcs12 -export -out test.pfx -inkey key.pem -in cert.pem -passout pass:secret

VYTO_SIGN_PASS=secret vytoc build main_win.vt --target windows-x64 --release \
    --manifest win_manifest.vt --installer --sign test.pfx -o hello.exe
osslsigncode verify -in hello.exe -CAfile cert.pem      # "Signature verification: ok"
```

### 10.5 Signing rules worth knowing

- Signing happens last, after the audit. A failed signing deletes the exe
  rather than leaving an unsigned one that looks finished.
- Leaving out `--sign` on the next build gives an unsigned exe, as expected.
- **Do not compress signed exes with UPX** or similar packers: Windows
  Defender treats packed programs as suspicious.

---

## 11. Files your app reads and writes

### 11.1 Files that ship with the app (images, fonts, data)

Put them in folders named **`assets/`**, **`conf/`** or **`storage/`** next to
the entry file (`main_win.vt`), and read them with `vyto/asset`:

```js
import { assetBytes } from "vyto/asset";

let b = assetBytes("assets/notes.txt");     // the file's bytes
let notes = strbytes(b, b.len);             // as text
```

Then build with **`--with-assets`**:

```sh
vytoc build main_win.vt --target windows-x64 --release --audit \
    --manifest win_manifest.vt --with-assets --installer -o hello.exe
```

`--with-assets` puts every file from those three folders **inside the exe**.
`assetBytes` finds them there first and falls back to the disk, so the same
code works in `vytoc run` on Linux (from the folders) and in the shipped exe
(from inside it). `readfile` and the image loaders check the embedded files
first too.

**With an installer, `--with-assets` is required** if the app needs those
files: the installer does not copy loose folders ([Section 9.6](#96-what-goes-into-the-installer-exactly)).
Without an installer, the alternative is to ship the `assets/` folder next to
the exe: `appDir()` (from `vyto/os`) is the folder that holds the exe, so
paths built from it work from anywhere.

### 11.2 Files the app creates (settings, documents, logs)

**Never write next to the exe.** After a normal install it lives in
`C:\Program Files\...`, which ordinary users cannot write to. Use the folders
Windows provides for this, from `vyto/desktop`:

| Call | On Windows | Use for |
|---|---|---|
| `configDir("hello")` | `%APPDATA%\hello` (e.g. `C:\Users\Zoë\AppData\Roaming\hello`) | settings. "Roaming": follows the user between PCs on a company network |
| `dataDir("hello")` | `%LOCALAPPDATA%\hello` | the app's own data, logs, databases |
| `cacheDir("hello")` | `%LOCALAPPDATA%\hello\cache` | anything that can be rebuilt if deleted |
| `sharedDataDir("hello")` | `%ProgramData%\hello` | data shared by all users and by background services ([Section 14](#14-background-services-and-app-suites)) |

They return the path only; **create the folder** before writing into it:

```js
import { dataDir } from "vyto/desktop";
import { file_mkdirs, file_open } from "vyto/io/file";

let dir = dataDir("hello");
file_mkdirs(dir);
let f = file_open(dir + "\\log.txt", "ab");
f.writeText("started\r\n");
f.close();
```

The name you pass (`"hello"`) becomes the folder name. Pick one that is
unlikely to clash with another program's, and keep it the same across
versions, or users lose their settings when they upgrade.

### 11.3 Seeing output from a GUI app

A GUI exe started by double-clicking has no console, so `print(...)` output
goes nowhere. Started from `cmd`, it does print there, but the prompt may
come back before the output arrives. For anything you need to read later,
write a log file in `dataDir(...)` as shown above.

---

## 12. Dialogs, message boxes and opening links

`vyto/desktop` gives your app Windows' own dialogs. They look and behave like
every other Windows program's, in the user's language.

```js
import { FileDialog, message, confirm, openUrl, openPath, dataDir, MSG_INFO } from "vyto/desktop";

let path = new FileDialog()
    .title("Open image")
    .filter("Images", "*.png;*.jpg")
    .owner(win.surface().native_window())   // keeps the dialog on top of your window
    .open();                                 // null when the user cancels
if (path != null) { /* use path */ }

if (confirm("Quit", "Discard changes?")) { win.close(); }
message("Saved", "Your file was saved.", MSG_INFO);
openUrl("https://example.com");              // opens the default browser
openPath(dataDir("hello"));                  // opens the folder in Explorer
```

| Call | Returns |
|---|---|
| `.open()` | one file the user picked, or `null` |
| `.openMany()` | several files; an empty list when cancelled |
| `.save("name.txt")` | a path to save to, or `null`. It asks before overwriting, but **it does not write anything**: your app writes the file to the returned path |
| `.folder()` | a folder, or `null` |
| `message(title, text, kind)` | `kind` is `MSG_INFO`, `MSG_WARN` or `MSG_ERROR` |
| `confirm(title, text)` | `true` only when the user clicks **Yes** |
| `openUrl(url)` | opens `http:`, `https:` and `mailto:` links only; anything else is refused |
| `openPath(path)` | opens a file or folder as a double-click would. Given an `.exe` it **runs it**, so only pass paths your app chose or the user picked |

Dialogs wait for the user's answer before your code continues. All of them
work on Windows 7 and pass `--audit`. Full details:
[`lib/vyto/desktop/README.md`](../lib/vyto/desktop/README.md).

### 12.1 A menu bar

`vyto/desktop/menu` gives the window a real Windows menu bar — the same one
Notepad has, drawn by Windows, with Alt+letter, F10, the arrow keys and the
user's theme all working the way they do everywhere else. On Linux the same
code shows `vyto/ui`'s drawn menu bar, which behaves the same way.

```js
import { AppMenu } from "vyto/desktop/menu";

let menus = new AppMenu(win);

let file = menus.file();                            // "File"
file.item("&New", "Ctrl+N", () => newDocument());
file.item("&Open…", "Ctrl+O", () => openDocument());
file.item("&Save", "Ctrl+S", () => save());
file.separator();
menus.addQuit(file);                                // Exit (Windows) / Quit (Linux)

menus.addClipboard(menus.edit());                   // Cut, Copy, Paste, Select All
menus.search().item("&Find…", "Ctrl+F", () => find());
menus.settings().check("&Dark Theme", "", false, (on) => setDark(on));
menus.addFullScreen(menus.window());                // F11
menus.addAbout(menus.help(), "About Hello", "Hello 1.0");

menus.attach();
win.run();
```

- **`&` picks the underlined letter**: `"&Open…"` is opened with Alt, then O.
  Write `&&` for a real `&`.
- **The shortcut you write is the shortcut you get.** `"Ctrl+S"` is shown
  beside the item *and* fires it from anywhere in the window. To show one
  without binding it, call `.hint("Ctrl+S")` on the item instead.
- **Keep the item to change it later**: `let save = file.item(...)`, then
  `save.set_enabled(false)`, `item.set_checked(true)` or `item.set_label("…")`.
  The Windows menu updates at once.
- **Add every menu and item before `attach()`.** The structure is copied into
  Windows then; only the state above can change afterwards.
- `attach()` also stops Escape from closing the window — in an app with a menu
  bar, Escape backs out of menus and dialogs, and File > Exit quits.
- An app with its own dark theme can call `menus.drawn()` before `attach()` to
  use the drawn bar on Windows too: Windows 7 has no dark menu bar.

Full details: [`lib/vyto/desktop/menu/README.md`](../lib/vyto/desktop/menu/README.md).

---

## 13. Networking and HTTPS

`vyto/net/http` works on Windows, **HTTPS included, on Windows 7 through 11**.
It does not use Windows' own TLS: curl and mbedTLS are built into your exe,
so TLS 1.2 and 1.3 work the same on every version.

What changes for you:

- **The first Windows build of a networked app takes longer**, about 25
  seconds more on 8 cores, while curl and mbedTLS compile. Later builds reuse
  them.
- **`cacert.pem` appears next to your exe.** It is the standard list of
  trusted certificate authorities (from Mozilla). The app trusts those **plus**
  the ones in Windows' own store. Ship the file with the app: a Windows PC
  that rarely uses the internet may not have fetched common roots yet, and
  without `cacert.pem` HTTPS to those sites would fail there. `--installer`
  includes it automatically; if you ship the bare exe, copy `cacert.pem` with
  it.
- To trust only your own certificate list, call `http_set_ca_file(path)`.
  Certificate checking cannot be turned off.

---

## 14. Background services and app suites

A **Windows service** is a program that Windows starts by itself, usually at
boot, and keeps running without anyone signed in: a sync job, a device
monitor, a payment listener. `vyto/desktop/service` turns a Vyto program into
one, and the installer registers it.

Services need a **per-machine** install (no `--per-user`).

### 14.1 One program that is a service

The service's code:

```js
import { Service } from "vyto/desktop/service";

fn main() {
    let svc = new Service("HelloSync");
    svc.run(() => {
        svc.log("started");
        while (!svc.stopping()) {
            // do one round of work here
            svc.wait(5000);          // sleeps 5 s, but wakes at once when Windows says stop
        }
        svc.log("stopped");
    });
}
```

Declare it in that program's manifest with `.service(name, display name,
description, start)`:

```js
new WinManifest("Hello Sync").version("1.0.0").company("Example Ltd")
    .service("HelloSync", "Hello Sync", "Keeps Hello's data in sync", "auto")
    .emit();
```

| `start` | Meaning |
|---|---|
| `"auto"` | starts at boot |
| `"delayed"` | starts at boot, a little after the rest, so boot is not slowed |
| `"manual"` | starts only when something starts it |

Built with `--installer`, setup registers the service, starts it, stops it
during an upgrade and removes it on uninstall. The service name (`HelloSync`)
may only use letters, digits, `-`, `_` and `.`.

Things to know about services:

- **It runs as Windows' system account (LocalSystem)**, not as the signed-in
  user. That means `dataDir()` inside the service is the system account's
  folder, not the user's. Keep a service's files in `sharedDataDir("...")`
  (`%ProgramData%`).
- **`svc.log(...)`** writes to `%ProgramData%\<name>\<name>.log`
  (here `C:\ProgramData\HelloSync\HelloSync.log`).
- **Debugging:** run the same exe from a terminal and it runs the same loop in
  the foreground, logging to the terminal. Press Ctrl+C to stop it.
  `svc.isService()` tells the code which way it was started.
- To check it on Windows, open `services.msc` (press Win+R, type
  `services.msc`): your service is listed by its display name with its status.

### 14.2 A suite: a GUI plus background services, one installer

Most real products are a GUI with services behind it. The manifest of the
**GUI** declares the other programs and their services; `--installer` builds
all of them and installs every exe **into the same folder**:

```js
new WinManifest("Vyto Café POS Suite").version("0.2.0").company("Vyto")
    .program("payments", "payments.vt")     // builds payments.exe, installed beside pos.exe
    .program("stock", "stock.vt")
    .serviceFor("payments", "VytoCafePayments", "Vyto Café Payments", "Simulated M-Pesa payments", "auto")
    .serviceFor("stock", "VytoCafeStock", "Vyto Café Stock", "Stock levels", "auto")
    .dependsOn("VytoCafePayments", "VytoCafeStock")
    .emit();
```

| Call | Meaning |
|---|---|
| `.program(id, "file.vt")` | build `file.vt` into `<id>.exe`. The id uses only `a-z`, `0-9`, `-` and `_`. The path is relative to the manifest |
| `.serviceFor(id, name, display, description, start)` | run program `id` as a service, as in 14.1 |
| `.dependsOn(a, b)` | service `a` needs service `b`: Windows starts `b` first and will not stop `b` while `a` runs. Setup stops and starts them in that order |

The manifest is checked as a whole: a duplicate id, a service for a program
that does not exist, or services that depend on each other in a circle stop
the build with the reason.

The extra programs are built with the same target, `--release`, manifest
identity, `--audit` and `--sign` as the main exe. They are **not** built with
`--with-assets` or `--bundle`, so a service should not depend on embedded
assets.

**How the GUI talks to its services:** `vyto/desktop/channel`. The GUI sends a
request and gets a reply; a service can also push events to every connected
GUI. It only listens on the PC itself (never the network), on a port Windows
picks when the service starts, so it can never collide with another program.
The GUI finds the port by itself, and reconnects when a service restarts.

In outline (`record`, `body` and `svc` stand for your own values; the full
code is in `apps/pos_suite`):

```js
// in the service
let srv = new ChannelServer("vyto-cafe-suite", "payments");
srv.handle("pay.simulate", (r) => { return record; });   // the reply
srv.push("payment.received", record);                     // to every connected GUI
while (!svc.stopping()) { srv.poll(250); }

// in the GUI
let ch = new Channel("vyto-cafe-suite", "payments", "till-1");
ch.onState((up) => { /* show "Payments online" or "offline" */ });
ch.request("pay.simulate", body, (r) => { /* r.ok, r.data, r.err */ });
attachChannel(win, ch);   // from vyto/desktop/channel/gui: the window wakes only when a message arrives
```

The complete, working example is **`apps/pos_suite`**: a till with a payments
service and a stock service, one installer, all three exes in one folder. Read
its `README.md`, and [`lib/vyto/desktop/channel/README.md`](../lib/vyto/desktop/channel/README.md)
for the details (time-outs, security, the message format).

---

## 15. Choosing how the app draws

`vyto/ui` widgets draw through a **painter**. Three work on Windows:

| | Direct2D (`D2DPainter`) | blend2d (`GfxPainter`) | built-in (no painter) |
|---|---|---|---|
| Looks | native Windows text (Segoe UI, ClearType) | smooth, identical on every platform; your own font | plainer: rich effects such as shadows and gradients are simplified |
| Extra files | none | `libblend2d.dll` beside the exe, or `--bundle` to link it in | none |
| Your own font files | not yet; installed system fonts only | yes | no |
| Memory | about 3.5 MB more than blend2d (measured on Windows 7) | the baseline | not measured |
| Extra setup on the build machine | none | build blend2d for Windows once (below) | none |
| Use it for | most Windows apps | apps that need their own typeface or pixel-identical output everywhere | a fallback |

**Direct2D** is what this guide uses. Measured on Windows 7 it draws text and
images about 4× faster than blend2d and shapes about 2× slower; a typical
widget screen comes out about even. The first launch after a reboot is slower
to draw (around a quarter of a second) while Windows builds its font cache.
Details and caveats: [`lib/vyto/win/d2d/README.md`](../lib/vyto/win/d2d/README.md).

**blend2d** needs a one-time build of the library for Windows, which needs the
C++ cross-compiler and a few build tools:

```sh
sudo apt-get install -y g++-mingw-w64-x86-64 cmake ninja-build
~/vyto/lib/vyto/gfx/native/build-blend2d.sh windows-x64
```

Then, in the Windows entry file, use `GfxPainter` from `vyto/gfx/painter`
instead of `D2DPainter`. The painter needs a font file: ship one with
`--with-assets` ([Section 11.1](#111-files-that-ship-with-the-app-images-fonts-data)).
[getting-started.md §8](getting-started.md#graphics-apps-blend2d-and-fonts)
covers the details.

**Never import both** `vyto/win/d2d` and `vyto/gfx` in the same program:
whatever is imported is linked, so the Windows exe would carry blend2d
without using it. This is the reason for the separate entry files in Step 1.

**Colours are ARGB.** If you draw your own shapes, a colour is written
`0xAARRGGBB`, where `AA` is the opacity. `0x2F6FDE` has opacity `00` and draws
**nothing**; write `0xFF2F6FDE`, or use `rgb(47, 111, 222)` from
`vyto/surface`.

---

## 16. Release checklist

Before you give a version to users:

1. Raise `.version(...)` in the manifest. Upgrades and Programs and Features
   rely on it.
2. Build with everything on:

   ```sh
   export VYTO_SIGN_PASS='...'
   vytoc build main_win.vt --target windows-x64 --release --audit \
       --manifest win_manifest.vt --with-assets --installer \
       --sign certificate.pfx --timestamp http://timestamp.digicert.com \
       -o hello.exe
   ```

   (Leave out `--with-assets` if the app has no `assets/`, `conf/` or
   `storage/` folder.)
3. Check the audit printed `PASS` and the build ended with
   `installer: ./<Name>-<version>-setup.exe`.
4. On a Windows test machine (ideally one that never had the app):
   - run the setup, launch the app from the Start menu, use its main features
   - run the **previous** version's setup first, then the new one, to test the
     upgrade
   - uninstall from Programs and Features and check the folder and shortcuts
     are gone
   - if you support Windows 7, do this on Windows 7 too
5. Check the signature: Properties → Digital Signatures on the setup exe.
6. Keep the exact setup exe you tested; that is the file you publish.

---

## 17. Troubleshooting

### Build errors

| Message | Cause and fix |
|---|---|
| `cross compiler 'x86_64-w64-mingw32-gcc' not found — install it or pass --cc/VYTO_CC` | The Windows compiler is missing: `sudo apt-get install -y gcc-mingw-w64-x86-64` |
| `--installer makes a Windows setup.exe; this build targets linux-x64` | Add `--target windows-x64`. The same applies to `--manifest`, `--audit` and `--sign` |
| `--per-user chooses the installer's scope: pass --installer too` | `--per-user` only means something with `--installer` |
| `... declares a Windows service, and installing a service needs a per-machine installer: drop --per-user` | Services need the all-users install |
| `--sign needs osslsigncode (apt install osslsigncode, or set VYTO_OSSLSIGNCODE)` | Install it: `sudo apt-get install -y osslsigncode` |
| `--sign: no certificate at <path>` | The `.pfx` path is wrong; it is relative to the folder you run `vytoc` from |
| `signing <file> failed (osslsigncode exit 255)` | Usually a wrong or missing `VYTO_SIGN_PASS`, or (with `--timestamp`) no internet connection. The unsigned exe is deleted, so nothing half-finished is left behind |
| `--timestamp stamps a signature: pass --sign too` | `--timestamp` only works together with `--sign` |
| `... would not load on Windows 7 (see above); the import audit failed` | Something in the program uses a function Windows 7 lacks; the lines above name it. Usually a package you added, or your own C code |
| `the --manifest program <file> failed (its error is above)` | The manifest program stopped, usually because `emit()` found a problem. The lines above list each one |
| `package 'vyto_gfx' ships prebuilt native libraries, but none for windows-x64` | The app imports `vyto/gfx`: build blend2d for Windows ([Section 15](#15-choosing-how-the-app-draws)), or switch to `D2DPainter` |
| `two programs of the suite ship different files named <name>` | Two programs of a suite produce a different file with the same name; rename one |

### On Windows

| Symptom | Cause and fix |
|---|---|
| "Windows protected your PC" | SmartScreen and an unsigned (or newly signed) exe. Users click **More info → Run anyway**; signing removes it as the certificate gains reputation ([Step 8](#10-step-8-sign-it)) |
| A black console window opens with the app | The program does not import `vyto/ui` or `vyto/surface`, so it was built as a console program. Add `--gui` |
| The app starts and immediately closes | Run it from `cmd` to see its error message. Common causes: a file it expected in `assets/` that was not embedded (`--with-assets`), or writing next to the exe in Program Files |
| Text and shapes look blurry | The manifest has `.dpi("unaware")`, or `.dpi("system")` and the window was moved to a monitor with a different scale |
| Setup says "`<App>` is running. Close it, then run setup again." | Close the app (check the taskbar's hidden icons and Task Manager) and run setup again |
| Settings are lost after an upgrade | The name passed to `configDir`/`dataDir` changed between versions. Keep it fixed |
| HTTPS fails on some PCs | `cacert.pem` is not next to the exe. Use `--installer`, or ship the file with the bare exe |
| My own shapes are invisible | The colour has no opacity: write `0xFFRRGGBB`, not `0xRRGGBB` ([Section 15](#15-choosing-how-the-app-draws)) |

---

## 18. What is not available yet

So you can plan around them:

- **Building on Windows or macOS.** Builds run on Linux only.
- **32-bit Windows and Windows on ARM.** Only 64-bit x86 (`windows-x64`).
- **Testing on Windows 10 and 11.** Declared supported, but the project's
  own tests so far ran on Windows 7 SP1 and under Wine.
- **Moving between monitors with different scaling, verified.** The window
  rescales itself on Windows 8.1 and later (theme, painter font, layout), but
  this has only been checked headless so far, not on a real two-display
  Windows 10 machine. Sizes an app computes once at startup in raw pixels do
  not follow; compute them through `Widget.dp()` or the theme.
- **Your own font files with Direct2D.** Use system fonts, or `GfxPainter`.
- **A tray icon, single-instance apps, and start at sign-in.** No API for
  these yet.
- **File associations** (opening `.myext` files with your app) are not set up
  by the installer.
- **Automatic updates.** Ship a new setup exe; running it upgrades in place.
- **MSI packages.** The installer is an exe, with the silent switches IT tools
  use ([Section 9.5](#95-silent-installs-for-it-departments)).
- **`vyto/intl`** (ICU-based formatting and translation) does not build for
  Windows yet.
- **Linux-only packages:** `vyto/net/link`, `wifi` and `raw` do not build for
  Windows, and `vyto/hw`'s camera, sensors, gpio, i2c and spi return nothing
  there (serial ports, HID devices, keyboards and mice, USB listing, power and
  device events do work). The full list of what is portable is in
  [getting-started.md §8](getting-started.md#what-is-portable-today).

---

## 19. Reference: the Windows flags of `vytoc build`

| Flag | Does |
|---|---|
| `--target windows-x64` | build for 64-bit Windows |
| `--release` | optimised, stripped build. Use it for everything you ship |
| `-o <file.exe>` | output file. The setup exe is written into the same folder |
| `--manifest <file.vt>` | name, version, company, icon, DPI mode and services, from a `WinManifest` program |
| `--audit` | fail the build if the exe or a DLL beside it would not load on Windows 7 |
| `--installer` | also build `<Name>-<version>-setup.exe` |
| `--per-user` | with `--installer`: install for the current user only, without administrator rights |
| `--sign <file.pfx>` | Authenticode-sign the exe, its DLLs, the setup and the uninstaller. Password from `$VYTO_SIGN_PASS` |
| `--timestamp <url>` | with `--sign`: timestamp the signature so it outlives the certificate |
| `--with-assets` | embed `assets/`, `conf/` and `storage/` (next to the entry file) in the exe |
| `--bundle` | link prebuilt libraries (blend2d) into the exe instead of shipping DLLs |
| `--gui` / `--console` | force a GUI program (no console window) or a console program. By default a program that imports `vyto/surface` (including through `vyto/ui`) is a GUI program |
| `--verbose` | print every compiler command, and the installer's file list |
| `--clean` | throw away this app's cache and rebuild from scratch |

Other commands:

| Command | Does |
|---|---|
| `vytoc audit <file.exe> [<file.dll> ...]` | the Windows 7 check on any exe or DLL |
| `vytoc run <file.vt>` | build and run on Linux, for development |

Further reading:

- [getting-started.md §8](getting-started.md#8-cross-compiling-for-windows):
  more depth on the Windows build, portability and graphics
- [`lib/vyto/win/d2d/README.md`](../lib/vyto/win/d2d/README.md): the Direct2D
  painter
- [`lib/vyto/desktop/README.md`](../lib/vyto/desktop/README.md): dialogs and
  folders
- [`lib/vyto/desktop/service/README.md`](../lib/vyto/desktop/service/README.md):
  services, including systemd on Linux
- [`lib/vyto/desktop/channel/README.md`](../lib/vyto/desktop/channel/README.md):
  GUI ↔ service messaging
- `apps/pos` (one exe) and `apps/pos_suite` (a GUI with two services): complete
  apps built exactly this way
