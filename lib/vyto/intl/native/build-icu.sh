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

COMMON="--enable-static --enable-shared --with-data-packaging=static"

if [ "$TRIPLE" = "windows-x64" ]; then
    # ICU cannot cross-compile in one pass: the build runs its own freshly built
    # tools (genrb, pkgdata, …) to generate the locale data, and those have to be
    # host binaries. Build natively first purely to get them, then point the
    # cross build at that tree with --with-cross-build.
    command -v x86_64-w64-mingw32-g++-posix >/dev/null 2>&1 || {
        echo "windows-x64 ICU needs g++-mingw-w64-x86-64" >&2; exit 1; }

    echo "[vyto/intl] building host ICU first (only for the cross tools)…"
    mkdir -p "$WORK/host"
    ( cd "$WORK/host"
      sh "$WORK/icu/source/runConfigureICU" Linux --enable-static >/dev/null
      make -j"$(nproc)" >/dev/null )

    echo "[vyto/intl] cross-building ICU for windows-x64…"
    mkdir -p "$WORK/cross"
    # data-packaging=library, not the `static` the POSIX branch uses: with
    # static packaging the CLDR data goes into libsicudt.a and icudt74.dll is
    # left a ~30 KB stub with no data in it, so the shipped DLLs load and then
    # fail every collation/format call with a missing-resource error. Windows
    # here is a shared-library deployment, so the data has to be a real DLL.
    # (Consequence: `vytoc build --bundle` with vyto/intl is not supported on
    # windows-x64 — it would need a second, static-packaged build.)
    CROSS_COMMON="$(echo "$COMMON" | sed 's/--with-data-packaging=static/--with-data-packaging=library/')"
    # The -posix toolchain variants, not the default -win32 ones. GCC 10's
    # win32-threads libstdc++ has no std::mutex, and ICU's UMutex then degrades
    # its fMutex member to a plain int and fails to compile with
    # "request for member 'load' ... which is of non-class type 'int'".
    # The shipped DLLs must need no GCC runtime beside them, as blend2d's do.
    # -static-lib{gcc,stdc++} alone is not enough — the -posix toolchain still
    # links libwinpthread-1.dll dynamically — but a plain -static (which works
    # for blend2d) cannot be used here: it also forces static resolution of
    # ICU's *own* inter-library references and the link dies on
    # "cannot find -licudt", since the data library is a DLL. So winpthread
    # alone is pulled in whole, with -Bdynamic restored for everything after it.
    # --exclude-libs=ALL is
    # required alongside it: without it icuuc.dll re-exports the static libgcc
    # symbols it absorbed, and linking icuin against both that import library
    # and libgcc_eh.a fails with "multiple definition of `_Unwind_Resume'".
    ( cd "$WORK/cross"
      sh "$WORK/icu/source/configure" \
          --host=x86_64-w64-mingw32 --with-cross-build="$WORK/host" \
          $CROSS_COMMON --prefix="$WORK/out" \
          CC=x86_64-w64-mingw32-gcc-posix CXX=x86_64-w64-mingw32-g++-posix \
          LDFLAGS="-static-libgcc -static-libstdc++ -Wl,--exclude-libs,ALL \
-Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive,-Bdynamic" >/dev/null
      make -j"$(nproc)" >/dev/null
      make install >/dev/null )
else
    echo "[vyto/intl] configuring + building ICU (shared + static)…"
    # --enable-static gives the .a archives for --bundle; data is linked into the
    # libraries (--with-data-packaging=static) so no runtime .dat file is needed.
    ( cd "$WORK/icu/source"
      ./runConfigureICU Linux $COMMON --prefix="$WORK/out" >/dev/null
      make -j"$(nproc)" >/dev/null
      make install >/dev/null )
fi

echo "[vyto/intl] installing headers + libs for $TRIPLE…"
mkdir -p "$HERE/$TRIPLE"
# Headers go in native/<triple>/include, which vytoc searches ahead of
# native/src for that target only. They must NOT go in native/src: that is
# shared by every triple, and ICU version-suffixes every symbol, so vendoring
# (say) ICU 74 headers there makes a Linux build compile ubrk_open_74 calls and
# then link against a system ICU 70 that exports ubrk_open_70 — every symbol
# undefined. Per-triple keeps a vendored ICU and a system ICU from colliding.
rm -rf "$HERE/$TRIPLE/include/unicode"
mkdir -p "$HERE/$TRIPLE/include/unicode"
cp "$WORK/out/include/unicode/"*.h "$HERE/$TRIPLE/include/unicode/"

if [ "$TRIPLE" = "windows-x64" ]; then
    # ICU's Windows naming differs from its POSIX naming in three ways: no "lib"
    # prefix, the major version is in the file name, and two libraries are
    # abbreviated (i18n -> in, data -> dt). So the DLLs are icuuc74.dll,
    # icuin74.dll and icudt74.dll, and the static archives libsicuuc.a,
    # libsicuin.a, libsicudt.a.
    #
    # The DLLs keep those exact names: icuin74.dll imports icuuc74.dll *by that
    # name*, so renaming them to a POSIX-looking libicuuc.dll would link fine
    # and then fail to load at runtime. vytoc links prebuilts by path and copies
    # them next to the exe, so the names cost nothing.
    #
    # Copied from the build tree rather than the install prefix: ICU's mingw
    # `make install` shells out to `cmd`, which does not exist here, so the
    # install step is only reliable for the headers.
    # Exactly the three vyto/intl uses. icuio and icutu also get built and are
    # 5.5 MB of dead weight here. icudt (the CLDR data) is NOT in lib/ with the
    # others — it is generated under data/out — and icuuc74.dll imports it by
    # name, so omitting it produces an exe that links cleanly and then fails to
    # load ICU at startup. Hence the tree-wide search.
    for stem in icuuc icuin icudt; do
        dll="$(find "$WORK/cross" -name "${stem}*.dll" -print 2>/dev/null | head -n1)"
        [ -n "$dll" ] || {
            echo "no $stem DLL anywhere under $WORK/cross — ICU layout changed." >&2
            echo "DLLs that were built:" >&2
            find "$WORK/cross" -name '*.dll' >&2 || true
            exit 1; }
        cp "$dll" "$HERE/$TRIPLE/$(basename "$dll")"
    done

    # Static archives for --bundle, renamed to the POSIX spelling so the
    # <lib>.a.deps convention below is identical on every platform.
    for pair in "icuuc:sicuuc" "icui18n:sicuin" "icudata:sicudt"; do
        out=${pair%%:*}; in=${pair##*:}
        arc="$(ls "$WORK/cross/lib/lib${in}.a" 2>/dev/null | head -n1 || true)"
        [ -n "$arc" ] && cp "$arc" "$HERE/$TRIPLE/lib${out}.a"
    done
    # No -ldl/-lpthread on Windows; ICU pulls the Win32 bits it needs itself.
    DEPS='-licudata -lstdc++'
else
    # Shared objects: copy the real versioned .so and give it the plain SONAME the
    # linker's -licuXX expects (vytoc ships whatever .so is in native/<triple>).
    for base in icuuc icui18n icudata; do
        real="$(ls "$WORK/out/lib/lib${base}.so."*.* 2>/dev/null | head -n1 || true)"
        [ -z "$real" ] && real="$(ls "$WORK/out/lib/lib${base}.so"* | head -n1)"
        cp "$real" "$HERE/$TRIPLE/lib${base}.so"
        # static archive for --bundle
        cp "$WORK/out/lib/lib${base}.a" "$HERE/$TRIPLE/lib${base}.a"
    done
    DEPS='-licudata -lstdc++ -lpthread -ldl -lm'
fi

# Static-link deps. The archives reference each other (i18n -> uc -> data) and
# ICU's C++ runtime; --bundle lists all three .a plus these. vytoc reads a
# per-archive "<lib>.a.deps"; putting the shared closure on each is harmless and
# order-robust once ld sees the group.
for base in icuuc icui18n icudata; do
    [ -f "$HERE/$TRIPLE/lib${base}.a" ] && printf '%s\n' "$DEPS" > "$HERE/$TRIPLE/lib${base}.a.deps"
done

if [ "$TRIPLE" = "windows-x64" ]; then
    # A DLL that imports the mingw runtime, or a missing data DLL, both link
    # cleanly and only fail when the program starts on Windows — where there is
    # no compiler to explain it. Check here instead.
    bad=0
    for d in "$HERE/$TRIPLE"/*.dll; do
        leak="$(x86_64-w64-mingw32-objdump -p "$d" 2>/dev/null |
                grep -iEo 'lib(winpthread|gcc_s_seh|stdc\+\+)-?[0-9]*\.dll' | sort -u)"
        [ -n "$leak" ] && { echo "  !! $(basename "$d") needs $leak" >&2; bad=1; }
        for dep in $(x86_64-w64-mingw32-objdump -p "$d" 2>/dev/null |
                     sed -n 's/^\s*DLL Name: \(icu[a-z]*[0-9]*\.dll\)$/\1/p'); do
            [ -f "$HERE/$TRIPLE/$dep" ] || { echo "  !! $(basename "$d") needs missing $dep" >&2; bad=1; }
        done
    done
    # A data DLL built with the wrong packaging is a stub: it links, loads, and
    # then every locale lookup fails. Real ICU 74 data is tens of MB.
    dt="$(ls "$HERE/$TRIPLE"/icudt*.dll 2>/dev/null | head -n1)"
    dtsz=$([ -n "$dt" ] && stat -c%s "$dt" || echo 0)
    [ "$dtsz" -gt 5000000 ] || {
        echo "  !! $(basename "${dt:-icudt*.dll}") is ${dtsz} bytes — a stub, not the CLDR data" >&2
        bad=1; }
    [ "$bad" = 0 ] || { echo "[vyto/intl] the DLLs above would fail on Windows" >&2; exit 1; }
    echo "[vyto/intl] DLLs are self-contained, deps present, data is $((dtsz / 1048576)) MB."
    echo "[vyto/intl] done: $HERE/$TRIPLE/{icuuc,icuin,icudt}*.dll + libicu*.a"
else
    echo "[vyto/intl] done: $HERE/$TRIPLE/{libicuuc,libicui18n,libicudata}.{so,a}"
fi
echo "[vyto/intl] system #link still works; --bundle now links the vendored static ICU."
