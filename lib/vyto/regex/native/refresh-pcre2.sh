#!/bin/sh
# Re-vendor the PCRE2 source tree for vyto/regex.
#
# UNLIKE every other native dependency in this tree, PCRE2 *is* committed to git.
# vyto/gfx, vyto/intl and vyto/net all fetch or build their dependency out of
# band, because they are large C++ or need a build-system decision. PCRE2 is
# small pure C with no build system of its own, and regex is not an optional
# module: a fetched dependency makes the test suite silently skip on a fresh
# clone (tests/run_tests.sh:505,518), which is acceptable for a canvas backend
# and not for pattern matching. See ../README.md, "Why PCRE2 is vendored".
#
# So this script is NOT part of the build. Nothing calls it. It exists to move
# the vendored tree to a new upstream release reproducibly, and to prove after
# the fact that the tree in git is byte-identical to that release.
#
# It populates:
#   native/src/pcre2/            the trimmed upstream tree, byte-for-byte
#   native/src/pcre2.h           generated from upstream src/pcre2.h.in
#   native/src/vregex_*.c        one wrapper TU per compiled upstream source
#   native/pcre2.version         the release tag
#   native/pcre2.sha256          sha256 of the release tarball
#   native/pcre2.files.sha256    sha256 of every vendored file
#
# It does NOT touch native/src/config.h or native/src/regex_shim.c — those are
# first-party and hand-maintained.
#
# Usage:
#   refresh-pcre2.sh              # re-vendor the version in native/pcre2.version
#   refresh-pcre2.sh 10.46        # move to a new release
#   refresh-pcre2.sh --verify     # check the working tree against the manifest
#   refresh-pcre2.sh --dry-run    # fetch and report, change nothing
#
# Requires: curl, tar, sha256sum (or shasum), sed.
#
# After a version bump, before committing:
#   1. Build upstream untrimmed and run its own suite — `./configure
#      --enable-jit --enable-unicode && make && make check`. The vendored tree
#      drops testdata/ and can never self-test again, so this is the only place
#      PCRE2's own conformance tests ever run.
#   2. Check this script's TU-drift report is empty (it fails loudly otherwise).
#   3. make clean-cache && ./tests/run_tests.sh
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/src"
VEND="$SRC/pcre2"
MODE="refresh"
VERSION=""

for a in "$@"; do
    case "$a" in
        --verify)  MODE="verify" ;;
        --dry-run) MODE="dry-run" ;;
        -*)        echo "unknown option: $a" >&2; exit 2 ;;
        *)         VERSION="$a" ;;
    esac
done

[ -n "$VERSION" ] || VERSION="$(cat "$HERE/pcre2.version" 2>/dev/null || echo 10.45)"

# sha256sum is coreutils; macOS has shasum -a 256.
if command -v sha256sum >/dev/null 2>&1; then
    SHA() { sha256sum "$@"; }
    SHACHECK() { sha256sum -c --quiet "$@"; }
elif command -v shasum >/dev/null 2>&1; then
    SHA() { shasum -a 256 "$@"; }
    SHACHECK() { shasum -a 256 -c --status "$@"; }
else
    echo "[vyto/regex] need sha256sum or shasum" >&2; exit 1
fi

# ---------------------------------------------------------------- verify mode
if [ "$MODE" = "verify" ]; then
    [ -f "$HERE/pcre2.files.sha256" ] || {
        echo "[vyto/regex] no manifest at native/pcre2.files.sha256" >&2; exit 1; }
    cd "$HERE"
    if SHACHECK pcre2.files.sha256; then
        n=$(grep -c . pcre2.files.sha256)
        echo "[vyto/regex] $n vendored files match PCRE2 $VERSION exactly"
    else
        echo "[vyto/regex] VENDORED TREE MODIFIED — see the failures above." >&2
        echo "             The tree under native/src/pcre2/ must stay byte-identical" >&2
        echo "             to the upstream release. Put local changes in the shim or" >&2
        echo "             the wrapper TUs instead." >&2
        exit 1
    fi
    exit 0
fi

# ------------------------------------------------------------------- download
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

TARBALL="pcre2-$VERSION.tar.bz2"
URL="https://github.com/PCRE2Project/pcre2/releases/download/pcre2-$VERSION/$TARBALL"
echo "[vyto/regex] downloading PCRE2 $VERSION…"
curl -fsSL "$URL" -o "$WORK/$TARBALL"

GOT="$(SHA "$WORK/$TARBALL" | cut -d' ' -f1)"
if [ -f "$HERE/pcre2.sha256" ]; then
    WANT="$(cut -d' ' -f1 < "$HERE/pcre2.sha256")"
    if [ "$GOT" != "$WANT" ]; then
        if [ "$VERSION" = "$(cat "$HERE/pcre2.version" 2>/dev/null)" ]; then
            echo "[vyto/regex] TARBALL HASH MISMATCH for the pinned version" >&2
            echo "             want $WANT" >&2
            echo "             got  $GOT" >&2
            exit 1
        fi
        echo "[vyto/regex] new version, recording tarball hash $GOT"
    fi
fi

tar -xjf "$WORK/$TARBALL" -C "$WORK"
UP="$WORK/pcre2-$VERSION"
[ -d "$UP/src" ] || { echo "[vyto/regex] unexpected tarball layout" >&2; exit 1; }

# ------------------------------------------------- derive the compiled TU list
# Never hand-maintain this. Upstream adds and removes translation units between
# releases (pcre2_chkdint.c arrived in 10.43, pcre2_compile_class.c in 10.45); a
# new source with no wrapper is a link error, and a stale wrapper is a duplicate
# symbol. Take libpcre2_8_la_SOURCES (via COMMON_SOURCES) plus NODIST_SOURCES,
# then subtract anything a file we keep pulls in with #include "pcre2_*.c" —
# those are not translation units and compiling them standalone breaks the link.
{ sed -n '/^COMMON_SOURCES/,/^$/p' "$UP/Makefile.am"
  sed -n '/^NODIST_SOURCES/p'      "$UP/Makefile.am"; } \
  | grep -o 'src/pcre2[A-Za-z0-9_]*\.c' | sed 's|src/||' | sort -u > "$WORK/tus.all"

: > "$WORK/inc"
while read -r f; do
    [ -f "$UP/src/$f" ] || continue
    grep -o '#include "pcre2[A-Za-z0-9_]*\.c"' "$UP/src/$f" 2>/dev/null \
      | sed 's/.*"\(.*\)"/\1/' >> "$WORK/inc" || true
done < "$WORK/tus.all"
sort -u -o "$WORK/inc" "$WORK/inc"
comm -23 "$WORK/tus.all" "$WORK/inc" > "$WORK/tus"

NTU=$(grep -c . "$WORK/tus")
echo "[vyto/regex] $NTU translation units, $(grep -c . "$WORK/inc") include-only"

# TU drift against the wrappers currently committed
if ls "$SRC"/vregex_*.c >/dev/null 2>&1; then
    for w in "$SRC"/vregex_*.c; do
        basename "$w" | sed 's/^vregex_/pcre2_/'
    done | sort -u > "$WORK/tus.have"
    if ! diff -q "$WORK/tus" "$WORK/tus.have" >/dev/null; then
        echo "[vyto/regex] TU LIST CHANGED since the last vendoring:"
        diff "$WORK/tus.have" "$WORK/tus" | sed 's/^/             /' || true
        [ "$MODE" = "dry-run" ] && exit 1
        echo "[vyto/regex] wrappers will be regenerated to match."
    fi
fi

if [ "$MODE" = "dry-run" ]; then
    echo "[vyto/regex] dry run: nothing written."
    exit 0
fi

# --------------------------------------------------------- lay down the files
rm -rf "$VEND"
mkdir -p "$VEND/src" "$VEND/deps/sljit"

cp "$UP/src/"*.h "$VEND/src/"
for f in $(cat "$WORK/tus") $(cat "$WORK/inc"); do
    [ -f "$UP/src/$f" ] && cp "$UP/src/$f" "$VEND/src/"
done

# Upstream's Makefile.am copies the .dist tables when the build is not
# configured with --enable-rebuild-chartables. Do the same. Never run
# pcre2_dftables: it bakes the vendoring machine's C locale into the tree.
cp "$UP/src/pcre2_chartables.c.dist" "$VEND/src/pcre2_chartables.c"

cp -R "$UP/deps/sljit/sljit_src" "$VEND/deps/sljit/"
for l in LICENCE LICENCE.md COPYING; do
    [ -f "$UP/$l" ] && cp "$UP/$l" "$VEND/$l"
done
cp "$VEND/LICENCE.md" "$HERE/../LICENSE-PCRE2.txt" 2>/dev/null || \
cp "$VEND/LICENCE"    "$HERE/../LICENSE-PCRE2.txt"

# The blanket *.o/*.a/*.so/*.dll rules in .gitignore would silently drop any
# prebuilt artefact hiding in the tarball, leaving an incomplete tree in git.
if find "$VEND" \( -name '*.o' -o -name '*.a' -o -name '*.so' -o -name '*.dll' \
                -o -name '*.exe' \) | grep -q .; then
    echo "[vyto/regex] binary artefacts in the vendored tree — .gitignore would" >&2
    echo "             drop these and the committed tree would not build." >&2
    find "$VEND" \( -name '*.o' -o -name '*.a' -o -name '*.so' -o -name '*.dll' \
                 -o -name '*.exe' \) >&2
    exit 1
fi

# ------------------------------------------------------- generate src/pcre2.h
# Kept outside native/src/pcre2/ so that tree stays provably pristine.
# pcre2_internal.h does #include "pcre2.h", which misses pcre2/src/ and falls
# through to -Inative/src, landing on this generated copy.
MAJ=$(sed -n 's/^m4_define(pcre2_major, *\[\(.*\)\]).*/\1/p'      "$UP/configure.ac")
MIN=$(sed -n 's/^m4_define(pcre2_minor, *\[\(.*\)\]).*/\1/p'      "$UP/configure.ac")
PRE=$(sed -n 's/^m4_define(pcre2_prerelease, *\[\(.*\)\]).*/\1/p' "$UP/configure.ac")
DATE=$(sed -n 's/^m4_define(pcre2_date, *\[\(.*\)\]).*/\1/p'      "$UP/configure.ac")
[ -n "$MAJ" ] && [ -n "$MIN" ] || {
    echo "[vyto/regex] could not read the version out of configure.ac" >&2; exit 1; }

sed -e "s/@PCRE2_MAJOR@/$MAJ/g" -e "s/@PCRE2_MINOR@/$MIN/g" \
    -e "s/@PCRE2_PRERELEASE@/$PRE/g" -e "s/@PCRE2_DATE@/$DATE/g" \
    "$UP/src/pcre2.h.in" > "$SRC/pcre2.h"

# A future release may introduce a fifth substitution. Leaving it unexpanded
# would be a syntax error at best and a wrong PCRE2_MAJOR at worst.
if grep -o '@[A-Za-z0-9_]*@' "$SRC/pcre2.h" | sort -u | grep .; then
    echo "[vyto/regex] unsubstituted token(s) above in the generated pcre2.h." >&2
    echo "             Teach this script the new token before continuing." >&2
    exit 1
fi

# ----------------------------------------------------------- wrapper TUs
# vytoc compiles native/src/*.c flat and non-recursively (src/main.c:616-635)
# with a fixed command line, and the language has no #cflags pragma
# (src/parse.c:860-878 — #link is the only one). So the vendored tree lives one
# directory down where the glob cannot see it, and these wrappers carry the
# defines. One wrapper per upstream TU: preserving upstream's translation-unit
# boundaries is what keeps its many same-named file statics from colliding.
rm -f "$SRC"/vregex_*.c
while read -r f; do
    stem=$(echo "$f" | sed 's/^pcre2_//; s/\.c$//')
    cat > "$SRC/vregex_$stem.c" <<EOF
/* Wrapper TU for $f — generated by native/refresh-pcre2.sh, do not edit.
 *
 * vytoc compiles native/src/*.c flat with a fixed command line and has no
 * #cflags pragma, so PCRE2's two required defines are set here rather than on
 * the compiler invocation. Everything else comes from native/src/config.h.
 *
 * Do NOT merge these wrappers into one unity build: upstream has many
 * same-named file statics, and collapsing the translation units makes them
 * collide. */
#define PCRE2_CODE_UNIT_WIDTH 8
#define HAVE_CONFIG_H
#include "pcre2/src/$f"
EOF
done < "$WORK/tus"

# ------------------------------------------------------------------ manifests
echo "$VERSION" > "$HERE/pcre2.version"
(cd "$WORK" && SHA "$TARBALL") | sed "s|$TARBALL|pcre2-$VERSION.tar.bz2|" \
    > "$HERE/pcre2.sha256"

cd "$HERE"
find src/pcre2 src/pcre2.h -type f | LC_ALL=C sort > "$WORK/list"
: > pcre2.files.sha256
while read -r p; do SHA "$p" >> pcre2.files.sha256; done < "$WORK/list"

echo "[vyto/regex] vendored PCRE2 $VERSION"
echo "             $(grep -c . pcre2.files.sha256) files, $(du -sh src/pcre2 | cut -f1) on disk"
echo "             $NTU wrapper TUs in src/"
echo "[vyto/regex] now run: sh $0 --verify && make clean-cache && ./tests/run_tests.sh"
