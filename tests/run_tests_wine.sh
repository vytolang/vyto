#!/bin/sh
# Runs the staged Windows suite (tests/tmp/win-x64, built by `make test-win`)
# under Wine on this machine: the same manifest, the same goldens and the same
# comparison rules as the run.ps1 that ships to a real Windows box.
#
# What it is for: a fast first check before anything goes to Windows. It
# catches a crash, a missing import, a wrong golden or a Unicode regression in
# seconds instead of a copy-run-copy-back round.
#
# What it is NOT: Windows 7. Wine reimplements msvcrt, kernel32, GDI and the
# rest, so a pass here does not prove a pass there (nor the reverse). The Win7
# box stays the check of record; tests/win/pe_audit.sh stays the import check.
#
# The prefix lives in $VYTO_WINEPREFIX (default: ~/.cache/vyto/wineprefix),
# outside tests/tmp so a restage does not throw away the ~50 s it takes Wine
# to create one. Mono and Gecko are declined: nothing here needs them.
set -u
cd "$(dirname "$0")/.."

STAGE=tests/tmp/win-x64
WINE=${WINE:-wine64}

if ! command -v "$WINE" >/dev/null 2>&1; then
    echo "SKIP wine suite (no $WINE: sudo apt-get install wine64)"
    exit 0
fi
if [ ! -f "$STAGE/manifest.txt" ]; then
    echo "FATAL: nothing staged in $STAGE; run 'make test-win' first"
    exit 1
fi

export WINEPREFIX="${VYTO_WINEPREFIX:-${XDG_CACHE_HOME:-$HOME/.cache}/vyto/wineprefix}"
export WINEDEBUG=-all
export WINEDLLOVERRIDES="mscoree,mshtml="
if [ ! -d "$WINEPREFIX" ]; then
    echo "creating Wine prefix $WINEPREFIX (once, ~1 min)"
    mkdir -p "$(dirname "$WINEPREFIX")"
    wineboot -i >/dev/null 2>&1
fi

# run.ps1's {uni}: Z, o-umlaut, e-diaeresis, CJK "sun". Wine turns the UTF-8
# argv and environment into UTF-16 for the program, as Windows would hand it.
UNI="Zöë日"

# The listdir fixture reads a seeded directory relative to the working dir.
mkdir -p "$STAGE/tests/tmp/pickdir/sub"
printf aaa > "$STAGE/tests/tmp/pickdir/a.txt"
printf bbb > "$STAGE/tests/tmp/pickdir/b.txt"
printf inner > "$STAGE/tests/tmp/pickdir/sub/inner.txt"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"; wineserver -k >/dev/null 2>&1' EXIT

# Same normalisation as run.ps1: drop CRs and trailing blank lines.
norm() { tr -d '\r' < "$1" | sed -e :a -e '/^\n*$/{$d;N;ba' -e '}'; }

pass=0
fail=0
while IFS='|' read -r name mode argstr envstr; do
    [ -n "$name" ] || continue
    argstr=$(printf '%s' "$argstr" | sed "s/{uni}/$UNI/g")
    envstr=$(printf '%s' "$envstr" | sed "s/{uni}/$UNI/g")
    exp="$STAGE/$name.expected"
    if [ ! -f "$exp" ]; then
        echo "FAIL wine_$name (no golden)"
        fail=$((fail + 1))
        continue
    fi
    # shellcheck disable=SC2086 # the manifest's args and env are space-split
    (cd "$STAGE" && env $envstr timeout 60 "$WINE" "./$name.exe" $argstr \
        > "$tmp/out" 2> "$tmp/err")
    case "$mode" in
        out) norm "$tmp/out" > "$tmp/got" ;;
        *)   cat "$tmp/out" "$tmp/err" > "$tmp/both"; norm "$tmp/both" > "$tmp/got" ;;
    esac
    norm "$exp" > "$tmp/want"
    ok=0
    if [ "$mode" = contains ]; then
        grep -qF -- "$(cat "$tmp/want")" "$tmp/got" && ok=1
    else
        cmp -s "$tmp/want" "$tmp/got" && ok=1
    fi
    if [ "$ok" = 1 ]; then
        echo "PASS wine_$name"
        pass=$((pass + 1))
    else
        echo "FAIL wine_$name"
        diff "$tmp/want" "$tmp/got" | head -20
        fail=$((fail + 1))
    fi
done < "$STAGE/manifest.txt"

# --- input: drive input_demo.exe with real X events ---------------------------
# The Win32 window code's input handling, end to end: buttons, wheel sign,
# tilt and Shift+wheel, capture on a drag that leaves the window, Unicode text
# (CJK included) through WM_CHAR, the UTF-8 title, and the pointer shape.
# Xvfb gives the window a screen of its own, so nothing lands on the desktop.
# Each X event goes through Wine's x11drv exactly as a real mouse's would, so
# this checks our WndProc, not a simulation of it.
input_check() {
    for t in Xvfb xdotool cc; do
        command -v "$t" >/dev/null 2>&1 || { echo "SKIP wine_input (no $t)"; return 0; }
    done
    if [ ! -f "$STAGE/input_demo.exe" ]; then
        echo "FAIL wine_input (input_demo.exe not staged)"
        fail=$((fail + 1)); return 0
    fi
    sig_tool=""
    if cc -O2 -o "$tmp/xcursor_sig" tests/win/xcursor_sig.c -lXfixes -lX11 2>/dev/null; then
        sig_tool="$tmp/xcursor_sig"
    fi

    Xvfb -displayfd 3 -screen 0 1280x800x24 -nolisten tcp 3>"$tmp/dpy" >/dev/null 2>&1 &
    xvfb_pid=$!
    i=0; while [ ! -s "$tmp/dpy" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    DISPLAY=":$(cat "$tmp/dpy")"; export DISPLAY
    x() { xdotool "$@" 2>/dev/null; }

    (cd "$STAGE" && VYTO_INPUT_GEOM="$tmp/geom" "$WINE" ./input_demo.exe > "$tmp/demo.log" 2>&1) &
    demo_pid=$!
    i=0; while ! grep -q '^ready' "$tmp/geom" 2>/dev/null && [ $i -lt 300 ]; do sleep 0.1; i=$((i + 1)); done
    W=$(timeout 10 xdotool search --sync --name "Vyto Eingabe" 2>/dev/null | head -1)
    if [ -z "$W" ] || ! grep -q '^ready' "$tmp/geom" 2>/dev/null; then
        echo "FAIL wine_input (input_demo did not come up)"
        fail=$((fail + 1)); kill "$xvfb_pid" 2>/dev/null; return 0
    fi
    eval "$(x getwindowgeometry --shell "$W")"   # X, Y: the client origin
    # centre of a widget the demo reported, in screen coordinates
    at() { awk -v n="$1" -v ox="$X" -v oy="$Y" \
        '$1=="geom" && $2==n { printf "%d %d", ox+$3+$5/2, oy+$4+$6/2 }' "$tmp/geom"; }
    ok() { echo "PASS wine_input_$1"; pass=$((pass + 1)); }
    bad() { echo "FAIL wine_input_$1"; [ $# -gt 1 ] && echo "  $2"; fail=$((fail + 1)); }

    title=$(x getwindowname "$W")
    if [ "$title" = "Vyto Eingabe — Zöë 日本 ✓" ]; then ok title; else bad title "got: $title"; fi

    # pointer shapes: arrow over the pad, I-beam over both text inputs, a
    # hand over the link, and back to the arrow
    if [ -n "$sig_tool" ]; then
        x mousemove $(at pad); sleep 0.4; s_pad=$("$sig_tool")
        x mousemove $(at field); sleep 0.4; s_field=$("$sig_tool")
        x mousemove $(at area); sleep 0.4; s_area=$("$sig_tool")
        x mousemove $(at link); sleep 0.4; s_link=$("$sig_tool")
        x mousemove $(at pad); sleep 0.4; s_back=$("$sig_tool")
        if [ "$s_field" != "$s_pad" ] && [ "$s_field" = "$s_area" ] \
           && [ "$s_link" != "$s_pad" ] && [ "$s_link" != "$s_field" ] \
           && [ "$s_back" = "$s_pad" ]; then
            ok cursor
        else
            bad cursor "pad=$s_pad field=$s_field area=$s_area link=$s_link back=$s_pad->$s_back"
        fi
    else
        echo "SKIP wine_input_cursor (no XFixes headers: sudo apt-get install libxfixes-dev)"
    fi

    # buttons and wheel over the pad: 2 middle, 8/9 back/forward, 3 right,
    # 5/4 wheel down/up, 6/7 tilt left/right, then Shift + wheel down
    set -- $(at pad); px=$1; py=$2
    x mousemove "$px" "$py"; sleep 0.2
    for b in 2 8 9 3 5 4 6 7; do x click "$b"; sleep 0.15; done
    x keydown shift; x click 5; x keyup shift; sleep 0.2
    # press in the pad, leave the window, release outside it
    x mousedown 1; sleep 0.1
    x mousemove $((px + 40)) "$py"; sleep 0.1
    x mousemove 1200 780; sleep 0.1
    x mouseup 1; sleep 0.3
    # Unicode through WM_CHAR, then Enter submits the field
    x mousemove $(at field); x click 1; sleep 0.3
    x windowfocus "$W"
    x type --delay 80 "aé üß 日本"; sleep 0.3
    x key Return; sleep 0.5
    x key --window "$W" alt+F4
    # Let the demo exit before the display goes away; Wine reports a lost X
    # connection into the very log being compared otherwise.
    i=0; while kill -0 "$demo_pid" 2>/dev/null && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
    if kill -0 "$demo_pid" 2>/dev/null; then
        bad exit "input_demo still running 10 s after Alt+F4"
        kill "$demo_pid" 2>/dev/null
    fi
    kill "$xvfb_pid" 2>/dev/null

    cat > "$tmp/demo.want" <<'WANT'
middle down
middle up
back down
back up
forward down
forward up
context (right down)
right up
scroll 3
scroll -3
scroll_x -3
scroll_x 3
scroll_x 3
left down
drag end
submit: aé üß 日本
WANT
    tr -d '\r' < "$tmp/demo.log" > "$tmp/demo.got"
    if cmp -s "$tmp/demo.want" "$tmp/demo.got"; then
        ok events
    else
        bad events "expected vs got:"
        diff "$tmp/demo.want" "$tmp/demo.got" | head -20
    fi
}
input_check

echo "wine: $pass passed, $fail failed (Wine $("$WINE" --version 2>/dev/null), not Windows 7)"
[ "$fail" -eq 0 ]
