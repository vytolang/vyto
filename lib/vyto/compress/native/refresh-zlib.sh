#!/bin/sh
# Re-vendor zlib, or verify the committed tree is untouched.
#
#   sh refresh-zlib.sh              re-fetch the pinned version
#   sh refresh-zlib.sh 1.3.2        fetch a specific version and re-pin
#   sh refresh-zlib.sh --verify     check the vendored tree is unmodified
#   sh refresh-zlib.sh --dry-run    say what would happen, change nothing
#
# NOT part of the build. Nothing calls it during a compile; it exists so that
# moving to a new zlib is a recorded, checkable operation rather than a pile of
# copied files. tests/run_tests.sh does invoke --verify.
#
# Why vendored at all, when vyto/gfx and vyto/intl fetch their dependencies out
# of band: a fetched dependency makes the test suite SKIP silently on a fresh
# clone, and compression is not optional — it is the substrate for reading real
# file formats. The same argument lib/vyto/regex makes for PCRE2 and
# lib/vyto/db/sqlite makes for the amalgamation. zlib is the easiest of the
# three to vendor: pure C, no build system worth running, ~1.1 MB.
#
# LOCAL CHANGES DO NOT GO IN src/zlib/. Put them in zlib_shim.c (our entry
# points) or zlib_config.h (the build options), both first-party and both
# untouched by a refresh.

set -e
cd "$(dirname "$0")"

VERSION=$(cat zlib.version 2>/dev/null || echo "1.3.1")
MODE="refresh"

case "${1:-}" in
    --verify)  MODE="verify" ;;
    --dry-run) MODE="dry" ;;
    "")        ;;
    -*)        echo "unknown option: $1" >&2; exit 2 ;;
    *)         VERSION="$1" ;;
esac

# The exact set of upstream sources this package compiles. zlib's four gz*.c
# are deliberately absent: they are the gzopen/gzread stdio file API, which
# Z_SOLO drops and this package does not use — it works on byte buffers. That
# also keeps libc file I/O out of the dependency surface.
#
# Adding a source here means adding a matching vzlib_*.c wrapper, or it is never
# compiled. This script generates them, so the two cannot drift.
SOURCES="adler32 compress crc32 deflate infback inffast inflate inftrees trees uncompr zutil"
HEADERS="crc32.h deflate.h inffast.h inffixed.h inflate.h inftrees.h trees.h zconf.h zlib.h zutil.h"

# ---- verify -----------------------------------------------------------------
# The vendored tree must be byte-identical to what upstream shipped. A local
# edit inside src/zlib/ would be silently reverted by the next refresh, so it is
# an error rather than a warning.
if [ "$MODE" = "verify" ]; then
    if [ ! -f zlib.files.sha256 ]; then
        echo "no zlib.files.sha256 — nothing to verify against" >&2
        exit 1
    fi
    cd src
    if sha256sum -c --quiet ../zlib.files.sha256; then
        echo "vendored zlib $VERSION verified"
        exit 0
    fi
    cat >&2 <<'EOF'

VENDORED TREE MODIFIED — the files under native/src/zlib/ no longer match what
upstream shipped. The next refresh will overwrite them.

Local changes belong in zlib_shim.c (our entry points) or zlib_config.h (the
build options), never in zlib itself.
EOF
    exit 1
fi

URL="https://github.com/madler/zlib/releases/download/v$VERSION/zlib-$VERSION.tar.gz"

if [ "$MODE" = "dry" ]; then
    echo "would fetch : $URL"
    echo "would vendor: src/zlib/{$(echo $SOURCES | tr ' ' ',')}.c + headers"
    echo "would keep  : src/{zlib_shim.c,zlib_shim.h,zlib_config.h}"
    exit 0
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "fetching $URL"
curl -sSfL -o "$TMP/zlib.tar.gz" "$URL"

if [ -f zlib.sha256 ]; then
    WANT=$(cat zlib.sha256)
    GOT=$(sha256sum "$TMP/zlib.tar.gz" | cut -d' ' -f1)
    if [ "$WANT" != "$GOT" ]; then
        echo "SHA-256 MISMATCH" >&2
        echo "  want $WANT" >&2
        echo "  got  $GOT" >&2
        echo "Refusing to vendor. If you changed the version, update zlib.sha256" >&2
        echo "from the hash published with the release." >&2
        exit 1
    fi
    echo "SHA-256 ok"
else
    echo "warning: no zlib.sha256 to check against" >&2
fi

tar xzf "$TMP/zlib.tar.gz" -C "$TMP"
SRCDIR="$TMP/zlib-$VERSION"
[ -d "$SRCDIR" ] || { echo "no zlib-$VERSION directory in the tarball" >&2; exit 1; }

# A compiled artefact in the tarball would be silently dropped by .gitignore's
# blanket rules, leaving an incomplete tree in git that still builds locally.
# Refuse rather than commit something unreproducible.
if find "$SRCDIR" -name '*.o' -o -name '*.a' -o -name '*.so' -o -name '*.dll' | grep -q .; then
    echo "tarball contains compiled artefacts — refusing to vendor" >&2
    exit 1
fi

rm -rf src/zlib
mkdir -p src/zlib
for f in $SOURCES; do cp "$SRCDIR/$f.c" "src/zlib/$f.c"; done
for h in $HEADERS; do cp "$SRCDIR/$h" "src/zlib/$h"; done
cp "$SRCDIR/LICENSE" ../LICENSE-ZLIB.txt

# Generate one wrapper TU per upstream source. vytoc globs native/src/*.c FLAT
# (src/main.c:1030-1046), so src/zlib/ is invisible to the build and each source
# needs a wrapper at the top level to be compiled at all. Generated rather than
# hand-maintained so a source added above cannot be forgotten.
for f in $SOURCES; do
    cat > "src/vzlib_$f.c" <<WRAP
/* Wrapper TU for zlib's $f.c — see ../../README.md, "Why zlib is vendored".
 *
 * GENERATED by native/refresh-zlib.sh. Do not edit; edit that script.
 *
 * vytoc globs native/src/*.c FLAT and non-recursively (src/main.c:1030-1046),
 * so native/src/zlib/ is invisible to the build. Each upstream source needs one
 * of these to be compiled at all, and the only include path is -Inative/src
 * (src/main.c:1043), which is what makes the relative include below resolve.
 *
 * One wrapper per upstream TU, never a unity build: zlib has same-named file
 * statics that collide when translation units are merged.
 *
 * The build options come from zlib_config.h, which zlib_shim.c includes too —
 * Z_SOLO changes which prototypes zlib.h declares, so the shim and the library
 * must be compiled with the same view of it.
 *
 * --freestanding splices -DVT_NO_LIBC into every package shim's compile line
 * (src/main.c:878-879). zlib needs an allocator, so there is nothing to degrade
 * to: the whole library drops out and zlib_shim.c's stub arm answers instead.
 * Same arrangement as lib/vyto/db/sqlite/native/src/vsqlite3.c:16-23.
 */
#ifndef VT_NO_LIBC
#include "zlib_config.h"
#include "zlib/$f.c"
#endif
WRAP
done

echo "$VERSION" > zlib.version
( cd src && find zlib -type f | sort | xargs sha256sum > ../zlib.files.sha256 )

echo "vendored zlib $VERSION"
echo "  $(find src/zlib -type f | wc -l) files, $(du -sh src/zlib | cut -f1)"
echo "  $(echo $SOURCES | wc -w) wrapper TUs"
echo
echo "Now run:  make clean-cache && make test && make test-dev"
