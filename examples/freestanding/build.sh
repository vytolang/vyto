#!/usr/bin/env bash
# Build a Volt program with the freestanding runtime profile and run it on the
# host — a no-cross-toolchain proof that the -DVT_NO_LIBC path compiles, links,
# and runs. The runtime object references only the six vt_host_* hooks (verified
# below); host_hooks.c bridges them to libc for this stand-in.
set -euo pipefail
cd "$(dirname "$0")/../.."   # repo root

VOLTC=./voltc
SRC=${1:-examples/01_hello.vt}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

echo "== compiling $SRC --freestanding =="
"$VOLTC" build "$SRC" --freestanding --cc gcc -o "$OUT/libapp.a"

echo "== the freestanding runtime references only vt_host_* (no libc) =="
RTOBJ=$(dirname "$SRC")/.volt-cache/volt_rt_gcc_fs.o
nm "$RTOBJ" | grep ' U ' | grep -v 'vt_host_' && { echo "FAIL: libc symbol leaked"; exit 1; } || true

echo "== linking with the libc-backed hook shim =="
gcc -c examples/freestanding/host_hooks.c -o "$OUT/host_hooks.o"
gcc "$OUT/host_hooks.o" "$OUT/libapp.a" -o "$OUT/app"

echo "== running =="
"$OUT/app"
