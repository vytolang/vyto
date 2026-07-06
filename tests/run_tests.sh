#!/bin/sh
# Runs every examples/NN_*.vt and diffs stdout against examples/NN_*.expected.
set -u
cd "$(dirname "$0")/.."
fail=0
for src in examples/[0-9]*.vt; do
    name=$(basename "$src" .vt)
    expected="examples/$name.expected"
    got=$(./voltc run "$src" 2>&1)
    if [ ! -f "$expected" ]; then
        echo "SKIP $name (no .expected)"
        continue
    fi
    if [ "$got" = "$(cat "$expected")" ]; then
        echo "PASS $name"
    else
        echo "FAIL $name"
        echo "--- expected ---"
        cat "$expected"
        echo "--- got ---"
        printf '%s\n' "$got"
        fail=1
    fi
done
exit $fail
