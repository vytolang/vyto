#!/bin/sh
# Unit tests written against lib/vyto/dev/test.
#
#     make test-dev
#
# Each tests/unit/*.vt is a self-verifying program: it prints one verdict line
# per assertion, then a summary, then exits 0 or 1. This runner checks TWO
# things about that output, and it needs both:
#
#   1. the exit status  — did the suite consider itself green
#   2. the SUMMARY LINE — did the suite reach the end at all
#
# (2) is not redundant. `panic()` cannot be caught in this language: when code
# under test panics, the process dies at exit(101) having already printed some
# `ok` lines and never reaching its summary. Judging on exit status alone would
# score that the same as a clean failure; judging on "some assertions passed"
# would call it green. Requiring the summary line is what makes an ABORTED suite
# a failure rather than a silent partial pass. tests/unit/aborts_midway.vt is
# the negative test that proves this check is live — see below.
#
# Split out of `make test` for the same reason test-pack is: it is its own
# concern and `make test` is already slow. That means a green `make test` says
# nothing about this target, so run it after touching lib/vyto/dev/test or
# anything under tests/unit.
set -u
cd "$(dirname "$0")/.."
fail=0

if [ ! -x ./vytoc ]; then
    echo "FATAL: ./vytoc not built — run 'make' first"
    exit 1
fi

mkdir -p tests/tmp

# par_begin / par_run / par_finish, shared with tests/run_tests.sh.
. tests/lib_par.sh

# Two files are not ordinary suites. They are checked separately, below, and
# skipping them here keeps the ordinary path free of special cases.
#
#   selftest      — deliberately contains failing assertions, so its own verdict
#                   is red by design; it is diffed against a golden instead.
#   aborts_midway — deliberately panics, and must be REPORTED failing.
#   silent_exit   — deliberately exits 0 without a summary, and must likewise be
#                   REPORTED failing.
#   red_suite     — an ordinary failing suite, which must also be REPORTED
#                   failing.
#   lying_suite   — reports green but exits nonzero; likewise REPORTED failing.
#
# Parallel is safe for the same reason the examples section is: .vyto-cache is
# keyed by entry file (src/main.c), so no two of these write the same mod_*.c.
# A test that needs scratch space must use tests/tmp/<its own name>/ — a fixed
# shared path would make this unsafe, and that constraint is invisible from
# inside the .vt file, so it is stated here.
d=$(par_begin dev)
: > "$d/.queue"
for src in tests/unit/*.vt; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .vt)
    case "$name" in
        selftest|aborts_midway|silent_exit|red_suite|lying_suite) continue ;;
    esac
    printf '%s\n' "$src" >> "$d/.queue"
done

# The verdict for one ordinary suite, as a standalone script. It is written out
# rather than inlined because BOTH callers need it: xargs runs it per file here,
# and the abort-liveness check below runs this same script directly. Sharing it
# is the point — a check that reimplemented this logic would keep passing when
# the real one broke, which is exactly what the fault-injection matrix caught.
verdict=tests/tmp/dev_verdict.sh
cat > "$verdict" <<'WORKER'
#!/bin/sh
src="$1"; name=$(basename "$src" .vt); out="${2:-tests/tmp/par_dev/$name}"
got=$(./vytoc run "$src" 2>&1); rc=$?
if [ "$rc" = 0 ] && printf '%s\n' "$got" | grep -q '^# .* ok$'; then
    echo "PASS unit_$name" > "$out"
else
    { echo "FAIL unit_$name (exit $rc)"
      printf '%s\n' "$got" | grep -E '^(not ok|# )' | head -30
    } > "$out"
fi
WORKER
chmod +x "$verdict"

par_run dev < /dev/null <<WORKER
#!/bin/sh
exec ./$verdict "\$1"
WORKER
par_finish dev

# --- the framework asserting on itself -------------------------------------
#
# selftest exercises every assertion in both directions, so its output is the
# framework's behaviour written down. Diffing it byte for byte is what catches
# an assertion that stopped printing, one that reports the wrong verdict, a
# miscount, or a regression in the got/want block — none of which the suite
# could report about itself. The witness here is `diff`, which has no
# relationship to the code under test.
#
# Its exit status is 1 BY DESIGN (it contains deliberate failures), so the exit
# code is asserted to be 1 rather than 0. A selftest that suddenly exited 0
# would mean its failing half stopped failing.
if [ -f tests/unit/selftest.expected ]; then
    got=$(./vytoc run tests/unit/selftest.vt 2>&1); rc=$?
    if [ "$rc" != 1 ]; then
        echo "FAIL unit_selftest (exit $rc, expected 1 — the deliberate failures are not failing)"
        fail=1
    elif [ "$got" = "$(cat tests/unit/selftest.expected)" ]; then
        echo "PASS unit_selftest"
    else
        echo "FAIL unit_selftest (output differs from golden)"
        printf '%s\n' "$got" | diff tests/unit/selftest.expected - | head -40
        fail=1
    fi
else
    echo "SKIP unit_selftest (no .expected)"
fi

# --- suites that must be REPORTED failing ----------------------------------
#
# Four ways a suite can be bad, covering both halves of the verdict check so
# that neither can rot unnoticed. Note the grep is `^# .* ok$`, which asks two
# questions at once — is there a summary, and does it say green — so it alone
# accounts for the first three:
#
#   red_suite     — an ordinary failure: runs to the end, prints a correct
#                   summary that ends FAILED, exits 1. Caught by the grep.
#   lying_suite   — prints a well-formed GREEN summary and exits 1 anyway. The
#                   grep is satisfied, so only the EXIT STATUS catches it, and
#                   no other fixture here reaches that clause. A report and an
#                   exit code that disagree is what a done() returning 0 with
#                   correct tallies would produce, or a hand-rolled
#                   `exitWith(0)`; the runner must believe the worse of the two.
#   silent_exit   — returns from main() without exitWith(t.done()), so it exits
#                   0 having asserted almost nothing. Exit status says green;
#                   only the missing SUMMARY LINE gives it away. The likeliest
#                   of these three in practice — a forgotten last line, or an
#                   early return from a guard sitting directly in main().
#   aborts_midway — panics, so it exits 101 AND prints no summary. Caught by
#                   either half, which is why it cannot stand in for the other
#                   two: with either check deleted it still comes out failing.
#
# Both run through the SAME verdict script the ordinary suites use, and the
# PASS condition is that it came back FAIL. Reimplementing the logic here would
# make these vacuous: a hand-written copy keeps passing when the real check
# breaks, because the two paths never meet.
must_fail() { # must_fail <name> <what it proves>
    _v=tests/tmp/dev_negcheck
    ./"$verdict" "tests/unit/$1.vt" "$_v"
    if grep -q '^FAIL ' "$_v"; then
        echo "PASS unit_$1 ($2)"
    else
        echo "FAIL unit_$1 (was scored green; $2 is not enforced)"
        # Indent the verdict that was wrongly green, so its own PASS line
        # cannot be mistaken for this check's verdict when scanning the log.
        sed 's/^/  /' "$_v"
        fail=1
    fi
    rm -f "$_v"
}
must_fail red_suite     "an ordinary failing suite is caught"
must_fail lying_suite   "a suite that reports green but exits red is caught"
must_fail aborts_midway "a panicking suite is caught"
must_fail silent_exit   "a suite that never reports is caught"
rm -f "$verdict"

if [ "$fail" -eq 0 ]; then echo "all dev tests passed"; else echo "dev tests FAILED"; fi
exit $fail
