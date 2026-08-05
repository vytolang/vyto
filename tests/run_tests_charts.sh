#!/bin/sh
# The vyto/ui/chart golden tests, split out of the main suite.
#
#     make test-charts
#
# 21 of the ~55 UI cases are charts, and they are the most expensive third of
# the suite: emitted Vyto C is per-entry-file (generics are monomorphized into
# the module that declared them), so every one of them compiles its own copy of
# the toolkit rather than sharing. Measured at roughly 7s each from a cold
# cache — about 2.5 minutes for the chart cases alone.
#
# They are also the least likely thing to be broken by a change to anything
# else: chart.vt is a leaf, nothing else in the toolkit imports it. Splitting
# them out keeps `make test` a tolerable inner-loop check while leaving the
# coverage intact and one command away.
#
# Run this before touching lib/vyto/ui/chart.vt, and before a release.
set -u
cd "$(dirname "$0")/.."
fail=0

if [ ! -x ./vytoc ]; then
    echo "FATAL: ./vytoc not built — run 'make' first"
    exit 1
fi

# Same cache rule as the main runner: these share tests/ui/.vyto-cache with it,
# so a stale object would make them validate the previous build. Wipe only when
# something they were built from is newer than the stamp.
ui_stamp=tests/ui/.vyto-cache/.suite-stamp
if [ ! -f "$ui_stamp" ]; then
    rm -rf tests/ui/.vyto-cache
elif [ -n "$(find src lib runtime vytoc -name .vyto-cache -prune -o \
                  -newer "$ui_stamp" -type f -print -quit 2>/dev/null)" ]; then
    rm -rf tests/ui/.vyto-cache
fi
mkdir -p tests/ui/.vyto-cache
touch "$ui_stamp"

for src in tests/ui/*_chart_*.vt; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .vt)
    got=$(VS_HEADLESS=1 VS_EVENTS="tests/ui/$name.events" ./vytoc run "$src" 2>&1)
    if [ "$got" = "$(cat "tests/ui/$name.expected")" ]; then
        echo "PASS ui_$name"
    else
        echo "FAIL ui_$name"
        echo "--- expected ---"; cat "tests/ui/$name.expected"
        echo "--- got ---"; printf '%s\n' "$got"
        fail=1
    fi
done

exit $fail
