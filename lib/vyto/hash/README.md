# vyto/hash — fast, non-cryptographic hashing

The hashes you reach for when you are indexing a table or checking a transfer,
not when you are proving who wrote something. Pure Vyto, no dependency, nothing
to provision — it compiles for every target including freestanding, which
matters because a package manager has to verify a download before it can fetch
anything.

```vyto
import { hash_str, hash_int, xxh64, siphash24, crc32, str_bytes } from "vyto/hash";

hash_str("user:42")                    // 3251549993382861327 — non-negative int
hash_int(1) & 15                       // a bucket index, well spread
xxh64(str_bytes("abc"), 0)             // 0x44bc2cf5ad770999
siphash24(msg, k0, k1)                 // keyed, for keys you did not choose
crc32(str_bytes("123456789"))          // 0xcbf43926
```

## Modules

| Function | What it gives you |
|---|---|
| `hash_str` `hash_int` `hash_bytes` | the default for a `Map` or `HashMap` key: non-negative `int`, well spread, no allocation |
| `hash_combine` | one hash from two, order-dependent, so `(row, col)` and `(col, row)` differ |
| `xxh64` `xxh64_str` | the general-purpose 64-bit hash. Four accumulators over 32-byte stripes; roughly an order of magnitude faster per byte than FNV on long inputs |
| `siphash24` `siphash24_str` | keyed. The answer to hash-flooding |
| `fnv1a_32` `fnv1a_64` (+ `_str`) | tiny: one multiply and one xor per byte, no table, no state |
| `crc32` `crc32c` `crc32_str` | checksums. `CRC32_IEEE` is zip/gzip/PNG; `CRC32C` is iSCSI/ext4/SSE4.2 |
| `Crc` | a CRC with its 256-entry table built once, plus the streaming form (`begin`/`update`/`finish`) |
| `mix64` | splitmix64's finalizer — spreads a value with poor bit distribution |
| `str_bytes` | the UTF-8 bytes of a string |

## Which one

**A table key:** `hash_str` / `hash_int` / `hash_bytes`. They are compositions —
FNV-1a in place, then `mix64` — chosen because they allocate nothing and fix
FNV's weak avalanche. Do not reach past them unless you have measured.

**A lot of bytes:** `xxh64`. It is the fast one, and the seed lets one program
vary its hashing without changing code.

**Keys that come from the network:** `siphash24`, with a key drawn once at
startup from `vyto/util/uuid`'s `rand_bytes`. See the warning below.

**Detecting corruption:** `crc32` or `crc32c`, and never as a substitute for a
MAC.

## Two conventions the whole directory follows

**Byte arrays are the primary form; strings are a convenience.** Every hash
takes `byte[]`. The `_str` variants exist because most keys are strings.
`fnv1a_*_str`, `crc32_str` and `hash_str` read the string's bytes in place and
allocate nothing; `xxh64_str` and `siphash24_str` copy first, because their
block structure wants a real buffer — so they are the wrong call in a per-key
hot loop, and `hash_str` is the right one.

**Every function is endian-neutral.** xxHash64 and SipHash are defined over
little-endian reads, and this implementation does those reads a byte at a time,
so the same input gives the same number on every target. That is not a
performance compromise worth undoing: the C compiler folds the loop back into a
single load wherever it legally can.

## Things worth knowing before you use these

> **None of this is cryptography.** A `vyto/hash` value is trivially forgeable
> and trivially invertible for short inputs. Passwords go through
> `vyto/crypto/argon2`; message authentication goes through `hmac_sha256` in
> `vyto/crypto`; content addressing goes through `sha256`. SipHash being keyed
> makes it resistant to *collision-finding*, which is a denial-of-service
> property, not an authentication one — it is a 64-bit output and is not a tag.

> **A hardcoded SipHash key is the same as no key.** The whole mechanism is that
> an attacker cannot compute your collisions in advance. If the key ships in the
> binary, they can. Draw `(k0, k1)` once per process from `rand_bytes(16)` and
> keep them; never derive them from something an attacker can see, like a
> hostname or a start time rounded to the second.

> **`hash_int` is `mix64`, not the identity, on purpose.** A table keyed on
> sequential ids with an identity hash puts consecutive rows in consecutive
> buckets, which is fine until you mask to a power of two and every id ≡ 0 mod
> 16 lands together. `mix64` costs two multiplies and removes the whole class of
> problem. The fixture asserts that eight sequential ids land in at least five
> distinct low buckets.

> **`crc32` is table-free and `Crc` is not.** The free functions do eight shifts
> per byte, which is allocation-free and right for a one-shot checksum. `Crc`
> builds a 256-entry table in its `init` — about 4x faster per byte, and it pays
> for itself somewhere in the low hundreds of bytes. Build one and keep it if you
> are checksumming in a loop. There is no cached global table, because Vyto has
> no mutable module globals, and that is a feature here rather than a workaround.

> **CRC-32 and CRC-32C are not interchangeable.** Different polynomials,
> different values for the same input, both standard. `crc32("123456789")` is
> `0xcbf43926` and `crc32c("123456789")` is `0xe3069283`. Picking the wrong one
> produces a checksum that is internally consistent and disagrees with every
> other implementation of the format you are reading.

> **The tests check against other people's numbers.** `tests/fixtures/hash_vectors.vt`
> is 87 known-answer checks: FNV-1a and CRC's published vectors, xxHash64's
> reference values, and all sixteen SipHash-2-4 vectors from Aumasson &
> Bernstein's paper. Self-consistent hashing proves nothing — code that agrees
> with itself can disagree with every other implementation in the world.
