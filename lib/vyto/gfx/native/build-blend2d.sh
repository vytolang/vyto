#!/bin/sh
# Populate the blend2d prebuilt dependency for vyto/gfx.
#
# blend2d is a large C++ library, so it is NOT vendored in git; this script
# builds it and drops the pieces the package needs:
#   native/src/blend2d/**.h            (headers — so the shim compiles, -Inative/src)
#   native/<triple>/libblend2d.{so,dll} (shared lib — default: linked + shipped)
#   native/<triple>/libblend2d.a        (static archive — for `vytoc build --bundle`)
#   native/<triple>/libblend2d.a.deps   (extra -l flags the static archive needs)
#
# Usage:
#   build-blend2d.sh                     # linux-x64 (or every triple already built)
#   build-blend2d.sh windows-x64         # add a triple, pinned to the built revision
#   build-blend2d.sh --refresh [triple…] # move to current upstream, rebuild everything
#   build-blend2d.sh --dry-run …         # say what would be built, then stop
#
# A full build is minutes per triple and replaces libraries that currently work,
# so --dry-run is there to confirm the triple list first.
#
# ONE CLONE, ONE REVISION. The headers in native/src/blend2d/ are shared by every
# triple, so a per-triple build that re-cloned upstream would leave the headers
# describing a newer blend2d than the libraries already on disk were compiled
# from — a silent ABI mismatch. The revision is therefore stamped in
# native/blend2d.commit and later runs check that exact commit out,
# so adding a platform cannot disturb the ones already working. --refresh is the
# deliberate way to move forward, and it rebuilds every triple present.
#
# Requires: git, cmake (>=3.22 after the pin below), ninja, a C++ compiler, and
# for windows-x64 the mingw cross toolchain:
#     sudo apt-get install -y g++-mingw-w64-x86-64
# Config: JIT off (self-contained, no asmjit; NO_JIT is also the config that
# hardened W^X/iOS targets need). Both shared and static are built.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
# Beside this script, NOT inside src/blend2d/ — that directory is gitignored
# (it is generated), so a stamp in there would never leave this machine and the
# pin would be worthless to anyone else cloning the repo.
STAMP="$HERE/blend2d.commit"
REFRESH=0
DRYRUN=0

TRIPLES=""
for a in "$@"; do
    case "$a" in
        --refresh) REFRESH=1 ;;
        --dry-run) DRYRUN=1 ;;
        -*) echo "unknown option: $a" >&2; exit 2 ;;
        *) TRIPLES="$TRIPLES $a" ;;
    esac
done

# Triples already built here: every subdirectory of native/ except src/.
existing=""
for d in "$HERE"/*/; do
    d="${d%/}"
    name="$(basename "$d")"
    [ "$name" = "src" ] && continue
    existing="$existing $name"
done

# With no stamp the existing libraries' revision is unknown, so pinning would be
# a lie — rebuild everything from one clone to establish a known baseline. Same
# for --refresh, which deliberately moves the revision forward.
if [ -z "$TRIPLES" ]; then
    TRIPLES="${existing:- linux-x64}"
elif [ ! -f "$STAMP" ] || [ "$REFRESH" = 1 ]; then
    for e in $existing; do
        case " $TRIPLES " in *" $e "*) ;; *) TRIPLES="$TRIPLES $e" ;; esac
    done
fi

# Reject unknown triples before cloning, not after — configure_triple would
# otherwise bail several minutes into the run.
for t in $TRIPLES; do
    case "$t" in
        linux-x64|windows-x64) ;;
        *) echo "no cross configuration for triple '$t'" >&2; exit 2 ;;
    esac
done

if [ -f "$STAMP" ] && [ "$REFRESH" = 0 ]; then
    echo "[vyto/gfx] revision: pinned $(cat "$STAMP")"
else
    echo "[vyto/gfx] revision: current upstream master (no pin$([ "$REFRESH" = 1 ] && echo ", --refresh"))"
fi
echo "[vyto/gfx] already built:${existing:- (none)}"
echo "[vyto/gfx] will build:$TRIPLES"
[ "$DRYRUN" = 1 ] && exit 0

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if [ -f "$STAMP" ] && [ "$REFRESH" = 0 ]; then
    PIN="$(cat "$STAMP")"
    echo "[vyto/gfx] fetching blend2d at the pinned revision $PIN…"
    mkdir -p "$WORK/blend2d"
    (cd "$WORK/blend2d" && git init -q . &&
     git remote add origin https://github.com/blend2d/blend2d &&
     git fetch -q --depth 1 origin "$PIN" && git checkout -q FETCH_HEAD) || {
        echo "cannot fetch pinned revision $PIN — upstream may have pruned it." >&2
        echo "Re-run with --refresh to move to current master (rebuilds every triple)." >&2
        exit 1
    }
else
    echo "[vyto/gfx] cloning blend2d (current master)…"
    git clone -q --depth 1 https://github.com/blend2d/blend2d "$WORK/blend2d"
fi
COMMIT="$(cd "$WORK/blend2d" && git rev-parse HEAD)"

# blend2d pins cmake >=3.24; relax to 3.22 (builds fine).
sed -i 's/cmake_minimum_required(VERSION [0-9.]*/cmake_minimum_required(VERSION 3.22/' \
    "$WORK/blend2d/CMakeLists.txt"

# Per-triple cmake configuration. Cross triples get a toolchain file; the host
# triple gets none (plain native build, as before). Sets CFG_TOOLCHAIN and
# CFG_LDFLAGS rather than echoing them — CFG_LDFLAGS contains spaces, and a
# `set -- $(...)` round trip would split it into separate cmake arguments.
configure_triple() {
    CFG_TOOLCHAIN=""
    CFG_LDFLAGS=""
    case "$1" in
    windows-x64)
        cat > "$WORK/mingw-w64.cmake" <<'EOF'
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
# The -posix variants, not the default -win32 ones: GCC 10's win32-threads
# libstdc++ has no std::thread, which blend2d's worker pool needs. The pthread
# shim is linked statically below, so this is invisible to anything downstream.
set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
        # Static GCC/C++/pthread runtimes so the shipped DLL needs no
        # libgcc_s_seh-1.dll / libstdc++-6.dll / libwinpthread-1.dll alongside it.
        CFG_TOOLCHAIN="$WORK/mingw-w64.cmake"
        CFG_LDFLAGS="-static -static-libgcc -static-libstdc++"
        ;;
    linux-x64) ;;  # native
    *)
        echo "no cross configuration for triple '$1'" >&2
        exit 2
        ;;
    esac
}

# Shared library file name for a triple.
shared_name() {
    case "$1" in
    windows-x64) echo "libblend2d.dll" ;;
    *)           echo "libblend2d.so" ;;
    esac
}

# Extra -l flags the STATIC archive needs at final link. vytoc already adds
# -lstdc++ / -static-libstdc++ / -static-libgcc for --bundle, so this is only
# what is left over.
static_deps() {
    case "$1" in
    windows-x64) echo "-lwinpthread" ;;
    *)           echo "-lpthread" ;;
    esac
}

for TRIPLE in $TRIPLES; do
    echo "[vyto/gfx] === $TRIPLE ==="
    configure_triple "$TRIPLE"
    if [ -n "$CFG_TOOLCHAIN" ]; then
        set -- "-DCMAKE_TOOLCHAIN_FILE=$CFG_TOOLCHAIN" \
               "-DCMAKE_SHARED_LINKER_FLAGS=$CFG_LDFLAGS"
    else
        set --
    fi

    echo "[vyto/gfx] building libblend2d (shared, NO_JIT)…"
    cmake -S "$WORK/blend2d" -B "$WORK/build-shared-$TRIPLE" -GNinja \
          -DCMAKE_BUILD_TYPE=Release -DBLEND2D_STATIC=FALSE -DBLEND2D_NO_JIT=TRUE \
          "$@" >/dev/null
    ninja -C "$WORK/build-shared-$TRIPLE" blend2d

    echo "[vyto/gfx] building libblend2d (static, NO_JIT) for --bundle…"
    cmake -S "$WORK/blend2d" -B "$WORK/build-static-$TRIPLE" -GNinja \
          -DCMAKE_BUILD_TYPE=Release -DBLEND2D_STATIC=TRUE -DBLEND2D_NO_JIT=TRUE \
          "$@" >/dev/null
    ninja -C "$WORK/build-static-$TRIPLE" blend2d

    echo "[vyto/gfx] installing libs for $TRIPLE…"
    mkdir -p "$HERE/$TRIPLE"
    so="$(shared_name "$TRIPLE")"
    cp "$WORK/build-shared-$TRIPLE/$so" "$HERE/$TRIPLE/$so"
    cp "$WORK/build-static-$TRIPLE/libblend2d.a" "$HERE/$TRIPLE/libblend2d.a"
    static_deps "$TRIPLE" > "$HERE/$TRIPLE/libblend2d.a.deps"
    # Deliberately NOT installed on Windows: the libblend2d.dll.a import library.
    # vytoc treats every *.a in the triple dir as the static archive for
    # --bundle, so shipping both would link blend2d twice. It links the .dll
    # directly instead, which mingw ld supports.
done

echo "[vyto/gfx] installing headers…"
rm -rf "$HERE/src/blend2d"
mkdir -p "$HERE/src/blend2d"
# headers only, preserving the include tree
(cd "$WORK/blend2d/blend2d" && find . -name '*.h' -print | cpio -pdm "$HERE/src/blend2d" 2>/dev/null)
printf '%s\n' "$COMMIT" > "$STAMP"

echo "[vyto/gfx] done at $COMMIT —$TRIPLES"
