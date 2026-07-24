#!/bin/sh
# Provision the libcurl dependency for vyto/net on a cross target.
#
# vyto/net links the *system* libcurl on Linux and macOS (#link "curl" in
# net.vt), so nothing here is needed for those. Windows has no system libcurl,
# so the package carries a prebuilt one and this script sets up the half that
# can be generated:
#   native/<triple>/include/curl/*.h   (headers — vytoc adds -I for this triple)
#
# The DLL is NOT downloaded. A libcurl build is a TLS-stack decision, not a
# fetch, so `native/<triple>/libcurl*.dll` is supplied out of band and shipped
# with a Vyto release. Point this script at one with --check to validate it.
#
# Usage:
#   provision-curl.sh                 # headers for windows-x64
#   provision-curl.sh --check         # validate the DLL that is already there
#   CURL_VER=8.5.0 provision-curl.sh  # pin a different header version
#
# Requires: curl, tar.
set -e

TRIPLE="windows-x64"
CHECK_ONLY=0
# Headers older than the DLL are the supported direction: libcurl keeps ABI
# compatibility forward, so a program built against these runs against any later
# libcurl. 7.81 is the floor that still has curl_multi_poll (7.66+), which
# http_shim.c uses.
CURL_VER="${CURL_VER:-7.81.0}"

for a in "$@"; do
    case "$a" in
        --check) CHECK_ONLY=1 ;;
        -*) echo "unknown option: $a" >&2; exit 2 ;;
        *) TRIPLE="$a" ;;
    esac
done

HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="$HERE/$TRIPLE"

# Validate whatever DLL is present. Both of these link cleanly and only fail
# once the program starts on Windows, where there is no compiler to explain it.
check_dll() {
    dll="$(ls "$DEST"/libcurl*.dll 2>/dev/null | head -n1 || true)"
    if [ -z "$dll" ]; then
        echo "[vyto/net] no libcurl DLL in $DEST"
        echo "[vyto/net]   supply one there, or take it from a Vyto release."
        return 0
    fi
    echo "[vyto/net] checking $(basename "$dll")…"
    OBJDUMP=x86_64-w64-mingw32-objdump
    command -v "$OBJDUMP" >/dev/null 2>&1 || {
        echo "[vyto/net]   ($OBJDUMP not installed — skipping)"; return 0; }
    deps="$("$OBJDUMP" -p "$dll" | sed -n 's/^\s*DLL Name: //p')"

    # The UCRT ships with Windows 10 but NOT Windows 7, where it needs
    # KB2999226. Windows 7 is this port's verified floor.
    if echo "$deps" | grep -qi 'api-ms-win-crt'; then
        echo "[vyto/net]   !! imports the UCRT (api-ms-win-crt-*)."
        echo "[vyto/net]      Fails to load on stock Windows 7 without KB2999226."
        echo "[vyto/net]      A legacy-msvcrt build avoids the prerequisite."
    fi
    if echo "$deps" | grep -qiE 'libwinpthread|libgcc_s|libstdc\+\+'; then
        echo "[vyto/net]   !! needs mingw runtime DLLs beside it:"
        echo "$deps" | grep -iE 'libwinpthread|libgcc_s|libstdc\+\+' | sed 's/^/[vyto\/net]      /'
    fi
    # Schannel uses the OS trust store, so a shipped CA bundle is dead weight
    # that also goes stale.
    if echo "$deps" | grep -qiE 'Secur32|CRYPT32'; then
        echo "[vyto/net]   TLS: Schannel (OS trust store — no CA bundle needed)."
        [ -f "$DEST/curl-ca-bundle.crt" ] &&
            echo "[vyto/net]   !! curl-ca-bundle.crt is present but unused; drop it."
    else
        echo "[vyto/net]   TLS: not Schannel — check whether it needs a CA bundle"
        echo "[vyto/net]      and its own crypto DLLs shipped alongside."
    fi
    echo "[vyto/net] done checking."
}

if [ "$CHECK_ONLY" = 1 ]; then
    check_dll
    exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "[vyto/net] fetching curl $CURL_VER headers…"
URL="https://curl.se/download/curl-${CURL_VER}.tar.gz"
curl -fsSL "$URL" -o "$WORK/curl.tgz"
tar -xzf "$WORK/curl.tgz" -C "$WORK"

src="$WORK/curl-${CURL_VER}/include/curl"
[ -d "$src" ] || { echo "no include/curl in the tarball — layout changed" >&2; exit 1; }

echo "[vyto/net] installing headers for $TRIPLE…"
# Per-triple, never native/src: that directory is shared by every target, so
# headers left there would shadow the system libcurl on Linux and macOS too.
rm -rf "$DEST/include/curl"
mkdir -p "$DEST/include/curl"
cp "$src"/*.h "$DEST/include/curl/"

echo "[vyto/net] installed $(ls "$DEST/include/curl" | wc -l) headers in $DEST/include/curl"
check_dll
