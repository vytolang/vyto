#!/bin/sh
# Re-vendor the SQLite amalgamation, or verify the committed tree is untouched.
#
#   sh refresh-sqlite.sh                 re-fetch the pinned version
#   sh refresh-sqlite.sh 3.54.0          fetch a specific version and re-pin
#   sh refresh-sqlite.sh --verify        check the vendored tree is unmodified
#   sh refresh-sqlite.sh --dry-run       say what would happen, change nothing
#
# NOT part of the build. Nothing calls this; it exists so that updating SQLite
# is a recorded, checkable operation rather than a pile of copied files.
#
# Why vendored at all, when vyto/gfx and vyto/intl fetch their dependencies out
# of band: a fetched dependency makes the test suite SKIP silently on a fresh
# clone, and a database whose tests quietly did not run is worse than one whose
# tests failed. The amalgamation is a single self-contained C file with no build
# system, which is exactly the case vendoring is for — the same argument
# lib/vyto/crypto makes for micro-ecc.
#
# LOCAL CHANGES DO NOT GO IN sqlite3/. Put them in db_shim.c or
# sqlite_config.h, both of which are first-party and survive a refresh.

set -e

cd "$(dirname "$0")"

VERSION=$(cat sqlite.version 2>/dev/null || echo "3.53.4")
URLPATH=$(cat sqlite.url 2>/dev/null || echo "")
MODE="refresh"

case "${1:-}" in
    --verify)  MODE="verify" ;;
    --dry-run) MODE="dry" ;;
    "")        ;;
    -*)        echo "unknown option: $1" >&2; exit 2 ;;
    *)         VERSION="$1"; URLPATH="" ;;
esac

# ---- verify -----------------------------------------------------------------
# The vendored tree must be byte-identical to what upstream shipped. A local
# edit inside sqlite3/ would be silently reverted by the next refresh, so it is
# an error rather than a warning.
if [ "$MODE" = "verify" ]; then
    if [ ! -f sqlite.files.sha256 ]; then
        echo "no sqlite.files.sha256 — nothing to verify against" >&2
        exit 1
    fi
    cd src
    if sha256sum -c --quiet ../sqlite.files.sha256; then
        echo "vendored SQLite $VERSION verified"
        exit 0
    fi
    cat >&2 <<'EOF'

VENDORED TREE MODIFIED — the files under native/src/sqlite3/ no longer match
what upstream shipped. The next refresh will overwrite them.

Local changes belong in db_shim.c (our entry points) or sqlite_config.h (the
build options), never in the amalgamation itself.
EOF
    exit 1
fi

# ---- work out the URL -------------------------------------------------------
# sqlite.org encodes the version into the filename as MMmmppNN — 3.53.4 becomes
# 3530400 — and files it under the release year, which is not derivable from the
# version. So the path is pinned in sqlite.url and only recomputed when the
# caller names a new version, in which case the year has to be supplied too.
if [ -z "$URLPATH" ]; then
    MAJ=$(echo "$VERSION" | cut -d. -f1)
    MIN=$(echo "$VERSION" | cut -d. -f2)
    PAT=$(echo "$VERSION" | cut -d. -f3)
    [ -n "$PAT" ] || PAT=0
    NUM=$(printf '%d%02d%02d00' "$MAJ" "$MIN" "$PAT")
    YEAR=${SQLITE_YEAR:-$(date +%Y)}
    URLPATH="$YEAR/sqlite-amalgamation-$NUM.zip"
    echo "note: guessing URL path $URLPATH"
    echo "      set SQLITE_YEAR=<release year> if that 404s"
fi
URL="https://sqlite.org/$URLPATH"

if [ "$MODE" = "dry" ]; then
    echo "would fetch : $URL"
    echo "would vendor: src/sqlite3/{sqlite3.c,sqlite3.h}"
    echo "would keep  : src/{db_shim.c,sqlite_config.h,vsqlite3.c}"
    exit 0
fi

# ---- fetch ------------------------------------------------------------------
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "fetching $URL"
curl -sSf -o "$TMP/sqlite.zip" "$URL"

# sqlite.org publishes SHA3-256, not SHA-256, on its download page. Checking the
# wrong algorithm produces a mismatch on a perfectly good file, which is an
# afternoon nobody needs twice.
if [ -f sqlite.sha3-256 ]; then
    WANT=$(cat sqlite.sha3-256)
    GOT=$(openssl dgst -sha3-256 -r "$TMP/sqlite.zip" | cut -d' ' -f1)
    if [ "$WANT" != "$GOT" ]; then
        echo "SHA3-256 MISMATCH" >&2
        echo "  want $WANT" >&2
        echo "  got  $GOT" >&2
        echo "Refusing to vendor. If you changed the version, update sqlite.sha3-256" >&2
        echo "from the hash printed on https://sqlite.org/download.html" >&2
        exit 1
    fi
    echo "SHA3-256 ok"
else
    echo "warning: no sqlite.sha3-256 to check against" >&2
fi

unzip -q -o "$TMP/sqlite.zip" -d "$TMP"
SRCDIR=$(find "$TMP" -maxdepth 1 -type d -name 'sqlite-amalgamation-*' | head -1)
[ -n "$SRCDIR" ] || { echo "no sqlite-amalgamation-* directory in the zip" >&2; exit 1; }

# Only these two. shell.c is the CLI and sqlite3ext.h is for loadable
# extensions, which SQLITE_OMIT_LOAD_EXTENSION rules out — vendoring either
# would be ~1.2 MB of code that never compiles.
mkdir -p src/sqlite3
cp "$SRCDIR/sqlite3.c" src/sqlite3/sqlite3.c
cp "$SRCDIR/sqlite3.h" src/sqlite3/sqlite3.h

echo "$VERSION" > sqlite.version
echo "$URLPATH" > sqlite.url
( cd src && sha256sum sqlite3/sqlite3.c sqlite3/sqlite3.h > ../sqlite.files.sha256 )

echo "vendored SQLite $VERSION"
echo "  src/sqlite3/sqlite3.c  $(wc -c < src/sqlite3/sqlite3.c) bytes"
echo "  src/sqlite3/sqlite3.h  $(wc -c < src/sqlite3/sqlite3.h) bytes"
echo
echo "Now run:  make clean-cache && ./vytoc run examples/103_db.vt"
