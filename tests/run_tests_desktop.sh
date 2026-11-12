#!/bin/sh
# vyto/desktop end to end on Linux: the real zenity (or kdialog, when it is
# the only one installed) on a private Xvfb display, every dialog answered with
# xdotool, run twice — without an owner, and owned by a real X window
# (--attach). make test checks the exact argv against a fake; this checks that
# the real program accepts it and that its real output parses.
#
# Not in make test: it needs Xvfb, xdotool and a dialog program, and takes
# ~20 s. SKIPs (exit 0) when any is missing.
#
# The dialog program runs in a scrubbed environment. A terminal inside a snap
# (VS Code's) exports GTK_PATH, LOCPATH, GIO_MODULE_DIR and friends pointing
# into the snap, and any GTK program started from it — zenity included —
# loads the snap's libraries and dies before showing anything. That is the
# terminal's environment, not vyto/desktop; a user's session does not have it.
set -u
cd "$(dirname "$0")/.."

for t in Xvfb xdotool; do
    command -v "$t" >/dev/null 2>&1 || { echo "SKIP desktop (no $t)"; exit 0; }
done
prog=""
for p in zenity kdialog; do
    [ -x "/usr/bin/$p" ] && { prog=/usr/bin/$p; break; }
done
[ -n "$prog" ] || { echo "SKIP desktop (no zenity or kdialog)"; exit 0; }

tmp=$(mktemp -d)
xp=""
trap 'rm -rf "$tmp"; [ -n "$xp" ] && kill "$xp" 2>/dev/null' EXIT
if ! ./vytoc build tests/desktop/real_dialogs.vt -o "$tmp/real_dialogs" >"$tmp/build.log" 2>&1; then
    echo "FAIL desktop (build)"; cat "$tmp/build.log"; exit 1
fi

Xvfb -displayfd 3 -screen 0 1280x800x24 -nolisten tcp 3>"$tmp/dpy" >/dev/null 2>&1 &
xp=$!
i=0; while [ ! -s "$tmp/dpy" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
dn=":$(cat "$tmp/dpy")"
x() { DISPLAY=$dn xdotool "$@" 2>/dev/null; }
step() {
    w=$(DISPLAY=$dn timeout 20 xdotool search --sync --name "^$1\$" 2>/dev/null | head -1)
    [ -n "$w" ] || return 1
    sleep 1; x windowfocus "$w"; sleep 0.3
}

pass=0; fail=0
run() { # mode
    d="$tmp/$1"; mkdir -p "$d/files" "$d/pick"
    printf a > "$d/files/desk_a.txt"; printf b > "$d/files/desk_b.txt"
    # XDG_DATA_HOME: GTK records every picked file in recently-used.xbel;
    # keep the test's throwaway paths out of the real Recent list.
    env -i HOME="$HOME" XDG_DATA_HOME="$tmp/xdgdata" PATH=/usr/bin:/bin DISPLAY="$dn" LANG=C.UTF-8 \
        VYTO_DESKTOP_DIALOG="$prog" "$tmp/real_dialogs" "$d" "$1" > "$tmp/$1.log" 2>&1 &
    dp=$!
    step vyto-desk-msg && x key Return; sleep 0.7
    step vyto-desk-ask && x key Return; sleep 0.7
    # GTK's location bar takes a typed path; a leading '/' opens it
    step vyto-desk-open && { x type --delay 40 "$d/files/desk_a.txt"; sleep 0.5; x key Return; }; sleep 1
    step vyto-desk-many && { sleep 0.5; x key ctrl+a; sleep 0.4; x key Return; }; sleep 1
    step vyto-desk-save && { x key ctrl+a; x type --delay 40 "$d/saved.txt"; sleep 0.5; x key Return; }; sleep 1
    step vyto-desk-folder && { x type --delay 40 "$d/pick/"; sleep 0.5; x key Return; }; sleep 1
    i=0; while kill -0 "$dp" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.25; i=$((i + 1)); done
    kill "$dp" 2>/dev/null
    cat > "$tmp/$1.want" <<WANT
available: true
message: true
confirm: true
open: $d/files/desk_a.txt
many: 2
save: $d/saved.txt
wrote: true
folder: $d/pick
done
WANT
    if cmp -s "$tmp/$1.want" "$tmp/$1.log" &&
       [ "$(cat "$d/saved.txt" 2>/dev/null)" = "saved — Zöë 日本" ]; then
        echo "PASS desktop_real_$(basename "$prog")_${1:-plain}"; pass=$((pass + 1))
    else
        echo "FAIL desktop_real_$(basename "$prog")_${1:-plain}"
        diff "$tmp/$1.want" "$tmp/$1.log" | head -20
        fail=$((fail + 1))
    fi
}
run plain
run window

echo "desktop: $pass passed, $fail failed ($(basename "$prog") $(env -i PATH=/usr/bin:/bin DISPLAY="$dn" "$prog" --version 2>/dev/null | head -1) on Xvfb)"
[ "$fail" -eq 0 ]
