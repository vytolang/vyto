#!/bin/sh
# Vendor a private ICU for vyto/intl (the hybrid provisioning path).
#
# By default vyto/intl links the *system* ICU (#link "icuuc"/"icui18n" in
# intl.vt), which needs no vendored files. Run this script only when you want a
# self-contained build — `vytoc build --bundle` (static ICU baked into the exe)
# or a target with no system ICU headers (macOS/Windows). It drops:
#   native/src/unicode/*.h              (headers — shim compiles via -Inative/src)
#   native/<triple>/libicuuc.so   .a    (common: Unicode props, normalization…)
#   native/<triple>/libicui18n.so .a    (i18n: collation, number/date, plurals)
#   native/<triple>/libicudata.so .a    (the CLDR data blob)
#   native/<triple>/libicu*.a.deps      (extra -l flags each static archive needs)
#
# Requires: curl, tar, a C++ compiler, make. ICU is built with both shared and
# static libraries. Data is linked in (not a separate .dat file) so a bundled
# exe stays self-contained.
set -e

TRIPLE="${1:-linux-x64}"
ICU_VER="${ICU_VER:-74-2}"                       # release tag: release-<VER>
ICU_UND="$(echo "$ICU_VER" | tr - _)"            # tarball uses 74_2
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

URL="https://github.com/unicode-org/icu/releases/download/release-${ICU_VER}/icu4c-${ICU_UND}-src.tgz"
echo "[vyto/intl] downloading ICU ${ICU_VER}…"
curl -fsSL "$URL" -o "$WORK/icu.tgz"
tar -xzf "$WORK/icu.tgz" -C "$WORK"               # -> $WORK/icu/source

echo "[vyto/intl] configuring + building ICU (shared + static)…"
# --enable-static gives the .a archives for --bundle; data is linked into the
# libraries (--with-data-packaging=static) so no runtime .dat file is needed.
( cd "$WORK/icu/source"
  ./runConfigureICU Linux \
      --enable-static --enable-shared \
      --with-data-packaging=static \
      --prefix="$WORK/out" >/dev/null
  make -j"$(nproc)" >/dev/null
  make install >/dev/null )

echo "[vyto/intl] installing headers + libs for $TRIPLE…"
rm -rf "$HERE/src/unicode"
mkdir -p "$HERE/src/unicode" "$HERE/$TRIPLE"
cp "$WORK/out/include/unicode/"*.h "$HERE/src/unicode/"

# Shared objects: copy the real versioned .so and give it the plain SONAME the
# linker's -licuXX expects (vytoc ships whatever .so is in native/<triple>).
for base in icuuc icui18n icudata; do
    real="$(ls "$WORK/out/lib/lib${base}.so."*.* 2>/dev/null | head -n1 || true)"
    [ -z "$real" ] && real="$(ls "$WORK/out/lib/lib${base}.so"* | head -n1)"
    cp "$real" "$HERE/$TRIPLE/lib${base}.so"
    # static archive for --bundle
    cp "$WORK/out/lib/lib${base}.a" "$HERE/$TRIPLE/lib${base}.a"
done

# Static-link deps. The archives reference each other (i18n -> uc -> data) and
# ICU's C++ runtime; --bundle lists all three .a plus these. vytoc reads a
# per-archive "<lib>.a.deps"; putting the shared closure on each is harmless and
# order-robust once ld sees the group.
DEPS='-licudata -lstdc++ -lpthread -ldl -lm'
for base in icuuc icui18n icudata; do
    printf '%s\n' "$DEPS" > "$HERE/$TRIPLE/lib${base}.a.deps"
done

echo "[vyto/intl] done: $HERE/$TRIPLE/{libicuuc,libicui18n,libicudata}.{so,a}"
echo "[vyto/intl] system #link still works; --bundle now links the vendored static ICU."
