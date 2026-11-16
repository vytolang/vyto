#!/bin/sh
# A stand-in for CUPS's lpstat, lp and cancel, for tests/run_tests.sh
# (print_cli). Installed under those three names in a temp dir that
# VYTO_PRINT_BIN_DIR points at.
#
# Logs every argv element on its own line, bracketed, to $FAKE_DIR/log (so a
# hostile queue name or title is seen to arrive as ONE argument, byte for
# byte), notes the locale it was given, then answers from
# $FAKE_DIR/<prog>_<first arg>.out with exit status from the matching .rc
# (default 0). A .sleep file makes it hang that many seconds (the timeout).
# lp also copies the file it was given to $FAKE_DIR/printed.bin.
d=${FAKE_DIR:?FAKE_DIR not set}
p=$(basename "$0")
{
    echo "$p: LC_ALL=${LC_ALL:-}"
    for a in "$@"; do printf '  [%s]\n' "$a"; done
} >> "$d/log"
if [ "$p" = lp ]; then
    for a in "$@"; do last=$a; done
    cat "$last" > "$d/printed.bin"
fi
key=$(printf '%s_%s' "$p" "${1:-}")
[ -f "$d/$key.sleep" ] && sleep "$(cat "$d/$key.sleep")"
[ -f "$d/$key.out" ] && cat "$d/$key.out"
rc=0
[ -f "$d/$key.rc" ] && rc=$(cat "$d/$key.rc")
exit "$rc"
