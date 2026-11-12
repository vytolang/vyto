#!/bin/sh
# Re-vendor libcurl + mbedTLS + the Mozilla CA bundle for windows-x64, or
# verify the committed tree is untouched.
#
#   sh refresh-curl.sh              re-fetch the pinned versions
#   sh refresh-curl.sh --verify     check the vendored tree is unmodified
#   sh refresh-curl.sh --dry-run    say what would happen, change nothing
#
# To move to a new release, edit curl.version / mbedtls.version / cacert.version
# AND the matching .sha256 file (from the hash the project publishes), then
# run with no arguments. A version bump without a new hash is refused.
#
# NOT part of the build. tests/run_tests.sh invokes --verify.
#
# Why vendored: Linux and macOS link the system libcurl (`#link "curl"`), but
# Windows has none. The prebuilt DLL this replaces was built against the
# Universal CRT, which Windows 7 lacks, so HTTPS did not work there at all, and
# being a DLL it could not be --bundle'd. Built from source against msvcrt with
# mbedTLS, the whole stack imports only kernel32, msvcrt, ws2_32 and bcrypt —
# all on Windows 7 through 11 — and does TLS 1.2/1.3 itself, independent of
# the OS's Schannel. The cost is that TLS security fixes arrive through this
# script, not Windows Update: mbedTLS 3.6 is LTS until at least March 2027.
#
# Everything lands under native/windows-x64/, which vytoc compiles ONLY for
# that triple (native/<triple>/src, src/main.c), so Linux builds see none of it.
#
# LOCAL CHANGES DO NOT GO IN src/curl/ OR src/mbedtls/. Build options live in
# the CURL_CMAKE_FLAGS list below (curl_config.h is generated from them) and in
# src/vyto_mbedtls_user.h; glue lives in src/vnet_*.c. All first-party.
#
# Needs, for a refresh only: curl, tar, sha256sum, cmake, ninja and the
# x86_64-w64-mingw32 toolchain (curl_config.h is produced by a real configure
# run against the cross compiler, never hand-written: curl's own
# config-win32.h is MSVC-only).

set -e
cd "$(dirname "$0")"
HERE=$(pwd)
W=windows-x64

CURL_VERSION=$(cat curl.version)
MBEDTLS_VERSION=$(cat mbedtls.version)
CACERT_DATE=$(cat cacert.version)
MODE="refresh"

case "${1:-}" in
    --verify)  MODE="verify" ;;
    --dry-run) MODE="dry" ;;
    "")        ;;
    *)         echo "unknown option: $1" >&2; exit 2 ;;
esac

# curl sources that cannot apply to this build (other TLS backends, QUIC, SSH,
# GSSAPI/SSPI, LDAP, c-ares, other OSes). vquic/vquic.c stays: it supplies
# the no-HTTP/3 answers (Curl_conn_may_http3) the HTTP code links against.
# Everything else in lib/ gets a
# wrapper; files whose feature is configured off compile to empty objects,
# which costs a few milliseconds and keeps this list short and obviously safe.
CURL_SKIP='^(vquic/(capsule|cf-capsule|cf-ngtcp2-cmn|cf-ngtcp2-proxy|cf-ngtcp2|cf-quiche|vquic-tls)\.c$|vssh/|vtls/(openssl|gtls|wolfssl|rustls|schannel|schannel_verify|apple)\.c$|amigaos\.c$|macos\.c$|ldap\.c$|openldap\.c$|curl_gssapi\.c$|socks_gssapi\.c$|socks_sspi\.c$|curl_sspi\.c$|vauth/.*(gssapi|sspi)\.c$|vauth/gsasl\.c$|vdns/asyn-ares\.c$)'

# The curl configure: HTTP and HTTPS only, static, mbedTLS, threaded resolver,
# IPv6, proxies kept (http_proxy from the environment works as on Linux).
# Cookies, HSTS and alt-svc are off because vyto/net/http keeps no state
# between requests; the rest is protocols and auth schemes nothing here speaks.
CURL_DISABLE="DICT FILE FTP GOPHER IMAP IPFS LDAP LDAPS MQTT POP3 RTSP SMTP TELNET TFTP SMB
  COOKIES ALTSVC HSTS NETRC DOH MIME FORM_API WEBSOCKETS PROGRESS_METER
  NTLM KERBEROS_AUTH NEGOTIATE_AUTH AWS SRP GETOPTIONS HEADERS_API LIBCURL_OPTION CA_SEARCH"
CURL_CMAKE_FLAGS="-DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF
  -DBUILD_TESTING=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF
  -DCURL_USE_PKGCONFIG=OFF -DCURL_USE_MBEDTLS=ON -DCURL_USE_SCHANNEL=OFF -DCURL_WINDOWS_SSPI=OFF
  -DUSE_NGHTTP2=OFF -DUSE_NGTCP2=OFF -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF
  -DCURL_USE_LIBSSH=OFF -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_LIBIDN2=OFF
  -DUSE_WIN32_IDN=OFF -DCURL_USE_GSSAPI=OFF -DUSE_LIBRTMP=OFF -DUSE_WIN32_LDAP=OFF
  -DENABLE_UNIX_SOCKETS=OFF -DENABLE_THREADED_RESOLVER=ON -DENABLE_IPV6=ON
  -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=none"
for p in $CURL_DISABLE; do CURL_CMAKE_FLAGS="$CURL_CMAKE_FLAGS -DCURL_DISABLE_$p=ON"; done

CURL_URL="https://curl.se/download/curl-$CURL_VERSION.tar.gz"
MBEDTLS_URL="https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBEDTLS_VERSION/mbedtls-$MBEDTLS_VERSION.tar.bz2"
CACERT_URL="https://curl.se/ca/cacert-$CACERT_DATE.pem"

# ---- verify -----------------------------------------------------------------
if [ "$MODE" = "verify" ]; then
    [ -f vendor.files.sha256 ] || { echo "no vendor.files.sha256 — nothing to verify against" >&2; exit 1; }
    cd "$W"
    if sha256sum -c --quiet ../vendor.files.sha256; then
        echo "vendored curl $CURL_VERSION + mbedTLS $MBEDTLS_VERSION + CA bundle $CACERT_DATE verified"
        exit 0
    fi
    cat >&2 <<'EOF'

VENDORED TREE MODIFIED — files under native/windows-x64/ no longer match what
refresh-curl.sh produced. The next refresh will overwrite them.

Build options belong in refresh-curl.sh (CURL_CMAKE_FLAGS) or
src/vyto_mbedtls_user.h; glue in src/vnet_*.c. Never in curl or mbedTLS.
EOF
    exit 1
fi

if [ "$MODE" = "dry" ]; then
    echo "would fetch : $CURL_URL"
    echo "              $MBEDTLS_URL"
    echo "              $CACERT_URL"
    echo "would write : $W/include/{curl,mbedtls,psa}, $W/src/{curl,mbedtls}/, $W/src/v{curl,mbed}_*.c,"
    echo "              $W/src/curl_config.h, $W/stage/cacert.pem"
    echo "would keep  : $W/src/{vyto_curl_build.h,vyto_mbedtls_build.h,vyto_mbedtls_user.h,vnet_*.c}"
    exit 0
fi

for t in curl tar sha256sum cmake ninja x86_64-w64-mingw32-gcc; do
    command -v "$t" >/dev/null 2>&1 || { echo "refresh needs $t" >&2; exit 1; }
done

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fetch() { # url file pinfile
    echo "fetching $1"
    curl -sSfL -o "$TMP/$2" "$1"
    want=$(cat "$3")
    got=$(sha256sum "$TMP/$2" | cut -d' ' -f1)
    if [ "$want" != "$got" ]; then
        echo "SHA-256 MISMATCH for $2" >&2
        echo "  want $want" >&2
        echo "  got  $got" >&2
        echo "Refusing to vendor. Update $3 from the hash the project publishes." >&2
        exit 1
    fi
}
fetch "$CURL_URL" curl.tar.gz curl.sha256
fetch "$MBEDTLS_URL" mbedtls.tar.bz2 mbedtls.sha256
fetch "$CACERT_URL" cacert.pem cacert.sha256
echo "SHA-256 ok"

tar xzf "$TMP/curl.tar.gz" -C "$TMP"
tar xjf "$TMP/mbedtls.tar.bz2" -C "$TMP"
CS="$TMP/curl-$CURL_VERSION"
MS="$TMP/mbedtls-$MBEDTLS_VERSION"
[ -d "$CS/lib" ] && [ -d "$MS/library" ] || { echo "unexpected tarball layout" >&2; exit 1; }

# A compiled artefact in a tarball would be silently dropped by .gitignore's
# blanket rules, leaving a tree in git that only builds locally.
if find "$CS" "$MS" -name '*.o' -o -name '*.a' -o -name '*.so' -o -name '*.dll' -o -name '*.lib' | grep -q .; then
    echo "tarball contains compiled artefacts — refusing to vendor" >&2
    exit 1
fi

# ---- curl_config.h from a real configure against mingw ------------------------
# mbedTLS has to exist as libraries for curl's configure to accept it, so build
# it first (in $TMP, thrown away) with the same user config the vendored copy
# compiles with.
cat > "$TMP/mingw.cmake" <<'EOF'
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
CFG="-D_WIN32_WINNT=0x0601 -DMBEDTLS_USER_CONFIG_FILE='\"$HERE/$W/src/vyto_mbedtls_user.h\"'"
echo "building a throwaway mbedTLS for curl's configure"
cmake -S "$MS" -B "$TMP/bm" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TMP/mingw.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF \
    -DCMAKE_C_FLAGS="$CFG" -DCMAKE_INSTALL_PREFIX="$TMP/mi" >/dev/null
ninja -C "$TMP/bm" install >/dev/null
echo "configuring curl"
# shellcheck disable=SC2086
cmake -S "$CS" -B "$TMP/bc" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TMP/mingw.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="$CFG" \
    -DMBEDTLS_INCLUDE_DIR="$TMP/mi/include" -DMBEDTLS_LIBRARY="$TMP/mi/lib/libmbedtls.a" \
    -DMBEDX509_LIBRARY="$TMP/mi/lib/libmbedx509.a" -DMBEDCRYPTO_LIBRARY="$TMP/mi/lib/libmbedcrypto.a" \
    $CURL_CMAKE_FLAGS > "$TMP/configure.log"
grep -q "Protocols: http https$" "$TMP/configure.log" || {
    echo "curl configured with unexpected protocols:" >&2
    grep "Protocols:" "$TMP/configure.log" >&2; exit 1; }
grep -q "Enabled SSL backends: mbedTLS$" "$TMP/configure.log" || {
    echo "curl did not configure with mbedTLS as its only TLS backend" >&2; exit 1; }

# ---- replace the vendored tree ---------------------------------------------
rm -rf "$W/include" "$W/src/curl" "$W/src/mbedtls" "$W/stage" "$W"/src/vcurl_*.c "$W"/src/vmbed_*.c
mkdir -p "$W/include" "$W/src/curl/lib" "$W/src/mbedtls/library" "$W/stage"

cp -r "$CS/include/curl" "$W/include/curl"
rm -f "$W/include/curl/Makefile"*
cp -r "$MS/include/mbedtls" "$MS/include/psa" "$W/include/"

# all curl lib headers (kept files include headers of skipped ones), kept .c only
( cd "$CS/lib" && find . -name '*.h' ) | sed 's|^\./||' | while read -r f; do
    mkdir -p "$W/src/curl/lib/$(dirname "$f")"; cp "$CS/lib/$f" "$W/src/curl/lib/$f"
done
CURL_SRCS=$( (cd "$CS/lib" && find . -name '*.c') | sed 's|^\./||' | grep -Ev "$CURL_SKIP" | sort)
for f in $CURL_SRCS; do
    mkdir -p "$W/src/curl/lib/$(dirname "$f")"; cp "$CS/lib/$f" "$W/src/curl/lib/$f"
done
cp "$MS"/library/*.c "$MS"/library/*.h "$W/src/mbedtls/library/"
MBED_SRCS=$( (cd "$MS/library" && ls *.c) | sort)

sed -e '1i /* GENERATED by native/refresh-curl.sh from a cmake configure of curl '"$CURL_VERSION"'\n * against x86_64-w64-mingw32 (msvcrt) and mbedTLS '"$MBEDTLS_VERSION"'. Do not edit: change\n * CURL_CMAKE_FLAGS in that script and re-run it. */' \
    "$TMP/bc/lib/curl_config.h" > "$W/src/curl_config.h"
if grep -q "$TMP" "$W/src/curl_config.h"; then
    echo "curl_config.h captured a temporary path — refusing" >&2; exit 1
fi

cp "$TMP/cacert.pem" "$W/stage/cacert.pem"
cp "$CS/COPYING" ../LICENSE-CURL.txt
cp "$MS/LICENSE" ../LICENSE-MBEDTLS.txt

# ---- wrapper TUs -------------------------------------------------------------
# vytoc globs native/<triple>/src/*.c flat, so the trees in src/curl and
# src/mbedtls are invisible to it; each upstream TU needs one wrapper at the top
# level. One per TU, never a unity build: mbedTLS is not unity-safe.
for f in $CURL_SRCS; do
    n=$(echo "$f" | sed 's|/|_|g')
    cat > "$W/src/vcurl_$n" <<WRAP
/* Wrapper TU for curl's lib/$f. GENERATED by native/refresh-curl.sh; do not
 * edit. Build flags come from vyto_curl_build.h (see that file). */
#ifndef VT_NO_LIBC
#include "vyto_curl_build.h"
#include "curl/lib/$f"
#endif
WRAP
done
for f in $MBED_SRCS; do
    cat > "$W/src/vmbed_$f" <<WRAP
/* Wrapper TU for mbedTLS's library/$f. GENERATED by native/refresh-curl.sh; do
 * not edit. Build flags come from vyto_mbedtls_build.h (see that file). */
#ifndef VT_NO_LIBC
#include "vyto_mbedtls_build.h"
#include "mbedtls/library/$f"
#endif
WRAP
done

# ---- manifest ----------------------------------------------------------------
( cd "$W" && find include src/curl src/mbedtls stage src/curl_config.h -type f \
      && ls src/vcurl_*.c src/vmbed_*.c ) | sort | ( cd "$W" && xargs sha256sum ) > vendor.files.sha256

echo "vendored curl $CURL_VERSION + mbedTLS $MBEDTLS_VERSION + CA bundle $CACERT_DATE"
echo "  $(echo "$CURL_SRCS" | wc -l) curl TUs, $(echo "$MBED_SRCS" | wc -l) mbedTLS TUs, $(du -sh "$W" | cut -f1)"
echo
echo "Now run:  make clean-cache && make test && make test-win"
