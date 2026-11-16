#!/bin/sh
# vyto/desktop/print end to end against the real CUPS on this machine: a
# temporary raw queue on socket://127.0.0.1:<port>, its bytes captured by
# socat. make test checks the exact lp/lpstat argv against fakes; this checks
# that real CUPS accepts them and that its real output parses.
#
#   print_real    a file printed raw arrives byte for byte; the job is
#                 followed until it leaves the queue
#   print_held    the queue paused: CUPS still accepts the job and holds it
#                 (the trap a till must not fall into); cancel_job removes
#                 it and nothing reaches the printer
#   print_wine    the Windows arm (winspool) under Wine, which sees CUPS
#                 queues: the same bytes arrive
#
# Not in make test: it adds a queue to the system CUPS (needs lpadmin) and
# needs socat. SKIPs (exit 0) when either is missing. The queue is removed on
# exit, even on failure.
set -u
cd "$(dirname "$0")/.."

for t in lpadmin lpstat lp cancel cupsdisable socat; do
    command -v "$t" >/dev/null 2>&1 || { echo "SKIP print (no $t)"; exit 0; }
done
lpstat -r >/dev/null 2>&1 || { echo "SKIP print (CUPS scheduler not running)"; exit 0; }

fail=0
q="vyto-test-$$"
port=$((19600 + $$ % 300))
tmp=tests/tmp/print_real
rm -rf "$tmp"; mkdir -p "$tmp"
soc=""
cleanup() {
    lpadmin -x "$q" 2>/dev/null
    [ -n "$soc" ] && kill "$soc" 2>/dev/null
}
trap cleanup EXIT INT TERM

socat -u TCP-LISTEN:$port,reuseaddr,fork OPEN:"$tmp/captured.bin",creat,append 2>/dev/null &
soc=$!
sleep 0.3
if ! lpadmin -p "$q" -E -v "socket://127.0.0.1:$port" -m raw 2>/dev/null; then
    echo "SKIP print (lpadmin refused: not in the lpadmin group?)"; exit 0
fi
if ! ./vytoc build tests/fixtures/print_real.vt -o "$tmp/print_real" >"$tmp/build.err" 2>&1; then
    echo "FAIL print_real (did not build)"; cat "$tmp/build.err"; exit 1
fi
# an ESC/POS-ish payload with a NUL and a high byte in it
printf '\033@vyto print test\n\000\377\035VB\000' > "$tmp/job.bin"

got=$("$tmp/print_real" print "$q" "$tmp/job.bin" 2>&1)
sleep 0.5
if [ "$got" = "listed: true
state: ready
queued: true
job: printed" ] && cmp -s "$tmp/job.bin" "$tmp/captured.bin"; then
    echo "PASS print_real"
else
    echo "FAIL print_real"; echo "$got"; od -c "$tmp/captured.bin" | head -3; fail=1
fi

: > "$tmp/captured.bin"
cupsdisable "$q"
got=$("$tmp/print_real" held "$q" "$tmp/job.bin" 2>&1)
left=$(LC_ALL=C lpstat -W not-completed -o "$q" 2>/dev/null)
cupsenable "$q"
sleep 1
if [ "$got" = "listed: true
state: paused
queued: true
held: waiting in the queue
cancelled: true
after cancel: printed" ] && [ -z "$left" ] && [ ! -s "$tmp/captured.bin" ]; then
    echo "PASS print_held"
else
    echo "FAIL print_held"; echo "$got"; echo "left in queue: [$left]"; fail=1
fi

if command -v wine64 >/dev/null 2>&1; then
    cat lib/vyto/win/win7_exports.txt lib/vyto/win/win7_exports_provisional.txt > "$tmp/win7.txt"
    if WIN7_EXPORTS="$tmp/win7.txt" ./vytoc build tests/win/print_demo.vt --target windows-x64 --release --audit \
            -o "$tmp/print_demo.exe" >/dev/null 2>&1; then
        : > "$tmp/captured.bin"
        out=$(WINEPREFIX="${VYTO_WINEPREFIX:-$HOME/.cache/vyto/wineprefix}" WINEDEBUG=-all \
              timeout 120 wine64 "$tmp/print_demo.exe" "$q" "$tmp/job.bin" 2>&1)
        sleep 1
        if echo "$out" | grep -q "^printed" && cmp -s "$tmp/job.bin" "$tmp/captured.bin"; then
            echo "PASS print_wine"
        else
            echo "FAIL print_wine"; echo "$out" | tail -5; fail=1
        fi
    else
        echo "FAIL print_wine (print_demo did not build)"; fail=1
    fi
else
    echo "SKIP print_wine (no wine64)"
fi

exit $fail
