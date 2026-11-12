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
# box stays the check of record; `vytoc audit` (run by run_tests_win.sh) stays the import check.
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

# --- vyto/desktop: answer each of desktop_demo's dialogs ----------------------
# Message box, Yes/No, open, multi-select open, save and the folder picker, in
# that order, each found by its title. Wine's comdlg32 and IFileOpenDialog
# are Wine's, not Windows 7's, so this proves our calls and the result
# parsing (the double-NUL multi-select list, the shell item path), not the
# look of the dialogs.
desktop_check() {
    for t in Xvfb xdotool; do
        command -v "$t" >/dev/null 2>&1 || { echo "SKIP wine_desktop (no $t)"; return 0; }
    done
    [ -f "$STAGE/desktop_demo.exe" ] || { echo "FAIL wine_desktop (not staged)"; fail=$((fail + 1)); return 0; }
    # A wineserver left over from the display-less manifest runs makes the
    # dialogs return at once without ever mapping a window.
    wineserver -k >/dev/null 2>&1; sleep 1
    dd="$STAGE/desk"; rm -rf "$dd"; mkdir -p "$dd/dfold"
    printf a > "$dd/desk_a.txt"; printf b > "$dd/desk_b.txt"
    Xvfb -displayfd 3 -screen 0 1280x800x24 -nolisten tcp 3>"$tmp/ddpy" >/dev/null 2>&1 &
    xp=$!
    i=0; while [ ! -s "$tmp/ddpy" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    DISPLAY=":$(cat "$tmp/ddpy")"; export DISPLAY
    (cd "$dd" && "$WINE" ../desktop_demo.exe > "$tmp/desk.log" 2>&1) &
    dp=$!
    wp="Z:$(cd "$dd" && pwd | tr / '\\')"
    step() {
        w=$(timeout 20 xdotool search --sync --name "^$1\$" 2>/dev/null | head -1)
        [ -n "$w" ] || return 1
        sleep 0.8; xdotool windowfocus "$w" 2>/dev/null; sleep 0.3
    }
    step vyto-desk-msg && xdotool key Return; sleep 0.5
    step vyto-desk-ask && xdotool key Return; sleep 0.5
    step vyto-desk-open && { xdotool type --delay 30 "$wp\\desk_a.txt"; xdotool key Return; }; sleep 0.8
    step vyto-desk-many && { xdotool type --delay 30 '"desk_a.txt" "desk_b.txt"'; xdotool key Return; }; sleep 0.8
    step vyto-desk-save && { xdotool key ctrl+a; xdotool type --delay 30 "$wp\\saved.txt"; xdotool key Return; }; sleep 0.8
    step vyto-desk-folder && { xdotool type --delay 30 "$wp\\dfold"; xdotool key Return; }; sleep 1
    i=0; while kill -0 "$dp" 2>/dev/null && [ $i -lt 75 ]; do sleep 0.2; i=$((i + 1)); done
    kill "$dp" 2>/dev/null
    kill "$xp" 2>/dev/null
    cat > "$tmp/desk.want" <<WANT
message: true
confirm: true
open: $wp\desk_a.txt
many: 2
  $wp\desk_a.txt
  $wp\desk_b.txt
save: $wp\saved.txt
wrote: true
folder: $wp\dfold
done
WANT
    tr -d '\r' < "$tmp/desk.log" | grep -v "^X connection" > "$tmp/desk.got"
    if cmp -s "$tmp/desk.want" "$tmp/desk.got"; then
        echo "PASS wine_desktop_dialogs"; pass=$((pass + 1))
    else
        echo "FAIL wine_desktop_dialogs"; diff "$tmp/desk.want" "$tmp/desk.got" | head -20
        fail=$((fail + 1))
    fi
    unset DISPLAY
}
desktop_check

# --- HTTPS: the vendored curl + mbedTLS against a local server ----------------
# No network: openssl s_server on loopback with the test PKI in
# tests/fixtures/tls. Trusting the fixture root must succeed — from a path
# with non-ASCII in it, since mbedTLS's own fopen could not open one; the
# default trust store and an unrelated root must both FAIL, which is the proof
# that verification is on rather than merely that a handshake happened.
https_check() {
    command -v openssl >/dev/null 2>&1 || { echo "SKIP wine_https (no openssl)"; return 0; }
    [ -f "$STAGE/https_local.exe" ] || { echo "FAIL wine_https (not staged)"; fail=$((fail + 1)); return 0; }
    port=$((20000 + $$ % 20000))
    (cd tests/fixtures/tls && exec openssl s_server -quiet -accept "127.0.0.1:$port" \
        -cert leaf.crt -key leaf.key -www) >/dev/null 2>&1 &
    sp=$!
    sleep 1
    hd="$STAGE/tls/$UNI"; mkdir -p "$hd"; cp "$STAGE/tls/root.crt" "$hd/root.pem"
    url="https://127.0.0.1:$port/"
    r_ok=$(cd "$STAGE" && HTTPS_URL=$url HTTPS_CA="tls\\$UNI\\root.pem" "$WINE" ./https_local.exe 2>&1 | tr -d '\r')
    r_def=$(cd "$STAGE" && HTTPS_URL=$url "$WINE" ./https_local.exe 2>&1 | tr -d '\r')
    r_bad=$(cd "$STAGE" && HTTPS_URL=$url HTTPS_CA='tls\other-root.crt' "$WINE" ./https_local.exe 2>&1 | tr -d '\r')
    kill "$sp" 2>/dev/null
    if [ "$r_ok" = "status=200 ok=true" ] && [ "$r_def" = "status=0 ok=false" ] &&
       [ "$r_bad" = "status=0 ok=false" ]; then
        echo "PASS wine_https_verify"; pass=$((pass + 1))
    else
        echo "FAIL wine_https_verify (fixture CA: $r_ok | default store: $r_def | other root: $r_bad)"
        fail=$((fail + 1))
    fi
}
https_check

# --- vyto/desktop/service: a Windows service, end to end ----------------------
# service_demo.exe installs itself with the Service Control Manager, is started
# by it (no argument = the service body, logging to %ProgramData%), stopped,
# and uninstalled. Wine shuts its session down — sending services SHUTDOWN —
# when the last ordinary process exits, so a console copy of the demo keeps the
# session alive meanwhile (it is killed by PID afterwards).
service_check() {
    [ -f "$STAGE/service_demo.exe" ] || { echo "FAIL wine_service (not staged)"; fail=$((fail + 1)); return 0; }
    wineserver -k >/dev/null 2>&1; sleep 1
    log="$WINEPREFIX/drive_c/ProgramData/VytoDemoSvc/VytoDemoSvc.log"
    rm -f "$log"
    (cd "$STAGE" && timeout 90 "$WINE" ./service_demo.exe uninstall >/dev/null 2>&1)
    (cd "$STAGE" && exec timeout 90 "$WINE" ./service_demo.exe > "$tmp/keep.log" 2>&1 < /dev/null) &
    kp=$!
    sleep 3
    sv() { (cd "$STAGE" && timeout 40 "$WINE" ./service_demo.exe "$1" 2>&1 | tr -d '\r'); }
    got="$(sv install); $(sv start); $(sv state)"
    sleep 2
    got="$got; $(sv state); $(sv stop); $(sv state); $(sv uninstall); $(sv state)"
    kill "$kp" 2>/dev/null
    want="install: ok; start: ok; state: 4; state: 4; stop: ok; state: 1; uninstall: ok; state: -1"
    logtxt=$(tr -d '\r' < "$log" 2>/dev/null | sed 's/^[0-9 :-]*  //' | sed 's/after [0-9]* heartbeat/after N heartbeat/')
    if [ "$got" = "$want" ] && [ "$logtxt" = "started (service: true)
stopped after N heartbeat(s)" ]; then
        echo "PASS wine_service_lifecycle"; pass=$((pass + 1))
    else
        echo "FAIL wine_service_lifecycle"
        echo "  got:  $got"
        echo "  want: $want"
        echo "  log:  $logtxt"
        fail=$((fail + 1))
    fi
    wineserver -k >/dev/null 2>&1
}
service_check

# --- vytoc --installer: silent install, upgrade and uninstall ------------------
# Every artifact is checked, not just exit codes: the files, the Add/Remove
# Programs entry, the Start menu shortcut, the service (per-machine), and that
# all of it is gone afterwards. /NODESKTOP keeps the shortcut off the desktop:
# Wine maps a user's Desktop to the real ~/Desktop of whoever runs this.
installer_check() {
    ms=$(ls "$STAGE"/inst/machine/*-setup.exe 2>/dev/null | head -1)
    us=$(ls "$STAGE"/inst/user/*-setup.exe 2>/dev/null | head -1)
    if [ -z "$ms" ] || [ -z "$us" ]; then
        echo "FAIL wine_installer (not staged)"; fail=$((fail + 1)); return 0
    fi
    wineserver -k >/dev/null 2>&1; sleep 1
    w() { timeout 90 "$WINE" "$@" 2>&1 | tr -d '\r'; }
    regq() { timeout 30 "$WINE" reg query "$1" >/dev/null 2>&1 && echo yes || echo no; }
    st() { (cd "$STAGE" && w ./service_demo.exe state); }
    pf="$WINEPREFIX/drive_c/Program Files/Vyto Tests"
    key='HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\Vyto Demo Service'
    lnk="$WINEPREFIX/drive_c/ProgramData/Microsoft/Windows/Start Menu/Programs/Vyto Demo Service.lnk"
    (cd "$STAGE" && exec timeout 120 "$WINE" ./service_demo.exe > "$tmp/keep2.log" 2>&1 < /dev/null) &
    kp=$!
    sleep 3
    out=""
    w "$ms" /S /NODESKTOP >/dev/null; out="install=$?"
    out="$out files=$([ -f "$pf/Vyto Demo Service/service_demo.exe" ] && [ -f "$pf/Vyto Demo Service/uninstall.exe" ] && echo yes || echo no)"
    out="$out reg=$(regq "$key") lnk=$([ -f "$lnk" ] && echo yes || echo no) $(st)"
    w "$ms" /S /NODESKTOP >/dev/null; out="$out | upgrade $(st)"
    w "C:\\Program Files\\Vyto Tests\\Vyto Demo Service\\uninstall.exe" /S >/dev/null
    i=0; while [ -d "$pf" ] && [ $i -lt 40 ]; do sleep 0.5; i=$((i + 1)); done
    out="$out | gone=$([ -d "$pf" ] && echo no || echo yes) reg=$(regq "$key") lnk=$([ -f "$lnk" ] && echo yes || echo no) $(st)"
    kill "$kp" 2>/dev/null
    want="install=0 files=yes reg=yes lnk=yes state: 4 | upgrade state: 4 | gone=yes reg=no lnk=no state: -1"
    if [ "$out" = "$want" ]; then
        echo "PASS wine_installer_machine"; pass=$((pass + 1))
    else
        echo "FAIL wine_installer_machine"; echo "  got:  $out"; echo "  want: $want"; fail=$((fail + 1))
    fi

    ukey='HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\Vyto Test App'
    w "$us" /S /NODESKTOP >/dev/null
    ud=$(find "$WINEPREFIX/drive_c/users" -type d -path "*Programs/Vyto Test App" 2>/dev/null | head -1)
    out="dir=$([ -n "$ud" ] && [ -f "$ud/app.exe" ] && echo yes || echo no) reg=$(regq "$ukey")"
    if [ -n "$ud" ]; then
        win_ud=$(timeout 30 "$WINE" winepath -w "$ud" 2>/dev/null | tr -d '\r')
        w "$win_ud\\uninstall.exe" /S >/dev/null
        i=0; while [ -d "$ud" ] && [ $i -lt 40 ]; do sleep 0.5; i=$((i + 1)); done
    fi
    out="$out | gone=$([ -n "$ud" ] && [ ! -d "$ud" ] && echo yes || echo no) reg=$(regq "$ukey")"
    if [ "$out" = "dir=yes reg=yes | gone=yes reg=no" ]; then
        echo "PASS wine_installer_user"; pass=$((pass + 1))
    else
        echo "FAIL wine_installer_user ($out)"; fail=$((fail + 1))
    fi
    wineserver -k >/dev/null 2>&1
}
installer_check

# --- vyto/desktop/channel on Windows: the GUI loop sleeps on the socket --------
# gui_probe.exe idle connects to the channel test server and waits. Its
# voluntary context switches over 2 s are counted: the Win32 loop now waits on
# the socket itself (WSAEventSelect + MsgWaitForMultipleObjectsEx), measured at
# ~64 under Wine, where the old 16 ms polling was ~480 and a mis-armed event
# spun at ~170000. Then cli.exe pushes and the window must close.
channel_check() {
    for t in Xvfb; do command -v "$t" >/dev/null 2>&1 || { echo "SKIP wine_channel_gui (no $t)"; return 0; }; done
    cdir="$STAGE/channel"
    [ -f "$cdir/gui_probe.exe" ] || { echo "FAIL wine_channel_gui (not staged)"; fail=$((fail + 1)); return 0; }
    wineserver -k >/dev/null 2>&1; sleep 1
    rm -rf "$cdir/dir"; mkdir -p "$cdir/dir"
    VYTO_CHANNEL_DIR="Z:$(cd "$cdir" && pwd | tr / '\\')\\dir"; export VYTO_CHANNEL_DIR
    # The display comes first: the first Wine process of a session decides
    # whether that session has X, so a server started without DISPLAY leaves
    # the GUI started after it unable to open a window.
    Xvfb -displayfd 3 -screen 0 800x600x24 -nolisten tcp 3>"$tmp/cdpy" >/dev/null 2>&1 & xp=$!
    i=0; while [ ! -s "$tmp/cdpy" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    cdisp=":$(cat "$tmp/cdpy")"
    (cd "$cdir" && DISPLAY=$cdisp exec timeout 60 "$WINE" ./srv.exe > /dev/null 2>&1 < /dev/null) & sp=$!
    sleep 3
    (cd "$cdir" && DISPLAY=$cdisp timeout 40 "$WINE" ./gui_probe.exe idle > "$tmp/gp.log" 2>&1) & gp=$!
    sleep 4
    pid=$(pgrep -x gui_probe.exe | head -1)
    wk=-1
    if [ -n "$pid" ]; then
        a=$(awk '/^voluntary_ctxt/{print $2}' /proc/$pid/status)
        sleep 2
        b=$(awk '/^voluntary_ctxt/{print $2}' /proc/$pid/status)
        wk=$((b - a))
    fi
    (cd "$cdir" && DISPLAY=$cdisp timeout 20 "$WINE" ./cli.exe pushone > /dev/null 2>&1)
    wait $gp
    kill $sp $xp 2>/dev/null
    got=$(tr -d '\r' < "$tmp/gp.log")
    unset VYTO_CHANNEL_DIR
    if [ "$got" = "state: true
push: hello gui
window closed" ] && [ "$wk" -ge 0 ] && [ "$wk" -lt 200 ]; then
        echo "PASS wine_channel_gui ($wk wakeups in 2 s idle)"; pass=$((pass + 1))
    else
        echo "FAIL wine_channel_gui (wakeups $wk; got: $got)"; fail=$((fail + 1))
    fi
    wineserver -k >/dev/null 2>&1
}
channel_check

# --- an app suite: the till + two services from one installer -----------------
# All exes in one folder; both services running; Payments' dependency on
# Stock recorded where Windows keeps it (DependOnService); the installed
# payments service answers channel_probe with a payment record; an upgrade
# leaves both running; uninstall removes both and the folder.
suite_check() {
    ss=$(ls "$STAGE"/inst/suite/*-setup.exe 2>/dev/null | head -1)
    [ -n "$ss" ] || { echo "FAIL wine_installer_suite (not staged)"; fail=$((fail + 1)); return 0; }
    wineserver -k >/dev/null 2>&1; sleep 1
    (cd "$STAGE" && exec timeout 200 "$WINE" ./service_demo.exe > /dev/null 2>&1 < /dev/null) & kp=$!
    sleep 3
    w() { timeout 90 "$WINE" "$@" 2>&1 | tr -d '\r'; }
    pf="$WINEPREFIX/drive_c/Program Files/Vyto/Vyto Café POS Suite"
    st() { (cd "$STAGE" && w ./svc_state.exe VytoCafeStock VytoCafePayments | tr '\n' ' '); }
    w "$ss" /S /NODESKTOP >/dev/null
    files=$(cd "$pf" 2>/dev/null && ls | tr '\n' ' ')
    deps=$(w reg query 'HKLM\SYSTEM\CurrentControlSet\Services\VytoCafePayments' /v DependOnService | grep -c VytoCafeStock)
    pay=$(cd "$STAGE" && w ./channel_probe.exe vyto-cafe-suite payments pay.simulate '{"amount_cents":45000,"phone":"254700000123"}')
    s1=$(st)
    w "$ss" /S /NODESKTOP >/dev/null
    s2=$(st)
    w "C:\\Program Files\\Vyto\\Vyto Café POS Suite\\uninstall.exe" /S >/dev/null
    i=0; while [ -d "$pf" ] && [ $i -lt 40 ]; do sleep 0.5; i=$((i + 1)); done
    s3=$(st)
    gone=yes; [ -d "$pf" ] && gone=no
    kill $kp 2>/dev/null
    wineserver -k >/dev/null 2>&1
    r=ok
    [ "$files" = "payments.exe pos.exe stock.exe uninstall.exe uninstall.ini " ] || r="files: $files"
    [ "$deps" = 1 ] || r="no DependOnService"
    echo "$pay" | grep -q '"amount_cents":45000' && echo "$pay" | grep -q '"method":"M-Pesa"' || r="payment: $pay"
    [ "$s1" = "VytoCafeStock: 4 VytoCafePayments: 4 " ] || r="after install: $s1"
    [ "$s2" = "VytoCafeStock: 4 VytoCafePayments: 4 " ] || r="after upgrade: $s2"
    [ "$s3" = "VytoCafeStock: -1 VytoCafePayments: -1 " ] && [ $gone = yes ] || r="after uninstall: $s3 gone=$gone"
    if [ "$r" = ok ]; then echo "PASS wine_installer_suite"; pass=$((pass + 1)); else
        echo "FAIL wine_installer_suite ($r)"; fail=$((fail + 1)); fi
}
suite_check

echo "wine: $pass passed, $fail failed (Wine $("$WINE" --version 2>/dev/null), not Windows 7)"
[ "$fail" -eq 0 ]
