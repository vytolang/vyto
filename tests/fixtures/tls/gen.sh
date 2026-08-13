#!/bin/sh
# gen.sh — regenerate the certificates examples/107_tls.vt runs against.
#
# THESE ARE TEST CERTIFICATES AND THEIR PRIVATE KEYS ARE IN GIT. They are
# checked in on purpose: 107_tls.vt is part of `make test`, and a suite that
# generates its own key material needs openssl(1) on the machine running it,
# which defeats the point of testing the Vyto bindings rather than the CLI.
# Nothing here is trusted by anything. Do not reuse a byte of it.
#
# Re-running this rewrites every file EXCEPT expired.crt, which is dated in the
# past on purpose (see below) and is regenerated only by --all.
#
#   sh tests/fixtures/tls/gen.sh          # roots and live leaves
#   sh tests/fixtures/tls/gen.sh --all    # those plus expired.crt
#
# What each file is for, in 107_tls.vt:
#
#   root.crt/.key       the CA everything below chains to; the trust anchor the
#                       verification tests pass in as `roots`
#   other-root.crt/.key an unrelated CA, so "rejects the wrong root" is a real
#                       rejection rather than an empty store failing
#   leaf.crt/.key       the server certificate the live handshake uses.
#                       CN=localhost, SAN: DNS localhost, DNS db.example.com,
#                       IP 127.0.0.1 — three SAN shapes in one certificate
#   wildcard.crt        CN=*.example.com, SAN DNS *.example.com — the wildcard
#                       rules (one label, never the public suffix)
#   cn-only.crt         CN=cn.example.com with NO SAN at all, which is the only
#                       case where RFC 6125 permits the CN fallback
#   expired.crt         valid in 2020 and nowhere near now, so the expiry test
#                       cannot rot into a pass by the clock moving

set -e
cd "$(dirname "$0")"

ALL=0
[ "$1" = "--all" ] && ALL=1

DAYS=7300      # 20 years: long enough that these do not expire during the
               # project's life, which is the only way an expiry failure in
               # this suite stays meaningful.

key() { openssl ecparam -name prime256v1 -genkey -noout -out "$1" 2>/dev/null; }

# ---- the two roots ----------------------------------------------------------

key root.key
openssl req -x509 -new -key root.key -sha256 -days $DAYS -out root.crt \
    -subj "/CN=Vyto Test Root CA" \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null

key other-root.key
openssl req -x509 -new -key other-root.key -sha256 -days $DAYS -out other-root.crt \
    -subj "/CN=Vyto Test Unrelated Root CA" \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null

# ---- leaves -----------------------------------------------------------------

# $1 out-stem  $2 subject CN  $3 SAN string ("" for none)  $4 key file
leaf() {
    out="$1"; cn="$2"; san="$3"; kf="$4"
    openssl req -new -key "$kf" -out "$out.csr" -subj "/CN=$cn" 2>/dev/null
    if [ -n "$san" ]; then
        printf 'basicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=%s\n' "$san" > "$out.ext"
    else
        printf 'basicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n' > "$out.ext"
    fi
    openssl x509 -req -in "$out.csr" -CA root.crt -CAkey root.key -CAcreateserial \
        -out "$out.crt" -days $DAYS -sha256 -extfile "$out.ext" 2>/dev/null
    rm -f "$out.csr" "$out.ext"
}

key leaf.key
leaf leaf "localhost" "DNS:localhost,DNS:db.example.com,IP:127.0.0.1" leaf.key
leaf wildcard "*.example.com" "DNS:*.example.com" leaf.key
leaf cn-only "cn.example.com" "" leaf.key

# ---- the expired one --------------------------------------------------------
#
# `openssl x509 -req -days N` can only date from now, and `-not_before` /
# `-not_after` arrived in 3.2 — too new to require. `openssl ca` takes explicit
# dates in every version that matters, at the cost of a scratch CA database.
if [ $ALL -eq 1 ]; then
    rm -rf ca.tmp
    mkdir -p ca.tmp/newcerts
    : > ca.tmp/index.txt
    echo 1000 > ca.tmp/serial
    cat > ca.tmp/ca.cnf <<'CNF'
[ ca ]
default_ca = t
[ t ]
dir             = ca.tmp
database        = ca.tmp/index.txt
new_certs_dir   = ca.tmp/newcerts
serial          = ca.tmp/serial
certificate     = root.crt
private_key     = root.key
default_md      = sha256
policy          = pol
email_in_dn     = no
rand_serial     = no
unique_subject  = no
[ pol ]
commonName = supplied
[ ext ]
basicConstraints = CA:FALSE
keyUsage = digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = DNS:localhost
CNF
    openssl req -new -key leaf.key -out ca.tmp/expired.csr -subj "/CN=localhost" 2>/dev/null
    openssl ca -config ca.tmp/ca.cnf -batch -notext -extensions ext \
        -startdate 20200101000000Z -enddate 20200201000000Z \
        -in ca.tmp/expired.csr -out expired.crt 2>/dev/null
    rm -rf ca.tmp
fi

rm -f root.srl
echo "wrote: root.crt other-root.crt leaf.crt wildcard.crt cn-only.crt$([ $ALL -eq 1 ] && echo ' expired.crt')"
