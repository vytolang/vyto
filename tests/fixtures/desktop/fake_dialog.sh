#!/bin/sh
# A stand-in for zenity, kdialog and xdg-open, for tests/run_tests.sh
# (desktop_*). Installed under those names in a temp dir; vyto/desktop finds it
# through VYTO_DESKTOP_DIALOG / VYTO_DESKTOP_OPEN.
#
# Logs every argv element on its own line, bracketed, to $FAKE_DIR/log — so a
# test can see that a hostile title arrived as ONE argument, byte for byte —
# then replays the next canned answer from $FAKE_DIR/replies: one line per
# call, "<exit status>|<stdout>", where stdout may contain \n escapes.
d=${FAKE_DIR:?FAKE_DIR not set}
if [ "${1:-}" = "--version" ]; then
    echo "${FAKE_ZENITY_VERSION:-3.42.1}"
    exit 0
fi
{
    echo "$(basename "$0"):"
    for a in "$@"; do printf '  [%s]\n' "$a"; done
} >> "$d/log"
line=$(head -n 1 "$d/replies" 2>/dev/null)
tail -n +2 "$d/replies" > "$d/replies.next" 2>/dev/null
mv "$d/replies.next" "$d/replies"
code=${line%%|*}
reply=${line#*|}
[ -n "$reply" ] && printf '%b\n' "$reply"
exit "${code:-0}"
