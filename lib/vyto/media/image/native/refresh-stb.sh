#!/bin/sh
# Re-vendor stb_image_write.h for vyto/media/image.
#
# stb_image_write IS committed to git, for the same reason PCRE2 and micro-ecc
# are (see ../../../regex/native/refresh-pcre2.sh and
# ../../../crypto/ecc/native/refresh-microecc.sh): a fetched dependency makes
# the test suite silently skip on a fresh clone. That is tolerable for a canvas
# backend and not for the only way a Vyto program can write an image file — a
# green run that proved nothing is worse than a red one.
#
# It is also the cheapest possible thing to vendor: ONE public-domain header,
# 71 KB, no build system, no configure, no generated files, no C++. That is the
# same profile that justified the PCRE2 exception, at a twentieth of the size.
#
# This script is NOT part of the build. Nothing calls it. It exists to move the
# vendored header to a new upstream release reproducibly, and to prove after the
# fact that what is in git is byte-identical to what upstream published.
#
# It populates:
#   native/src/stb/stb_image_write.h   the upstream header, byte-for-byte
#   native/stb.version                 the version string from the header
#   native/stb.sha256                  sha256 of the header as published
#   native/stb.files.sha256            sha256 of every vendored file
#
# stb.sha256 and stb.files.sha256 carry the same digest here, which is not a
# mistake: micro-ecc and PCRE2 ship release tarballs and the two hashes differ,
# but stb is distributed as loose headers with no archive to hash. The pair is
# kept anyway so the three vendored dependencies have one shape.
#
# It does NOT touch native/src/image_shim.c or image_shim.h — those are
# first-party and hand-maintained.
#
# Nothing is trimmed. Upstream ships one file and every byte of it compiles.
#
# Usage:
#   refresh-stb.sh              # re-vendor, tracking master
#   refresh-stb.sh <ref>        # a tag, branch or commit sha
#   refresh-stb.sh --verify     # check the working tree against the manifest
#   refresh-stb.sh --dry-run    # fetch and report, change nothing
#
# Requires: curl, sha256sum (or shasum).
#
# After a version bump, before committing:
#   1. make clean-cache && ./vytoc run examples/111_image.vt
#      The golden hashes encoded PNG bytes, so a change in stb's deflate or its
#      default filter WILL move them. That is a real signal — read the diff and
#      confirm the images still decode — not something to regenerate blindly.
#   2. ./vytoc build tests/fixtures/image_freestanding.vt --freestanding
#      The VT_NO_LIBC arm in image_shim.c must still cover every entry point.

set -e

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
src="$here/src/stb"
ref=master
dryrun=0
verify=0

for arg in "$@"; do
    case "$arg" in
        --verify)  verify=1 ;;
        --dry-run) dryrun=1 ;;
        -*)        echo "unknown option: $arg" >&2; exit 2 ;;
        *)         ref=$arg ;;
    esac
done

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$@"
    else
        shasum -a 256 "$@"
    fi
}

# --verify: the working tree against the committed manifest. This is the check
# that makes "vendored byte-for-byte" a fact rather than a claim.
if [ "$verify" -eq 1 ]; then
    if [ ! -f "$here/stb.files.sha256" ]; then
        echo "no manifest at $here/stb.files.sha256" >&2
        exit 1
    fi
    (cd "$here/src" && sha256 -c "$here/stb.files.sha256")
    echo "vendored tree matches $here/stb.files.sha256 (version $(cat "$here/stb.version"))"
    exit 0
fi

url="https://raw.githubusercontent.com/nothings/stb/$ref/stb_image_write.h"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

echo "fetching $url"
curl -fsSL -o "$tmp/stb_image_write.h" "$url"

# The version lives in the header's first line, e.g. "stb_image_write - v1.16".
version=$(sed -n '1s/.*- \(v[0-9][0-9.]*\) -.*/\1/p' "$tmp/stb_image_write.h")
if [ -z "$version" ]; then
    echo "could not read a version from the header's first line" >&2
    exit 1
fi
digest=$(sha256 "$tmp/stb_image_write.h" | awk '{print $1}')

echo "  version: $version"
echo "  sha256:  $digest"

if [ "$dryrun" -eq 1 ]; then
    echo "--dry-run: nothing written"
    exit 0
fi

mkdir -p "$src"
cp "$tmp/stb_image_write.h" "$src/stb_image_write.h"

printf '%s\n' "$version" > "$here/stb.version"
printf '%s\n' "$digest"   > "$here/stb.sha256"
(cd "$here/src" && sha256 stb/stb_image_write.h) > "$here/stb.files.sha256"

echo "vendored stb_image_write $version into $src"
echo "now run, before committing:"
echo "  make clean-cache && ./vytoc run examples/111_image.vt"
echo "  ./vytoc build tests/fixtures/image_freestanding.vt --freestanding"
