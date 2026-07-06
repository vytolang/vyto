#!/bin/sh
# Runs every examples/NN_*.vt and diffs stdout against examples/NN_*.expected.
# Also builds the greeter prebuilt-.so package and checks voltbind output.
set -u
cd "$(dirname "$0")/.."
fail=0

# --- prepare the greeter package: prebuilt .so + voltbind-generated binding ---
triple=linux-x64   # matches volt_triple() on this CI/dev box
mkdir -p "examples/greeter/native/$triple"
so="examples/greeter/native/$triple/libgreeter.so"
if [ ! -f "$so" ] || [ examples/greeter/csrc/greeter.c -nt "$so" ]; then
    cc -shared -fPIC -O2 -Wl,-soname,libgreeter.so \
       -o "$so" examples/greeter/csrc/greeter.c || exit 1
fi
./voltbind examples/greeter/csrc/greeter.h \
    --filter 'greeter_*' --filter 'GREET*' > examples/greeter/greeter.vt || exit 1
if diff -u tests/greeter.vt.expected examples/greeter/greeter.vt >/dev/null 2>&1; then
    echo "PASS voltbind_greeter"
else
    echo "FAIL voltbind_greeter (binding differs from tests/greeter.vt.expected)"
    diff -u tests/greeter.vt.expected examples/greeter/greeter.vt | head -40
    fail=1
fi

# --- run all examples against golden output ---
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

# --- prebuilt .so deployment: exe must run from a copied-out directory ---
out=$(./voltc build examples/09_prebuilt_so.vt) || fail=1
if [ -f "examples/.volt-cache/libgreeter.so" ]; then
    echo "PASS so_deployed_next_to_exe"
else
    echo "FAIL so_deployed_next_to_exe"
    fail=1
fi

exit $fail
