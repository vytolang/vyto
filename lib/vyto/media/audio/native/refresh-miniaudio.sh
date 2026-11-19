#!/bin/sh
# Re-vendor miniaudio for vyto/media/audio.
#
# miniaudio IS committed to git, for the same reason stb, PCRE2 and micro-ecc
# are (see ../../image/native/refresh-stb.sh): a fetched dependency makes the
# test suite silently skip on a fresh clone, and a green run that proved nothing
# is worse than a red one.
#
# It is one header (~4 MB of source, ~125 KB of linked code once trimmed by
# audio_config.h), dual-licensed public domain / MIT-0, with no build system and
# no generated files. Every OS audio library it talks to (ALSA, PulseAudio,
# WASAPI, DirectSound, WinMM, AAudio, OpenSL) is loaded at run time with
# dlopen/LoadLibrary, so nothing new is linked and a machine without one of
# them simply falls through to the next backend.
#
# This script is NOT part of the build. Nothing calls it. It exists to move the
# vendored header to a new upstream release reproducibly, and to prove after the
# fact that what is in git is byte-identical to what upstream published.
#
# It populates:
#   native/src/miniaudio/miniaudio.h   the upstream header, byte-for-byte
#   native/src/miniaudio/LICENSE       upstream's licence text
#   native/miniaudio.version           the release tag
#   native/miniaudio.files.sha256      sha256 of every vendored file
#
# It does NOT touch native/src/audio_*.c or audio_*.h — those are first-party.
#
# Upstream's miniaudio.c is deliberately not fetched: it is two lines that
# define MINIAUDIO_IMPLEMENTATION, and native/src is compiled flat, so a stray .c
# in the wrong place would become a translation unit. audio_ma.c does that job.
#
# Usage:
#   sh refresh-miniaudio.sh 0.11.26      fetch that tag and rewrite the tree
#   sh refresh-miniaudio.sh --dry-run T  fetch and print, write nothing
#   sh refresh-miniaudio.sh --verify     check the tree against the manifest
set -eu

here=$(cd "$(dirname "$0")" && pwd)
src="$here/src/miniaudio"
verify=0
dryrun=0
ref=

for arg in "$@"; do
    case $arg in
        --verify)  verify=1 ;;
        --dry-run) dryrun=1 ;;
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

if [ "$verify" -eq 1 ]; then
    if [ ! -f "$here/miniaudio.files.sha256" ]; then
        echo "no manifest at $here/miniaudio.files.sha256" >&2
        exit 1
    fi
    (cd "$here/src" && sha256 -c "$here/miniaudio.files.sha256")
    echo "vendored tree matches $here/miniaudio.files.sha256: miniaudio $(cat "$here/miniaudio.version")"
    exit 0
fi

if [ -z "$ref" ]; then
    echo "usage: $0 <tag> | --dry-run <tag> | --verify" >&2
    exit 2
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

for f in miniaudio.h LICENSE; do
    url="https://raw.githubusercontent.com/mackron/miniaudio/$ref/$f"
    echo "fetching $url"
    curl -fsSL -o "$tmp/$f" "$url"
done

# The header states its own version; refuse a tag that disagrees with it, so
# miniaudio.version can never name a release the bytes are not from.
maj=$(sed -n 's/^#define MA_VERSION_MAJOR *\([0-9]*\).*/\1/p' "$tmp/miniaudio.h")
min=$(sed -n 's/^#define MA_VERSION_MINOR *\([0-9]*\).*/\1/p' "$tmp/miniaudio.h")
rev=$(sed -n 's/^#define MA_VERSION_REVISION *\([0-9]*\).*/\1/p' "$tmp/miniaudio.h")
version="$maj.$min.$rev"
if [ "$version" != "$ref" ]; then
    echo "tag $ref but the header says $version" >&2
    exit 1
fi
echo "  miniaudio.h $version $(sha256 "$tmp/miniaudio.h" | awk '{print $1}')"

if [ "$dryrun" -eq 1 ]; then
    echo "--dry-run: nothing written"
    exit 0
fi

mkdir -p "$src"
cp "$tmp/miniaudio.h" "$tmp/LICENSE" "$src/"
echo "$version" > "$here/miniaudio.version"
(cd "$here/src" && sha256 miniaudio/miniaudio.h miniaudio/LICENSE) > "$here/miniaudio.files.sha256"

echo "vendored miniaudio $version into $src"
echo "now run, before committing:"
echo "  make clean-cache && sh tests/run_tests.sh   (the audio_* cases)"
echo "  ./vytoc build tests/fixtures/audio_freestanding.vt --freestanding"
