#!/bin/sh
# Re-vendor the micro-ecc source tree for vyto/crypto/ecc.
#
# micro-ecc IS committed to git, for the same reason PCRE2 is (see
# ../../../regex/native/refresh-pcre2.sh): a fetched dependency makes the test
# suite silently skip on a fresh clone, which is tolerable for a canvas backend
# and not for cryptography. A crypto module whose tests quietly do not run is
# worse than one that is missing — the second failure is visible. micro-ecc is
# also small, pure C, and has no build system of its own, so there is nothing
# to decide at provision time the way libcurl's TLS stack has to be decided.
#
# This script is NOT part of the build. Nothing calls it. It exists to move the
# vendored tree to a new upstream release reproducibly, and to prove after the
# fact that the tree in git is byte-identical to that release.
#
# It populates:
#   native/src/microecc/          the trimmed upstream tree, byte-for-byte
#   native/microecc.version       the release tag
#   native/microecc.sha256        sha256 of the release tarball
#   native/microecc.files.sha256  sha256 of every vendored file
#
# It does NOT touch native/src/uecc_config.h, native/src/vecc_uecc.c or
# native/src/ecc_shim.c — those are first-party and hand-maintained.
#
# What is trimmed: everything upstream does not compile from. The vendored set
# is every .c/.h/.inc plus LICENSE.txt; dropped are emk_*.py (upstream's build
# system), library.properties, README.md, .gitignore, and the examples/, test/
# and scripts/ directories. The asm_arm*/asm_avr* includes are kept even though
# no triple vytoc targets can reach them (uECC.c:182,186 gate them on 32-bit
# ARM and AVR, and every vytoc triple is 64-bit) — they are upstream sources,
# and a trim rule of "what upstream compiles" needs no per-release judgement
# call, where "what we happen to reach today" would.
#
# Usage:
#   refresh-microecc.sh              # re-vendor the version in microecc.version
#   refresh-microecc.sh v1.2         # move to a new release
#   refresh-microecc.sh --verify     # check the working tree against the manifest
#   refresh-microecc.sh --dry-run    # fetch and report, change nothing
#
# Requires: curl, tar, sha256sum (or shasum).
#
# After a version bump, before committing:
#   1. Build upstream untrimmed and run its own suite — micro-ecc ships test/
#      with NIST vectors, and the vendored tree drops it, so this is the only
#      place those ever run.
#   2. rm -rf ../../../../.vyto-cache */.vyto-cache   (a lib-only edit does not
#      invalidate a per-directory cache; stale objects will hide the change)
#   3. ./tests/run_tests.sh   — 86_crypto holds the shim to its known answers.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/src"
DEST="$SRC/microecc"
REPO="https://github.com/kmackay/micro-ecc"

# Every upstream file we compile from, plus the licence. Keep sorted.
FILES="LICENSE.txt
asm_arm.inc
asm_arm_mult_square.inc
asm_arm_mult_square_umaal.inc
asm_avr.inc
asm_avr_mult_square.inc
curve-specific.inc
platform-specific.inc
types.h
uECC.c
uECC.h
uECC_vli.h"

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$@"
    else shasum -a 256 "$@"
    fi
}

VERIFY=0
DRY=0
VERSION="$(cat "$HERE/microecc.version" 2>/dev/null || echo v1.1)"
for a in "$@"; do
    case "$a" in
        --verify)  VERIFY=1 ;;
        --dry-run) DRY=1 ;;
        -*) echo "unknown option: $a" >&2; exit 2 ;;
        *)  VERSION="$a" ;;
    esac
done

# --- --verify: the tree in git against the manifest in git -------------------
if [ "$VERIFY" -eq 1 ]; then
    if [ ! -f "$HERE/microecc.files.sha256" ]; then
        echo "[vyto/crypto/ecc] no microecc.files.sha256 to verify against" >&2
        exit 1
    fi
    cd "$SRC"
    if sha256 -c "$HERE/microecc.files.sha256" >/dev/null 2>&1; then
        echo "[vyto/crypto/ecc] vendored micro-ecc matches the manifest ($VERSION)"
    else
        echo "[vyto/crypto/ecc] VENDORED TREE HAS DRIFTED from $VERSION:" >&2
        sha256 -c "$HERE/microecc.files.sha256" 2>&1 | grep -v ': OK$' >&2
        exit 1
    fi
    # A file present in the tree but absent from the manifest is drift the
    # checksum pass above cannot see.
    for f in $(ls "$DEST"); do
        grep -q "  microecc/$f\$" "$HERE/microecc.files.sha256" || {
            echo "[vyto/crypto/ecc] untracked file in vendored tree: $f" >&2
            exit 1
        }
    done
    exit 0
fi

# --- fetch -------------------------------------------------------------------
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
TARBALL="$TMP/microecc.tar.gz"

echo "[vyto/crypto/ecc] fetching micro-ecc $VERSION…"
curl -sSL -o "$TARBALL" "$REPO/archive/refs/tags/$VERSION.tar.gz"
TARSUM="$(sha256 "$TARBALL" | cut -d' ' -f1)"
echo "[vyto/crypto/ecc] tarball sha256 $TARSUM"

tar xzf "$TARBALL" -C "$TMP"
UP="$(ls -d "$TMP"/micro-ecc-* | head -n1)"
[ -d "$UP" ] || { echo "unexpected tarball layout" >&2; exit 1; }

for f in $FILES; do
    [ -f "$UP/$f" ] || { echo "upstream $VERSION is missing $f — trim list needs updating" >&2; exit 1; }
done

if [ "$DRY" -eq 1 ]; then
    echo "[vyto/crypto/ecc] --dry-run: would vendor $(echo "$FILES" | wc -l | tr -d ' ') files from $VERSION"
    echo "$FILES" | sed 's/^/  /'
    exit 0
fi

# --- vendor ------------------------------------------------------------------
rm -rf "$DEST"
mkdir -p "$DEST"
for f in $FILES; do cp "$UP/$f" "$DEST/$f"; done

echo "$VERSION" > "$HERE/microecc.version"
echo "$TARSUM  micro-ecc-${VERSION#v}.tar.gz" > "$HERE/microecc.sha256"
(cd "$SRC" && sha256 microecc/* | sort -k2 > "$HERE/microecc.files.sha256")

echo "[vyto/crypto/ecc] vendored $VERSION into src/microecc"
echo "[vyto/crypto/ecc] now run upstream's own test suite, then ./tests/run_tests.sh"
