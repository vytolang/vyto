#!/bin/sh
# Cross-builds the Windows-portable slice of the suite for windows-x64 and stages
# it, with its goldens and a self-checking runner, into tests/tmp/win-x64/.
#
# This script does NOT run anything Windows — the .exe files are meant to be
# copied to a real Windows machine and checked there with run.ps1 (which this
# script generates). Copy results.txt back to see what passed.
#
# Scope: core language + runtime + the stdlib packages with no POSIX
# dependencies. vyto/surface, vyto/ui, vyto/gfx, vyto/net/*, vyto/os/worker,
# vyto/hw/* and vyto/intl are NOT portable yet and are deliberately excluded —
# see docs/getting-started.md.
set -u
cd "$(dirname "$0")/.."

CROSS_CC=x86_64-w64-mingw32-gcc
TRIPLE=windows-x64
STAGE=tests/tmp/win-x64
# -static-libgcc so the staged exes need no libgcc_s_*.dll on the target box:
# the whole staging dir has to be self-contained once it leaves this machine.
CC_CMD="$CROSS_CC -static-libgcc"

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
tests/fixtures/args_echo.vt|args_echo|eq|alpha beta
tests/fixtures/anim_clock.vt|anim_clock|eq|
tests/fixtures/condlink_win.vt|condlink_win|eq|
tests/fixtures/listdir.vt|listdir|eq|
tests/fixtures/overflow_trap.vt|overflow_trap|contains|
apps/motiondemo/motiondemo.vt|motiondemo|contains||VS_HEADLESS=1 VS_EVENTS=motiondemo.events|--with-assets
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

    # Per-program environment (e.g. VS_HEADLESS=1 VS_EVENTS=x.events). Set on
    # this process so the child inherits it, then removed again — Start-Process
    # has no environment parameter in PowerShell 2.0.
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
    $spArgs = @{ FilePath = $exe; NoNewWindow = $true; Wait = $true
                 RedirectStandardOutput = $outFile; RedirectStandardError = $errFile }
    if ($argstr) { $spArgs.ArgumentList = $argstr.Split(" ") }
    $started = $true
    try {
        Start-Process @spArgs | Out-Null
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

Note on 45_os: its golden (examples/45_os.expected.windows-x64) was written
from a prediction of cmd.exe's behaviour for run()/capture(), NOT from an
observed run. If it fails, the observed output is very likely the correct
golden — reconcile it on the build machine rather than assuming a code bug.

Not covered here: vyto/surface, vyto/ui, vyto/gfx, vyto/net/*, vyto/os/worker,
vyto/hw/* and vyto/intl are not portable to Windows yet.
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
