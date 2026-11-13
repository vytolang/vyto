#!/bin/sh
# Cross-builds the Windows-portable slice of the suite for windows-x64 and stages
# it, with its goldens and a self-checking runner, into tests/tmp/win-x64/.
#
# This script does NOT run anything Windows — the .exe files are meant to be
# copied to a real Windows machine and checked there with run.ps1 (which this
# script generates). Copy results.txt back to see what passed.
#
# Scope: core language + runtime + every stdlib package that has a Windows
# story. That is most of them: vyto/surface, vyto/ui and vyto/gfx (blend2d
# cross-builds), vyto/net/* (Winsock + curl and mbedTLS built from vendored source) and vyto/os/worker
# (a re-exec pool rather than fork) are all staged here.
#
# Only vyto/hw/* (Linux device interfaces) and vyto/intl (ICU) are excluded —
# see docs/getting-started.md.
set -u
cd "$(dirname "$0")/.."

CROSS_CC=x86_64-w64-mingw32-gcc
TRIPLE=windows-x64
STAGE=tests/tmp/win-x64
# vytoc links Windows exes with -static-libgcc itself now, so the staged exes
# need no libgcc_s_*.dll on the target box without any flag here.
CC_CMD="$CROSS_CC"

if ! command -v "$CROSS_CC" >/dev/null 2>&1; then
    echo "FATAL: $CROSS_CC not found — install it with:"
    echo "  sudo apt-get install -y gcc-mingw-w64-x86-64"
    exit 1
fi

if [ ! -x ./vytoc ]; then
    echo "FATAL: ./vytoc not built — run 'make' first"
    exit 1
fi

rm -rf "$STAGE"
mkdir -p "$STAGE"
fail=0

# --- host-side unit test of the Windows-only date_shim branch -------------
# Windows has no strptime, so date_shim.c carries its own. That parser is the
# one piece of the port whose behaviour can be verified without a Windows box,
# by compiling the _WIN32 branch here with the MSVC names shimmed in.
if cc -O1 -Ilib/vyto/util/native/src -o tests/tmp/strptime_test \
       tests/win/strptime_test.c 2>/dev/null; then
    if TZ=UTC tests/tmp/strptime_test > tests/tmp/strptime_test.out 2>&1; then
        echo "PASS win_strptime"
    else
        echo "FAIL win_strptime"
        cat tests/tmp/strptime_test.out
        fail=1
    fi
else
    echo "FAIL win_strptime (harness did not compile)"
    fail=1
fi

# --- prebuilt-library deploy path: greeter as a Windows DLL ---------------
# 09_prebuilt_so pulls native/<triple>/*.dll, links it, and vytoc copies it next
# to the exe. Nothing else in the tree covers the .dll arm of that logic.
mkdir -p "examples/greeter/native/$TRIPLE"
if ! $CROSS_CC -shared -O2 -o "examples/greeter/native/$TRIPLE/libgreeter.dll" \
        examples/greeter/csrc/greeter.c; then
    echo "FAIL build greeter.dll"
    fail=1
fi

# --- the program manifest ------------------------------------------------
# src|name|mode|args|env|buildflags
#   mode  eq        stdout+stderr merged, exact match against <name>.expected
#         out       stdout only (the program shells out and cmd.exe writes its
#                   own diagnostics to stderr, which are not ours to golden)
#         contains  stdout+stderr merged, <name>.expected holds a substring
#   args        extra argv, space-separated
#   env         K=V pairs set for the run, space-separated
#   {uni} in args or env is replaced by run.ps1 with the non-ASCII string
#   "Zöë日", built from [char] codes: PowerShell 2.0 reads a BOM-less script
#   and manifest in the ANSI code page, so the literal cannot travel in them.
#   buildflags  extra vytoc flags for the cross-build
programs=$(cat <<'EOF'
examples/01_hello.vt|01_hello|eq|
examples/02_structs.vt|02_structs|eq|
examples/03_classes.vt|03_classes|eq|
examples/04_closures.vt|04_closures|eq|
examples/05_widgets.vt|05_widgets|eq|
examples/06_ffi.vt|06_ffi|eq|
examples/07_modules.vt|07_modules|eq|
examples/08_native_pkg.vt|08_native_pkg|eq|
examples/09_prebuilt_so.vt|09_prebuilt_so|eq|
examples/10_strings.vt|10_strings|eq|
examples/11_callbacks.vt|11_callbacks|eq|
examples/12_math.vt|12_math|eq|
examples/13_overflow.vt|13_overflow|eq|
examples/19_builder.vt|19_builder|eq|
examples/20_defaults.vt|20_defaults|eq|
examples/21_structmethods.vt|21_structmethods|eq|
examples/22_consts.vt|22_consts|eq|
examples/23_num_methods.vt|23_num_methods|eq|
examples/23_string_const.vt|23_string_const|eq|
examples/24_string_methods.vt|24_string_methods|eq|
examples/25_array_methods.vt|25_array_methods|eq|
examples/26_array_hof.vt|26_array_hof|eq|
examples/27_map_methods.vt|27_map_methods|eq|
examples/28_generics.vt|28_generics|eq|
examples/29_generic_classes.vt|29_generic_classes|eq|
examples/30_reactive.vt|30_reactive|eq|
examples/31_generic_sort.vt|31_generic_sort|eq|
examples/31_reactive_mem.vt|31_reactive_mem|eq|
examples/40_fmt.vt|40_fmt|eq|
examples/41_json.vt|41_json|eq|
examples/42_file.vt|42_file|eq|
examples/43_time.vt|43_time|eq|
examples/44_date.vt|44_date|eq|
examples/45_os.vt|45_os|out|
examples/53_datatable.vt|53_datatable|eq|
examples/83_regex.vt|83_regex|eq|
tests/fixtures/regex_match.vt|regex_match|eq|
tests/fixtures/regex_match.vt|regex_match_nojit|eq||VYTO_REGEX_JIT=0
examples/84_regex_tools.vt|84_regex_tools|eq|
tests/fixtures/regex_extra.vt|regex_extra|eq|
tests/fixtures/regex_extra.vt|regex_extra_nojit|eq||VYTO_REGEX_JIT=0
examples/85_validator.vt|85_validator|eq|
tests/fixtures/validator.vt|validator|eq|
tests/fixtures/args_echo.vt|args_echo|eq|alpha beta
tests/fixtures/unicode_paths.vt|unicode_paths|eq|{uni}|VYTO_UNI_IN={uni}
tests/fixtures/anim_clock.vt|anim_clock|eq|
tests/fixtures/condlink_win.vt|condlink_win|eq|
tests/fixtures/listdir.vt|listdir|eq|
tests/fixtures/overflow_trap.vt|overflow_trap|contains|
examples/46_socket.vt|46_socket|eq|
examples/47_http.vt|47_http|eq|
examples/48_websocket.vt|48_websocket|eq|
examples/49_poll_server.vt|49_poll_server|eq|
examples/50_worker_pool.vt|50_worker_pool|eq|
apps/motiondemo/motiondemo.vt|motiondemo|contains||VS_HEADLESS=1 VS_EVENTS=motiondemo.events|--with-assets
tests/win/d2d_props.vt|d2d_props|eq||VS_HEADLESS=1
tests/fixtures/win_gui_app.vt|win_gui_app|eq|||--manifest tests/fixtures/win_gui_manifest.vt
tests/fixtures/win_console_app.vt|win_console_app|eq|
tests/win/desktop_dirs_win.vt|desktop_dirs_win|eq|
tests/fixtures/mkdirs_backslash.vt|mkdirs_backslash|eq|
tests/win/worker_win.vt|worker_win|eq|
tests/win/bigfile_win.vt|bigfile_win|eq|
EOF
)

# args_echo, overflow_trap and listdir have no .expected on disk — run_tests.sh
# inlines all three. Materialize them so the staged bundle is self-describing.
mkdir -p tests/tmp
printf 'n=2\nalpha\nbeta\n' > tests/tmp/args_echo.expected
printf "integer overflow in '+'\n" > tests/tmp/overflow_trap.expected
# vt_dir_list skips "." but keeps "..", and sorts with strcmp, on both platforms
printf '.. (dir)\na.txt (file)\nb.txt (file)\nsub (dir)\n' > tests/tmp/listdir.expected
# motiondemo is the suite's only blend2d program: it renders 13 animated tiles
# headless and says so. Same assertion run_tests.sh makes on Linux.
printf 'motion gallery ready\n' > tests/tmp/motiondemo.expected

# Resolve a program's golden: per-target override wins over the shared one.
expected_for() {
    _src=$1
    _name=$2
    _base=${_src%.vt}
    if [ -f "$_base.expected.$TRIPLE" ]; then echo "$_base.expected.$TRIPLE"
    elif [ -f "$_base.expected" ]; then echo "$_base.expected"
    elif [ -f "tests/tmp/$_name.expected" ]; then echo "tests/tmp/$_name.expected"
    else echo ""
    fi
}

printf '%s\n' "$programs" | while IFS='|' read -r src name mode args env bflags; do
    [ -n "$src" ] || continue
    exp=$(expected_for "$src" "$name")
    if [ -z "$exp" ]; then
        echo "FAIL $name (no golden for $src)"
        echo x >> "$STAGE/.failed"
        continue
    fi
    # $bflags is deliberately unquoted: it carries zero or more vytoc flags.
    # shellcheck disable=SC2086
    if ./vytoc build "$src" --target "$TRIPLE" --cc "$CC_CMD" $bflags \
            -o "$STAGE/$name.exe" >/dev/null; then
        cp "$exp" "$STAGE/$name.expected"
        # a scripted-event program needs its script staged beside the exe
        case "$env" in
            *VS_EVENTS=*)
                evf=${env##*VS_EVENTS=}
                evf=${evf%% *}
                cp "$(dirname "$src")/$evf" "$STAGE/$evf" 2>/dev/null ||
                    echo "WARN $name: no events file $evf beside $src"
                ;;
        esac
        printf '%s|%s|%s|%s\n' "$name" "$mode" "$args" "$env" >> "$STAGE/manifest.txt"
        echo "BUILT $name"
    else
        echo "FAIL $name (cross-build failed)"
        echo x >> "$STAGE/.failed"
    fi
done

# the loop above runs in a subshell (pipe), so failures come back via a file
if [ -f "$STAGE/.failed" ]; then fail=1; rm -f "$STAGE/.failed"; fi

# --- Windows 7 import audit ------------------------------------------------
# dump_exports.exe lists a real Windows install's system DLL exports; run.ps1
# runs it, and its win7_exports.txt from a Windows 7 box is committed as
# lib/vyto/win/win7_exports.txt. `vytoc audit` then checks every staged binary
# against that list here, before anything is copied to Windows: an import
# Windows 7 lacks fails the exe at load with "entry point not found".
if $CROSS_CC -O2 -o "$STAGE/dump_exports.exe" tests/win/dump_exports.c; then
    echo "BUILT dump_exports"
else
    echo "FAIL build dump_exports"
    fail=1
fi
# The input demo: a manual check on Windows (README there), and the program
# run_tests_wine.sh drives with xdotool. Not in the manifest: it waits for a
# person or a script, it does not print-and-exit.
if ./vytoc build tests/win/input_demo.vt --target "$TRIPLE" --cc "$CC_CMD" \
        -o "$STAGE/input_demo.exe" >/dev/null 2>"$STAGE/.input_demo.err"; then
    echo "BUILT input_demo"
    rm -f "$STAGE/.input_demo.err"
else
    echo "FAIL build input_demo"
    cat "$STAGE/.input_demo.err"
    fail=1
fi
# vyto/desktop's dialogs one after another (desktop_demo) and HTTPS against a
# local test server (https_local): both need something on the other end — a
# person or xdotool, an openssl s_server — so they are staged, not in the
# manifest. run_tests_wine.sh drives both; the README says how by hand.
# The channel's Windows side: its test server and client, and the GUI probe
# whose idle wakeups run_tests_wine.sh counts (the Win32 socket wait).
mkdir -p "$STAGE/channel"
for t in srv cli gui_probe; do
    if ./vytoc build "tests/fixtures/channel/$t.vt" --target "$TRIPLE" --cc "$CC_CMD" \
            -o "$STAGE/channel/$t.exe" >/dev/null 2>"$STAGE/.ch_$t.err"; then
        rm -f "$STAGE/.ch_$t.err"
    else
        echo "FAIL build channel/$t"; cat "$STAGE/.ch_$t.err"; fail=1
    fi
done
for t in desktop_demo https_local service_demo; do
    if ./vytoc build "tests/win/$t.vt" --target "$TRIPLE" --cc "$CC_CMD" \
            -o "$STAGE/$t.exe" >/dev/null 2>"$STAGE/.$t.err"; then
        echo "BUILT $t"
        rm -f "$STAGE/.$t.err"
    else
        echo "FAIL build $t"
        cat "$STAGE/.$t.err"
        fail=1
    fi
done
# Installers (vytoc --installer): a per-machine one for an app that is also a
# service, and a per-user one. run_tests_wine.sh installs, upgrades and
# uninstalls both silently; the README says how to check them by hand.
mkdir -p "$STAGE/inst/machine" "$STAGE/inst/user"
if ./vytoc build tests/win/service_demo.vt --target "$TRIPLE" --cc "$CC_CMD" --release \
        --manifest tests/win/service_demo_manifest.vt --installer \
        -o "$STAGE/inst/machine/service_demo.exe" >/dev/null 2>"$STAGE/.inst.err" &&
   ./vytoc build tests/fixtures/win_console_app.vt --target "$TRIPLE" --cc "$CC_CMD" --release \
        --manifest tests/fixtures/win_gui_manifest.vt --installer --per-user \
        -o "$STAGE/inst/user/app.exe" >/dev/null 2>>"$STAGE/.inst.err" &&
   mkdir -p "$STAGE/inst/suite" &&
   ./vytoc build apps/pos_suite/pos_win.vt --target "$TRIPLE" --cc "$CC_CMD" --release \
        --manifest apps/pos_suite/pos_manifest.vt --installer \
        -o "$STAGE/inst/suite/pos.exe" >/dev/null 2>>"$STAGE/.inst.err" &&
   ./vytoc build tests/win/svc_state.vt --target "$TRIPLE" --cc "$CC_CMD" \
        -o "$STAGE/svc_state.exe" >/dev/null 2>>"$STAGE/.inst.err" &&
   ./vytoc build tests/win/channel_probe.vt --target "$TRIPLE" --cc "$CC_CMD" \
        -o "$STAGE/channel_probe.exe" >/dev/null 2>>"$STAGE/.inst.err"; then
    echo "BUILT installers"
    rm -f "$STAGE/.inst.err" "$STAGE/inst/machine/service_demo.exe" "$STAGE/inst/user/app.exe" \
          "$STAGE/inst/suite/pos.exe"
else
    echo "FAIL build installers"
    cat "$STAGE/.inst.err"
    fail=1
fi
mkdir -p "$STAGE/tls"
cp tests/fixtures/tls/root.crt tests/fixtures/tls/other-root.crt "$STAGE/tls/"
if ! ./vytoc audit "$STAGE"/*.exe "$STAGE"/inst/*/*-setup.exe "$STAGE"/channel/*.exe; then fail=1; fi
# The DLLs shipped beside the exes are skipped by the exe audit (they are the
# app's own), so audit them as binaries of their own. No exceptions: curl used
# to be a UCRT-linked DLL here and was reported KNOWN; it is now built from
# source into each exe (lib/vyto/net/native/refresh-curl.sh).
for dll in "$STAGE"/*.dll; do
    [ -f "$dll" ] || continue
    if ! ./vytoc audit "$dll"; then fail=1; fi
done

# --- the Windows-side runner ---------------------------------------------
cat > "$STAGE/run.ps1" <<'PS1'
# Runs every staged .exe and checks it against its golden. Generated by
# tests/run_tests_win.sh — edit that, not this.
#
# Usage (from this directory):
#   powershell -ExecutionPolicy Bypass -File run.ps1
# then copy results.txt back to the machine that built this.
#
# Written for PowerShell 2.0, which is what a stock Windows 7 ships: no
# $PSScriptRoot outside modules, no `Get-Content -Raw`, no `Set-Content
# -NoNewline`, no assigning from an if-expression. Keep it that way — a newer
# host runs this fine, an older one is the whole point.

$root = Split-Path -Parent $MyInvocation.MyCommand.Definition
Set-Location -LiteralPath $root
$results = @()
$fail = 0

function Emit($line) {
    Write-Host $line
    $script:results += $line
}

# Get-Content -Raw is PS 3.0+; read the whole file the framework way instead.
function ReadAll($path) {
    if (-not (Test-Path -LiteralPath $path)) { return "" }
    return [System.IO.File]::ReadAllText($path)
}

# Normalize before comparing: print() goes through fwrite to a text-mode stdout,
# so every \n arrives as \r\n. Drop the CRs and any trailing blank lines; the
# rest must match exactly.
function Normalize([string]$s) {
    if ($null -eq $s) { return "" }
    return ($s -replace "`r", "").TrimEnd("`n")
}

# Run a program to completion, its stdout and stderr copied byte for byte into
# two files. Not Start-Process -Wait: that looks the process up again after
# starting it (to put it in a job object) and throws "Cannot process request
# because the process N has exited" when a fast program such as 01_hello has
# already finished — a race, so it failed a varying handful of tests per run.
# Process.Start keeps the handle CreateProcess returned, so an exited program
# is still waited on. The pumps are background threads, given 5 s after the
# exit: a grandchild that inherited a pipe cannot hang the run. C# 2 syntax,
# since PowerShell 2.0 compiles this with the .NET 2.0 compiler.
Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.IO;
using System.Threading;
public static class VytoRun {
    static Thread Pump(Stream from, string path) {
        Thread t = new Thread(delegate() {
            using (FileStream to = new FileStream(path, FileMode.Create, FileAccess.Write,
                                                  FileShare.ReadWrite | FileShare.Delete)) {
                byte[] buf = new byte[65536];
                int n;
                while ((n = from.Read(buf, 0, buf.Length)) > 0) { to.Write(buf, 0, n); to.Flush(); }
            }
        });
        t.IsBackground = true;
        t.Start();
        return t;
    }
    public static int Run(string exe, string args, string dir, string outFile, string errFile) {
        ProcessStartInfo si = new ProcessStartInfo(exe, args);
        si.UseShellExecute = false;
        si.WorkingDirectory = dir;
        si.RedirectStandardOutput = true;
        si.RedirectStandardError = true;
        using (Process p = Process.Start(si)) {
            Thread o = Pump(p.StandardOutput.BaseStream, outFile);
            Thread e = Pump(p.StandardError.BaseStream, errFile);
            p.WaitForExit();
            o.Join(5000);
            e.Join(5000);
            return p.ExitCode;
        }
    }
}
'@

# The unicode_paths fixture's argument: Z, o-umlaut, e-diaeresis and the CJK
# "sun" (U+65E5), outside every single ANSI code page at once. Kept as [char]
# codes so this script stays ASCII.
$uni = "Z" + [char]0x00F6 + [char]0x00EB + [char]0x65E5

# The listdir fixture reads a seeded directory relative to the working dir.
$pick = Join-Path $root "tests\tmp\pickdir"
New-Item -ItemType Directory -Force -Path (Join-Path $pick "sub") | Out-Null
[System.IO.File]::WriteAllText((Join-Path $pick "a.txt"), "aaa")
[System.IO.File]::WriteAllText((Join-Path $pick "b.txt"), "bbb")
[System.IO.File]::WriteAllText((Join-Path $pick "sub\inner.txt"), "inner")

foreach ($line in Get-Content -LiteralPath "manifest.txt") {
    if (-not $line.Trim()) { continue }
    $parts = $line.Split("|")
    $name = $parts[0]; $mode = $parts[1]; $argstr = $parts[2]; $envstr = $parts[3]
    $argstr = $argstr.Replace("{uni}", $uni)
    $envstr = $envstr.Replace("{uni}", $uni)

    # Per-program environment (e.g. VS_HEADLESS=1 VS_EVENTS=x.events). Set on
    # this process so the child inherits it (VytoRun passes no environment of
    # its own), then removed again.
    $setKeys = @()
    if ($envstr) {
        foreach ($kv in $envstr.Split(" ")) {
            if (-not $kv) { continue }
            $eq = $kv.IndexOf("=")
            if ($eq -lt 1) { continue }
            $k = $kv.Substring(0, $eq)
            [Environment]::SetEnvironmentVariable($k, $kv.Substring($eq + 1), "Process")
            $setKeys += $k
        }
    }

    $exe = Join-Path $root "$name.exe"
    $outFile = [System.IO.Path]::GetTempFileName()
    $errFile = [System.IO.Path]::GetTempFileName()
    # The argument string goes through as is: Start-Process joined its
    # ArgumentList with spaces and no quoting, so this is the same command line.
    $started = $true
    try {
        [VytoRun]::Run($exe, $argstr, $root, $outFile, $errFile) | Out-Null
    } catch {
        Emit "FAIL $name (could not start: $_)"
        $fail = 1
        $started = $false
    }
    # Unset before anything can `continue`, so one program's environment never
    # leaks into the next.
    foreach ($k in $setKeys) { [Environment]::SetEnvironmentVariable($k, $null, "Process") }
    if (-not $started) { continue }

    $stdout = ReadAll $outFile
    $stderr = ReadAll $errFile
    Remove-Item -LiteralPath $outFile -Force
    Remove-Item -LiteralPath $errFile -Force

    # 'out' compares stdout alone: those programs shell out to cmd.exe, whose
    # own diagnostics land on stderr and are not part of our golden.
    if ($mode -eq "out") { $got = Normalize $stdout }
    else { $got = Normalize ($stdout + $stderr) }
    $want = Normalize (ReadAll (Join-Path $root "$name.expected"))

    if ($mode -eq "contains") { $ok = $got.Contains($want) } else { $ok = ($got -eq $want) }
    if ($ok) {
        Emit "PASS $name"
    } else {
        Emit "FAIL $name"
        Emit "--- expected ---"
        Emit $want
        Emit "--- got ---"
        Emit $got
        $fail = 1
    }
}

# The system DLL exports this Windows has, for `vytoc audit`. Copy
# win7_exports.txt back as lib/vyto/win/win7_exports.txt when this is Windows 7.
$dump = Join-Path $root "dump_exports.exe"
if (Test-Path -LiteralPath $dump) {
    $exportsFile = Join-Path $root "win7_exports.txt"
    $dumpErr = [System.IO.Path]::GetTempFileName()
    [VytoRun]::Run($dump, "", $root, $exportsFile, $dumpErr) | Out-Null
    Remove-Item -LiteralPath $dumpErr -Force
    Emit "WROTE win7_exports.txt"
}

if ($fail -eq 0) { Emit "ALL PASS" } else { Emit "FAILURES PRESENT" }
[System.IO.File]::WriteAllLines((Join-Path $root "results.txt"), [string[]]$results)
exit $fail
PS1

cat > "$STAGE/README.txt" <<'TXT'
Vyto windows-x64 test bundle
============================

Built on Linux with x86_64-w64-mingw32-gcc by tests/run_tests_win.sh.
Everything needed is in this directory; no runtime install is required.

To run, from this directory on a Windows machine:

    powershell -ExecutionPolicy Bypass -File run.ps1

run.ps1 is written for PowerShell 2.0 (what a stock Windows 7 ships), so it
needs no WMF update. Newer hosts run it unchanged.

Each program is run and its output compared against the matching .expected
file. Results go to the console and to results.txt — copy results.txt back to
the build machine.

run.ps1 also writes win7_exports.txt: the functions this Windows' system DLLs
export. From a Windows 7 machine, copy it back as lib/vyto/win/win7_exports.txt;
`vytoc audit` (and `vytoc build --audit`) checks every exe's imports against it.

Note on 45_os: it uses only shell builtins that sh and cmd.exe share (exit N,
echo), so its output does not depend on which tools are on PATH.

Note on bigfile_win: it writes one byte just past 2 GB. NTFS is not sparse
by default, so that really writes ~2 GB of zeros next to the exe (removed
afterwards): it needs the free space and can take a minute on a slow disk.

Note on worker_win: about 5 seconds. It starts workers that die at once, and
a worker whose parent exits in the middle of its job.

By hand, after run.ps1 (these need a person or a network):

  HTTPS (vendored curl + mbedTLS). From cmd in this directory:
      set VYTO_HTTP_LIVE=1 & 47_http.exe          -> live.status=200 ok=true
      set HTTPS_URL=https://letsencrypt.org/& https_local.exe
                                                   -> status=200 ok=true
      set HTTPS_URL=https://self-signed.badssl.com/& https_local.exe
                                                   -> status=0 ok=false (MUST fail)
      set HTTPS_URL=https://expired.badssl.com/& https_local.exe
                                                   -> status=0 ok=false (MUST fail)
    Then rename cacert.pem away and run the letsencrypt line again: that is
    the Windows ROOT store alone. Note the result either way — on a box that
    rarely browses it may fail, which is why cacert.pem ships with the exe.

  vyto/desktop:  desktop_demo.exe window
    A window opens, then one dialog after another. Check each:
      - message box: information icon, title and text show "Zöë 日本"
      - Yes/No box: question icon; press Yes
      - open: the Explorer-style dialog with a "Text" filter; pick a file
        whose name has non-ASCII letters; its full path is printed
      - open many: Ctrl+click two files; both paths are printed
      - save: suggests vyto-new.txt; choosing an existing file asks first.
        The dialog only picks the path; the demo then writes a small file
        there and prints "wrote: true" — check the file exists
      - folder: the modern folder picker; the folder path is printed
      - every dialog above stays in front of the window, and the window
        cannot be clicked while one is open (they are owned by it)
      - a warning box with no owner, then example.com opens in the browser
    desktop_dirs_win.exe prints its checks (also in run.ps1).

  Installers (inst\): run each setup.exe by double-click.
    - inst\machine\Vyto-Demo-Service-1.2.3-setup.exe asks for administrator
      rights (UAC), installs to C:\Program Files\Vyto Tests\..., and
      services.msc then lists "Vyto Demo Service", running. Run it again:
      it says the installed version will be updated.
    - inst\user\Vyto-Test-App-1.2.3-setup.exe asks for nothing and installs
      to %LOCALAPPDATA%\Programs.
    - Both show in Programs and Features (Add/Remove Programs) with name,
      version and publisher; their Start menu shortcut works; uninstalling
      from there removes the files, shortcuts, the entry and the service.
    - inst\suite\Vyto-Café-POS-Suite-0.2.0-setup.exe: the till and two
      services in one folder (C:\Program Files\Vyto\Vyto Café POS Suite).
      services.msc lists "Vyto Café Payments" and "Vyto Café Stock",
      running; Payments' Dependencies tab shows Stock. Open the till: both
      header pills green, tiles show "30 left". Sell, pay with
      "M-Pesa (simulated)": the receipt shows the M-Pesa ref, the counts drop.
      Stop "Vyto Café Payments" in services.msc: its pill goes grey and the
      button says "Payments offline", cash still works; start it again: the
      pill turns green on its own. Task Manager: pos.exe near 0% CPU idle.
    - With the suite installed, from cmd:
          channel_probe.exe vyto-cafe-suite stock steal     -> steal: refused
      (another program cannot take a service's port: SO_EXCLUSIVEADDRUSE)

Not covered here: vyto/hw/* (Linux device interfaces) and vyto/intl (ICU) are
not portable to Windows yet. Everything else in the stdlib is staged above.
TXT

if command -v zip >/dev/null 2>&1; then
    (cd tests/tmp && rm -f vyto-win-x64.zip && zip -qr vyto-win-x64.zip win-x64) &&
        echo "packaged tests/tmp/vyto-win-x64.zip"
fi

echo "staged $(ls "$STAGE"/*.exe 2>/dev/null | wc -l) executables in $STAGE"
if [ "$fail" -ne 0 ]; then
    echo "cross-build had failures"
else
    echo "cross-build clean — copy $STAGE to a Windows machine and run run.ps1"
fi
exit $fail
