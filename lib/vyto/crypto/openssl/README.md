# vyto/crypto/openssl — TLS, X.509, and a libssl dependency

The reason this package exists is TLS. `vyto/net` gets HTTPS today by linking
libcurl, which is a whole HTTP client dragged in for its TLS stack, and a raw
`vyto/net/socket` `Socket` cannot be wrapped by any of it. This wraps one.

```vyto
import { tls_client, tls_connect, OSSL_WANT_READ, OSSL_WANT_WRITE }
    from "vyto/crypto/openssl";
import { socket_connect } from "vyto/net/socket";

let s = socket_connect("db.example.com", 5432);
s.setNonBlocking(true);

let ctx = tls_client();                     // verifies chain AND hostname
let c = tls_connect(ctx, s.fd as int, "db.example.com");

while (true) {
    let r = c.handshakeStep();
    if (r == 1) { break; }                  // done
    if (r != OSSL_WANT_READ && r != OSSL_WANT_WRITE) {
        print(c.lastError());               // a real reason, not "it failed"
        return;
    }
    waitFor(s, r == OSSL_WANT_WRITE);       // the direction it ASKED for
}

print(c.protocolVersion());                 // "TLSv1.3"
c.write(strBytes("hello"));
```

**It does not open sockets.** `tls_connect` takes a file descriptor, so the same
code works over anything that produced one — and it means this package links no
socket shim of its own.

## Status: the TLS half is implemented, the primitives are not

This is the part to read before reaching for anything below.

### Implemented

| | |
|---|---|
| `ossl_version()` · `ossl_available()` | the linked library's version string; whether it initialised |
| `tls_client()` | client context: verifies the chain against the system store **and** the hostname |
| `tls_client_verify_chain_only()` | chain but not hostname — what `sslmode=verify-ca` means |
| `tls_client_insecure_no_verification()` | verifies nothing. Named so it cannot be reached for by accident |
| `tls_server(chainPem, keyPem)` | server context from PEM **text**, not paths |
| `TlsContext.setMinVersion` · `.setAlpn` · `.trustFile` | TLS 1.2/1.3 floor · ALPN · an extra CA bundle |
| `tls_connect(ctx, fd, host)` · `tls_accept(ctx, fd)` | wrap a connected descriptor |
| `TlsConn.handshakeStep()` | the handshake primitive — see *the direction matters* below |
| `TlsConn.readInto` · `.writeFrom` | the bulk path: caller's buffer, no allocation |
| `TlsConn.read` · `.write` | the convenient path, returning an `OsslResult` |
| `TlsConn.peerCert` · `.protocolVersion` · `.alpnSelected` · `.lastError` | what was negotiated, and why it failed |
| `TlsConn.channelBinding("tls-server-end-point")` | RFC 5929 material, for SCRAM-SHA-256-PLUS |
| `TlsConn.close()` | sends `close_notify`; does **not** close the socket |
| `cert_from_pem` · `cert_from_der` | parse one certificate |
| `Cert.subject` · `.issuer` · `.serial` · `.notBefore` · `.notAfter` · `.sans` | extracted at construction, so they outlive the connection |
| `Cert.fingerprint()` | SHA-256 over the DER — what pinning compares |
| `cert_verify_chain(leaf, intermediates, roots)` | signatures, dates, basic constraints. **Not** the hostname |
| `cert_matches_host(cert, host)` | RFC 6125: SAN, CN only when there is no SAN, wildcards over one label |

### Not implemented — these still `panic`

`ossl_digest`, `ossl_hmac`, `ossl_seal`, `ossl_open`, `Pkey`, `ossl_genkey`,
`ossl_pkey_from_pem`, `ossl_pkey_to_pem`, `ossl_sign`, `ossl_verify`.

They panic rather than returning an empty result on purpose: a caller who
mistakes a zero-length ciphertext for success is the failure mode this package
exists to avoid. The signatures are in `openssl.vt` so the shape is settled, and
so this list is checkable rather than a promise.

TLS went first because the primitives have pure-Vyto equivalents that are
already vector-tested and cost nothing to link, and TLS has none.

**Use `vyto/crypto` and `vyto/crypto/argon2` for hashing, HMAC, HKDF, PBKDF2,
Argon2id and ChaCha20-Poly1305.** Reach here for a TLS connection or a parsed
certificate. When both eventually implement the same primitive they must agree
byte for byte, and `examples/86_crypto.vt` is where that gets proved — as a
third column, not a replacement one.

### Also out of scope, and likely to stay there

FIPS provider, providers beyond the default, DTLS, QUIC, OCSP stapling,
CMS/PKCS#7, client certificates, and anything needing a callback from C back
into Vyto until the function-pointer story is settled. CSR generation is wanted
but not started.

## The direction matters, and it is the one thing easy to get wrong

TLS is not a byte pipe. `SSL_read` can need the socket to become **writable** —
a TLS 1.3 key update, a renegotiation — and `SSL_write` can need it
**readable**. So `handshakeStep`, `readInto` and `writeFrom` do not report a
bare "would block"; they report *which direction*:

| Return | Meaning |
|---|---|
| `> 0` | bytes transferred (a write may be **short**) |
| `1` (handshake) | complete |
| `0` | clean close — the peer sent `close_notify` |
| `OSSL_WANT_READ` (-1) | wait for readability, then call again |
| `OSSL_WANT_WRITE` (-2) | wait for **writability**, then call again |
| `OSSL_FAILED` (-3) | failed; `lastError()` says why |

Code that always waits for readability after a blocked read will deadlock, and
only on a long-lived connection under load — which is the worst way to find out.
`lib/vyto/db/wire/tls.vt` is the worked example: it records the direction the
SSL layer asked for and overrides `awaitReady` to ignore the direction its
*caller* assumed.

## Verification is on by default, and turning it off is loud

A TLS client that does not verify is strictly worse than plaintext: it costs the
same and provides confidence it has not earned. Against an attacker who can
route packets it is worth less than plaintext, because plaintext does not
persuade anyone they are safe.

So there are three constructors and the weak ones say so in their names.
`tls_client()` is the one to use. `cert_verify_chain` and `cert_matches_host`
are kept **separate** because the two are independently wrong in practice, and a
combined call hides which one a caller forgot.

`examples/107_tls.vt` asserts the three levels actually differ — `verify-full`
rejects a hostname that `verify-ca` accepts on the same connection. Without that
pair, every `sslmode=verify-full` in `vyto/db` could silently be getting
`require` and nothing would notice.

## Provisioning — the part that decides everything else

| Target | How |
|---|---|
| Linux | `#link "ssl"` / `#link "crypto"` — the distro's `libssl-dev`. Nothing to run |
| macOS | `native/provision-openssl.sh macos-arm64 --from <prefix>` |
| Windows | `native/provision-openssl.sh windows-x64 --from <prefix>` |

macOS ships `libssl.dylib` but no headers, and Homebrew's live under a prefix
`vytoc` cannot be told about: there is no `#cflags` pragma, and the only include
paths a shim gets are `native/src` and `native/<triple>/include`. Windows has no
system OpenSSL at all. Both are therefore provisioned into
`native/<triple>/include/openssl/`, where the prebuilt scan finds the library
beside them.

**The script does not download a tarball, and that is deliberate.**
`provision-curl.sh` fetches curl's headers from its release tarball, which is
correct for curl and wrong here: OpenSSL's `opensslconf.h` and
`configuration.h` are *generated by Configure* and encode the options that build
was made with. Headers from a generic tarball are not the headers your
`libcrypto` was built with, and the mismatch is silent — it compiles, it links,
and it goes wrong at runtime in a struct layout. So the headers come from the
same prefix as the binary and the script's job is to prove they match.

The binaries are never committed (`*.dll`, `*.dylib` are gitignored); they ship
with a Vyto release, exactly as libcurl's DLL does.

**Version floor: OpenSSL 3.0.** The 1.1 → 3.x soname change is not
ABI-compatible and 3.x deprecated the low-level APIs a dual-version shim would
need two paths for. `ossl_shim.c` refuses to compile below it with an `#error`,
and `provision-openssl.sh` checks it earlier, where the message can be useful.

Two version traps worth knowing: Argon2id via `EVP_KDF` exists only in 3.2+, so
`vyto/crypto/argon2` stays the default for password hashing regardless of what
is installed; and the FIPS provider must be loaded explicitly in 3.x, where
loading it *removes* algorithms rather than adding them.

## Contract with the C

Same as `regex_shim.c` for PCRE2 and `ecc_shim.c` for micro-ecc: **no OpenSSL
type, constant or error code crosses into Vyto.** Handles are opaque pointers to
structs declared in `native/src/ossl_shim.h`, statuses are that header's own
`VOSSL_*` numbers, and every string is written into a caller-owned `(out, cap)`
buffer that reports the length it wanted. A libssl upgrade cannot change a
number the Vyto side compares against.

Three consequences worth stating:

**ARC frees everything.** `Cert`, `TlsContext` and `TlsConn` own their C objects
and free them in `deinit`. No caller ever frees. A `TlsConn` holds a strong
reference to its `TlsContext`, because the `SSL` does not keep the `SSL_CTX`
alive by itself and a context that went out of scope in the caller would be
freed underneath a live connection.

**The error queue is drained on every entry and exit.** OpenSSL's queue is
per-thread and sticky: an unread error from one call is reported as the cause of
the next, and a handshake that "failed for no reason" is nearly always that. A
verification failure is reported from `SSL_get_verify_result` in preference to
the queue, because "unable to get local issuer certificate" is a fixable message
and "tlsv1 alert unknown ca" is not.

**There is a full `VT_NO_LIBC` arm.** `--freestanding` splices
`-DVT_NO_LIBC` into every package shim's compile line, not just the runtime's,
so an unguarded `<openssl/ssl.h>` would break the freestanding build of anything
that merely imports this package. The stub arm answers 0/null everywhere, so
`ossl_available()` is false and every constructor returns a handle whose `ok()`
is false — outcomes callers already have to handle, so the `.vt` needs no
platform conditionals. `tests/fixtures/openssl_freestanding.vt` exists solely to
keep that arm compiling, because nothing else in the suite does.

## The include is unconditional, deliberately

`ossl_shim.c` includes the OpenSSL headers with no `#ifdef`, exactly as
`vyto/net`'s `net_shim.c` includes `<curl/curl.h>`. `native/src` is globbed flat
and compiled for every target, so a triple with no provisioned headers **fails
to compile this package**, loudly, naming the missing header.

That is the intended outcome. A TLS package that silently compiles to nothing on
some platform is how a program ships without encryption.

## Who uses it

`vyto/db/pgsql` and `vyto/db/mysql`, through `vyto/db/wire/tls`. Both default to
plaintext — `sslmode=disable`, `ssl-mode=DISABLED` — because whether a given
server speaks TLS is something the application knows and a driver does not; ask
for it per connection. Importing `vyto/db` itself links no libssl, and
`tests/run_tests.sh`'s `db_no_driver_contagion` fails if an `ossl` object is
ever produced for a driverless program.

## Tests

`examples/107_tls.vt` — 58 assertions, in the golden suite, and **it needs no
server, no network and no internet**. A client and a server `TlsConn` are both
constructed in one process over a loopback socket pair and driven by one pump
loop, so a real TLS 1.3 handshake — certificate verification, hostname matching,
ALPN negotiation, application data both ways, `close_notify` — runs inside
`make test`.

It covers the RFC 6125 rules individually rather than as one "the certificate is
valid", because each is a rule someone has got wrong in a shipped TLS client: a
wildcard matches one label and not two, never the bare domain, never across
domains; the CN is consulted only when there is no SAN at all; an IP SAN matches
only the right address.

Certificates live in `tests/fixtures/tls`, generated by `gen.sh` and **committed
with their private keys**. They are throwaway, nothing trusts them, and nothing
may reuse them. They are in git because a suite that generates its own key
material needs `openssl(1)` on the machine running it — which would make it a
test of the CLI rather than of these bindings, and would skip silently wherever
the CLI is missing. `expired.crt` is dated to January 2020 on purpose, so the
expiry check cannot rot into a pass as the clock moves.

Live coverage against real servers is opt-in and outside `make test`:
`tests/fixtures/pgsql_tls_live.vt` and `mysql_tls_live.vt`, driven by
`tests/run_tests_pgsql.sh` and `run_tests_mysql.sh`.
