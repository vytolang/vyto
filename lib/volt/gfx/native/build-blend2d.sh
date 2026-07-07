#!/bin/sh
# Populate the blend2d prebuilt dependency for volt/gfx.
#
# blend2d is a large C++ library, so it is NOT vendored in git; this script
# builds it once and drops the pieces the package needs:
#   native/src/blend2d/**.h        (headers — so the shim compiles, -Inative/src)
#   native/<triple>/libblend2d.so  (shared lib — default: linked + shipped)
#   native/<triple>/libblend2d.a   (static archive — for `voltc build --bundle`)
#   native/<triple>/libblend2d.a.deps  (extra -l flags the static archive needs)
#
# Requires: git, cmake (>=3.22 after the pin below), a C++ compiler, ninja.
# Config: JIT off (self-contained, no asmjit; NO_JIT is also the config that
# hardened W^X/iOS targets need). Both shared and static are built.
set -e

TRIPLE="${1:-linux-x64}"
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "[volt/gfx] cloning blend2d…"
git clone --depth 1 https://github.com/blend2d/blend2d "$WORK/blend2d"

# blend2d pins cmake >=3.24; relax to 3.22 (builds fine).
sed -i 's/cmake_minimum_required(VERSION [0-9.]*/cmake_minimum_required(VERSION 3.22/' \
    "$WORK/blend2d/CMakeLists.txt"

echo "[volt/gfx] building libblend2d (shared, NO_JIT)…"
cmake -S "$WORK/blend2d" -B "$WORK/build-shared" -GNinja -DCMAKE_BUILD_TYPE=Release \
      -DBLEND2D_STATIC=FALSE -DBLEND2D_NO_JIT=TRUE >/dev/null
ninja -C "$WORK/build-shared" blend2d

echo "[volt/gfx] building libblend2d (static, NO_JIT) for --bundle…"
cmake -S "$WORK/blend2d" -B "$WORK/build-static" -GNinja -DCMAKE_BUILD_TYPE=Release \
      -DBLEND2D_STATIC=TRUE -DBLEND2D_NO_JIT=TRUE >/dev/null
ninja -C "$WORK/build-static" blend2d

echo "[volt/gfx] installing headers + libs for $TRIPLE…"
rm -rf "$HERE/src/blend2d"
mkdir -p "$HERE/src/blend2d" "$HERE/$TRIPLE"
# headers only, preserving the include tree
(cd "$WORK/blend2d/blend2d" && find . -name '*.h' -print | cpio -pdm "$HERE/src/blend2d" 2>/dev/null)
cp "$WORK/build-shared/libblend2d.so" "$HERE/$TRIPLE/libblend2d.so"
cp "$WORK/build-static/libblend2d.a" "$HERE/$TRIPLE/libblend2d.a"
# extra libs the static archive needs (the shared .so pulls these via DT_NEEDED)
printf -- '-lpthread\n' > "$HERE/$TRIPLE/libblend2d.a.deps"

echo "[volt/gfx] done: $HERE/$TRIPLE/{libblend2d.so, libblend2d.a}"
