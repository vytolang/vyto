#!/bin/sh
# The vyto/mobile/android widget goldens, split out of the main suite.
#
#     make test-mobile
#
# Headless, scripted events, no toolchain — these run anywhere the host suite
# runs. Not to be confused with tests/run_tests_android.sh, which needs an NDK
# and an SDK and checks that the port *builds*; this checks that the widgets
# *behave*.
#
# Split out for the same reason as the charts: vyto/mobile/android/ui is a leaf
# that nothing else imports, so these cannot be broken by a change anywhere
# else in the toolkit — while each one compiles its own copy of the whole ui
# stack (emitted Vyto C is per-entry-file, because generics are monomorphized
# into the module that declared them) and so costs several seconds.
#
# Deliberately NOT moved: 53_dragscroll and 54_dragscroll_nested. They import
# only vyto/ui and exercise Widget.on_drag's walk up the parent chain, which is
# core toolkit behaviour that desktop apps rely on too. They belong in the run
# that guards core.vt.
#
# Run this before touching lib/vyto/mobile/android/ui.vt, and before a release.
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

for src in tests/ui/*_mobile_*.vt; do
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
