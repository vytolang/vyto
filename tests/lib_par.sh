# ---- parallel section runner -----------------------------------------------
#
# Sourced by tests/run_tests.sh and tests/run_tests_dev.sh. Not executable on
# its own — it defines functions and expects the caller to have cd'd to the
# repo root and created tests/tmp.
#
# Every case in a parallel section compiles its own C, which is the expensive
# part and is CPU-bound. That is safe because .vyto-cache is keyed by ENTRY FILE
# (src/main.c), so two programs in one directory never write the same mod_*.c —
# before that, concurrent builds raced and failed intermittently with
# `undefined reference` to stdlib symbols.
#
# Cache safety is necessary but not sufficient: a section may only be run this
# way if its cases share no other mutable state. Sections that deliberately
# manipulate a shared cache (cross-compile, freestanding, the db driver
# contagion checks) or write to a fixed tests/tmp path stay sequential.
#
# Usage:  par_run <section-name> <worker-script-body-file> < list-of-args
# Each worker writes its whole verdict to tests/tmp/<section>/<key>, which is
# then replayed in sorted order — so a FAIL's expected/got block can never
# interleave with another worker's output, and the log reads identically run to
# run regardless of who finished first.
#
# par_finish sets `fail=1` in the CALLER's shell, so every sourcing script must
# have `fail` initialised before calling it.
TEST_JOBS=${VYTO_TEST_JOBS:-$(nproc 2>/dev/null || echo 4)}

par_dir() { echo "tests/tmp/par_$1"; }

# par_begin <section>: fresh results dir, emit its path
par_begin() {
    d=$(par_dir "$1"); rm -rf "$d"; mkdir -p "$d"; echo "$d"
}

# par_run <section> <worker>: run the worker over stdin, $1 = one input line.
# The worker is a standalone script, not a function: `xargs` starts a fresh
# shell that would not inherit one.
par_run() {
    _d=$(par_dir "$1"); _w="$_d/.worker"
    cat > "$_w"; chmod +x "$_w"
    if [ -s "$_d/.queue" ]; then
        # -d '\n': one QUEUE LINE is one argument. Without it xargs splits on
        # any whitespace, which silently shreds a line that carries fields or a
        # path containing a space.
        xargs -d '\n' -P "$TEST_JOBS" -n1 "$_w" < "$_d/.queue"
    fi
    rm -f "$_w" "$_d/.queue"
}

# par_finish <section>: replay verdicts sorted, then recover the failure flag
# (it cannot cross a pipe or subshell).
par_finish() {
    _d=$(par_dir "$1")
    for _f in $(ls "$_d" 2>/dev/null | sort); do cat "$_d/$_f"; done
    if grep -rq '^FAIL ' "$_d" 2>/dev/null; then fail=1; fi
    rm -rf "$_d"
}
