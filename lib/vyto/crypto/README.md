# vyto/crypto — hashing, key derivation, authenticated encryption, P-256

Four packages, split on build boundaries. A `native/src` compiles once per
package *directory*, so the split is what keeps a program that hashes a password
from linking anything:

| Import | What it gives you | Native code |
|--------|-------------------|-------------|
| `vyto/crypto` | SHA-256, SHA-512, SHA-384, SHA-1, MD5, HMAC, HKDF, PBKDF2, ChaCha20-Poly1305, constant-time compare, hex | none — pure Vyto |
| `vyto/crypto/argon2` | Argon2id/i/d password hashing, BLAKE2b | none — pure Vyto |
| `vyto/crypto/ecc` | P-256 key generation, ECDSA, ECDH, point compression | micro-ecc v1.1, vendored |
| `vyto/crypto/openssl` | TLS client and server over any socket, X.509 parsing, chain verification, RFC 6125 hostname matching, channel binding | **system** libssl/libcrypto 3.0+ |

`vyto/crypto/openssl` is the odd one out and deliberately so: it is the only
package here with an external dependency, it is provisioned rather than
vendored (an OpenSSL build is a decision, not a fetch — `native/provision-openssl.sh`),
and its primitives are **not** implemented, only its TLS and X.509 halves.
Use the pure-Vyto modules for hashing and AEAD; reach for this one when you
need a TLS connection or a certificate parsed. It has its own README —
[`openssl/README.md`](openssl/README.md) — with the implemented / not-implemented
list, the provisioning story, and the non-blocking rule that is easy to get
wrong.

```vyto
import { sha256, hmac_sha256, hkdf_sha256, pbkdf2_sha256,
         chacha20poly1305_seal, chacha20poly1305_open,
         ct_equal, hex_encode, strBytes } from "vyto/crypto";
import { ecc_keypair, ecdsa_sign, ecdsa_verify, ecdh } from "vyto/crypto/ecc";

hex_encode(sha256(strBytes("abc")))          // "ba7816bf8f01cfea…"
hmac_sha256(key, msg)                        // 32 bytes
hkdf_sha256(ikm, salt, strBytes("ctx"), 32)  // key from a key
argon2id(pw, salt, 3, 262144, 1, 32)         // key from a password — use this
pbkdf2_sha256(pw, salt, 600000, 32)          // the older, weaker way

let sealed = chacha20poly1305_seal(key32, nonce12, plaintext, aad);
let r = chacha20poly1305_open(key32, nonce12, sealed, aad);
if (r.ok) { use(r.data); }                   // never touch r.data unless ok

let kp = ecc_keypair();                      // check kp.ok
let sig = ecdsa_sign(kp.priv, sha256(msg));  // deterministic nonce
ecdsa_verify(kp.pub, sha256(msg), sig.data)  // bool
```

## Why the split

A `native/src` is compiled once per package **directory**, for every module in
it. Putting `ecc.vt` beside `crypto.vt` would compile micro-ecc — and, on
Windows, force the bcrypt link — into every program that only wanted to hash a
password. `vyto/util/uuid` is split from `vyto/util` for exactly this reason
(see `lib/vyto/util/uuid/uuid.vt:20-24`), and this follows it.

So the common case costs nothing: `import … from "vyto/crypto"` pulls in no C,
no system library, and nothing to provision.

## Why micro-ecc is vendored into git

Same reasoning as PCRE2 in `vyto/regex`, and the opposite of blend2d, ICU and
libcurl. Those are fetched or built out of band because they are large, C++, or
need a build-system decision that cannot be made for the user. micro-ecc is
~2 500 lines of dependency-free C with no build system of its own.

The deciding argument is testing. A fetched dependency makes the suite
**silently skip** on a fresh clone (`tests/run_tests.sh` does this for the gfx
tests by design). Skipping a canvas backend is a missing feature you notice
immediately; skipping the crypto tests produces a green run that proves
nothing, which is strictly worse than a red one.

`native/refresh-microecc.sh` moves the vendored tree to a new release
reproducibly and `--verify` proves the tree in git is byte-identical to the
release it claims:

```sh
lib/vyto/crypto/ecc/native/refresh-microecc.sh --verify
```

Nothing in the build calls that script. It exists so provenance is checkable
rather than asserted.

## Byte formats

All big-endian, all fixed-size, all raw — no DER, no PEM, no ASN.1.

| Thing | Size | Layout |
|-------|------|--------|
| private key | 32 | scalar `d` |
| public key | 64 | `X‖Y`, no `0x04` prefix |
| compressed point | 33 | `0x02`/`0x03` ‖ `X` (prefix is Y's parity) |
| signature | 64 | `r‖s` |
| ECDH shared secret | 32 | X coordinate of the shared point |
| AEAD output | n+16 | ciphertext ‖ Poly1305 tag |

WebAuthn wants the signature DER-encoded and the public key in COSE_Key form.
Neither is done here — this layer produces the raw values those encodings wrap.

## Security notes

These are the things that will actually bite, in the order they will bite.

**Nonces must never repeat under one key.** ChaCha20-Poly1305 does not degrade
on nonce reuse, it breaks: two messages under the same key and nonce leak the
XOR of their plaintexts *and* let an attacker forge tags for that key. There is
no nonce management in this module on purpose — whoever owns the key owns the
counter. Draw 12 fresh bytes per message from `rand_bytes`, or keep a counter
that is persisted before use, never after.

**Randomness fails closed.** `vyto/util/uuid`'s `rand_bytes` ends its fallback
ladder at a time-seeded `rand()` and returns `void`
(`lib/vyto/util/uuid/native/src/rand_shim.c:63-66`). That is a defensible
choice for a UUID and a fatal one for an ECDSA nonce, where a predictable draw
leaks the private key outright. So `ecc` has its own ladder — getrandom,
BCryptGenRandom, `/dev/urandom` — with no PRNG at the end, and every operation
that consumes randomness returns `ok == false` rather than producing a weak
result. **Check `.ok`.** A zeroed key is not a key.

**Signing is deterministic by default.** `ecdsa_sign` derives its nonce by
HMAC-SHA256 DRBG over the private key and message hash, so it does not depend
on the RNG at signing time at all. It follows RFC 6979's structure but is *not*
byte-compatible with it — micro-ecc assembles the candidate nonce as raw
native-endian machine words where the RFC specifies a big-endian `bits2int`
(`uECC.c:1450-1468`). Signatures verify everywhere; they just cannot be
reproduced from the RFC's vectors. `ecdsa_sign_random` exists for protocols
that need fresh signatures each time and is strictly more dangerous.

**Validate foreign public keys.** `ecc_valid_public` does the full SEC1 check.
`ecdh` runs it for you; do it yourself before storing a key that arrived from
outside. Feeding attacker-chosen invalid points to a static ECDH key extracts
that key one query at a time.

**None of this is hardened against timing side channels.** It is transpiled C
at `-O2` with data-dependent branches and no cache-timing defence; micro-ecc
targets microcontrollers and blinds scalars when an RNG is installed (this shim
always installs one), but it does not claim BoringSSL's posture. This is the
right tool for a local vault on hardware you control. It is the wrong tool for
a network service that decrypts attacker-supplied ciphertext on demand and can
be timed. `ct_equal` is the deliberate exception, because tag comparison is
where a timing leak becomes a forgery — use it for every secret comparison.

**Use Argon2id for passwords, not PBKDF2.** PBKDF2 needs almost no memory, so
an attacker's GPU runs thousands of guesses in parallel for the price of one.
Argon2id charges every guess for hundreds of megabytes of randomly-accessed
RAM, which is the resource that does not parallelise cheaply. PBKDF2 remains in
`vyto/crypto` for compatibility with keys already derived that way.

## Performance, measured

`vytoc run … --release` on the development box (linux-x64):

| Operation | Cost |
|-----------|------|
| SHA-256 | ~62 MB/s |
| SHA-512 | ~101 MB/s |
| PBKDF2-HMAC-SHA256 | ~220 000 iterations/sec |
| PBKDF2-HMAC-SHA512 | ~167 000 iterations/sec |
| PBKDF2 at OWASP's 600 000-iteration floor | **2.7 s** |
| Argon2id t=3, m=16 MiB, p=1 | 88 ms |
| Argon2id t=3, m=64 MiB, p=1 | **357 ms** |
| Argon2id t=3, m=256 MiB, p=1 | 1.5 s |

The comparison that matters is the last two rows against the third. **Argon2id
at 256 MiB is faster than PBKDF2 at its recommended floor *and* enormously
harder to attack** — the attacker has to find 256 MiB per parallel guess
instead of essentially nothing. That is not a tradeoff; it is better on both
axes, which is why the recommendation is unconditional.

**SHA-512 is the faster hash here, by about 1.5×**, which surprises people who
read it as "SHA-256 but bigger". It processes a 128-byte block per round set
where SHA-256 processes 64, and its 64-bit words are one machine register on
every triple vytoc targets — where SHA-256's 32-bit arithmetic costs an extra
mask after every operation to stay inside a signed 64-bit `int`. On a 32-bit
target the ordering would reverse. This is throughput only: both are the same
one-shot construction, and SHA-256 remains the more interoperable default.

The PBKDF2 numbers are roughly an order of magnitude off OpenSSL, which uses the
SHA-NI instructions. Argon2id closes that gap for the password case by making
memory rather than hash throughput the bottleneck, so the pure-Vyto
implementation is not meaningfully handicapped: Vyto moves ~1.5 GB/s
sequentially through a large `byte[]`, and memory bandwidth is what Argon2
spends.

Pick parameters from measured time on the *slowest* machine that has to open
the vault, and treat them as security parameters — they are the only thing
between a stolen vault file and offline guessing. Raise `m` first; raising `p`
without threads mostly just changes the output (see below).

## Argon2 specifics

`argon2id` is the one to call. `argon2i` and `argon2d` are exported for
completeness and for reading other people's hashes.

- **Single-threaded.** RFC 9106 expects the lanes of a segment to run in
  parallel; here they are a loop. Output is byte-identical either way —
  synchronisation happens at segment boundaries — so this interoperates with
  any other Argon2. It simply is not faster at higher `p`. Raise `m`.
- **Why id and not d.** Argon2id's first half-pass is data-independent, which
  denies a side-channel attacker the memory access pattern; the rest is
  data-dependent, which keeps the time-memory tradeoff hard. Argon2d is
  stronger against tradeoff attacks alone and weaker against anyone who can
  watch memory, so it is the wrong default.
- **Invalid parameters panic**, they do not return an empty array. A KDF that
  quietly hands back zero bytes on a bad argument produces an all-zero key that
  works perfectly until someone notices every vault shares it. Salt must be
  ≥ 8 bytes and memory ≥ 8 KiB per lane, per the RFC.
- **`secret` and `ad`** are available through `argon2_raw`. `secret` is a
  pepper — a key kept outside the vault file, so a stolen file alone cannot be
  attacked offline.

## Tests

`examples/86_crypto.vt` — 97 checks, run by `tests/run_tests.sh` like every
other numbered example. Nearly all are known-answer tests against values
published elsewhere (FIPS 180-4, RFC 4231, RFC 5869, RFC 7693, RFC 8018, RFC
8439, RFC 9106, RFC 6979's key pair), because self-consistent crypto proves
nothing: code that round-trips its own output perfectly can still disagree with
every other implementation on the planet.

All three RFC 9106 Argon2 vectors pass, including `p=4` with a secret and
associated data — which is why the implementation supports multi-lane
addressing despite running the lanes sequentially. Dropping to `p=1`-only would
have made the spec's own vectors unusable and left nothing external to check
against.

The four things checked by property rather than constant are the ones that
cannot have constants — key generation, randomised signing, the deterministic
nonce that is not RFC 6979 byte-compatible, and HKDF-SHA512, for which RFC 5869
publishes no vectors at all (its appendix covers SHA-256 and SHA-1 only). Those
are covered by round-trips, two-party agreement, domain separation, and
rejection of tampered input.

**SHA-512's own vectors are known-answer throughout**, including the 1M×`'a'`
case that catches a 128-byte padding off-by-one and RFC 4231's case 6, whose
131-byte key exceeds the block and forces the hash-down path — the one place a
constant copied from the SHA-256 code (64 instead of 128) would survive every
other test.

SHA-256 exists twice in this tree — pure Vyto in `crypto.vt` and C in
`ecc_shim.c`, where micro-ecc's deterministic signing needs to drive it through
a function-pointer table many times per signature. Both are held to the same
vectors in that example, which is what keeps the duplication from drifting.
