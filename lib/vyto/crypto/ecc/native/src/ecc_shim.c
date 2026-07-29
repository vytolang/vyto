/* ecc_shim.c — the C half of vyto/crypto/ecc.
 *
 * micro-ecc lives behind this file. Its types (uECC_Curve, uECC_HashContext)
 * and its curve selection never cross into Vyto: every entry point below takes
 * and returns plain scalars and caller-owned byte buffers of a fixed,
 * documented size, so a micro-ecc version bump cannot change a number the Vyto
 * side sees. Same arrangement as lib/vyto/regex/native/src/regex_shim.c.
 *
 * Contract, uniform across this file:
 *   - Returns 1 on success and 0 on failure, matching the library it wraps.
 *     There is no error code: every failure here is either "the key or point
 *     you handed me is not on the curve" or "I could not get entropy", and
 *     both are already distinguishable from the call the caller made.
 *   - Buffers are caller-allocated and exactly sized (VECC_* in ecc_shim.h).
 *     Nothing is heap-allocated here and nothing is returned by pointer.
 *   - Keys, points and signatures are raw big-endian byte strings in SEC1
 *     order: a public key is X||Y, a signature is r||s. That is what COSE,
 *     WebAuthn and every other consumer expects. See uecc_config.h on why
 *     uECC_VLI_NATIVE_LITTLE_ENDIAN must stay 0.
 *
 * Everything under native/src/microecc/ is upstream and byte-identical to the
 * v1.1 release; see native/refresh-microecc.sh.
 */

#include "uecc_config.h"
#include "microecc/uECC.h"
#include "ecc_shim.h"

#include <string.h>
#include <stdint.h>

#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#include <bcrypt.h>
#else
/* The /dev/urandom rung is reached on every non-Windows platform, including
   Linux when getrandom is unavailable (an old kernel, or a seccomp filter). */
#include <stdio.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#endif

/* ---- entropy ------------------------------------------------------------
 *
 * This deliberately does NOT call vyto/util/uuid's vt_rand_bytes, even though
 * that function exists and walks almost the same ladder. Its last resort is a
 * time-seeded rand() (rand_shim.c:63-66) and it returns void, so a caller
 * cannot tell that it happened.
 *
 * A guessable UUID is a bug. A guessable ECDSA nonce is a key disclosure: two
 * signatures under a biased or repeated k let anyone solve for the private key
 * directly, which is how the PS3 signing key and a long line of Bitcoin
 * wallets were lost. There is no safe degraded mode, so this ladder ends in
 * failure rather than in rand(), and every operation that consumes randomness
 * refuses to run rather than produce a weak result.
 */
static int strict_entropy(uint8_t *dest, unsigned size) {
    if (size == 0) return 1;
    size_t got = 0;
#if defined(__linux__)
    while (got < size) {
        ssize_t r = getrandom(dest + got, size - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
    }
#elif defined(_WIN32)
    if (BCRYPT_SUCCESS(BCryptGenRandom(NULL, (PUCHAR)dest, (ULONG)size,
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        got = size;
#endif
#if !defined(_WIN32)
    if (got < size) {
        FILE *f = fopen("/dev/urandom", "rb");
        if (f) {
            got += fread(dest + got, 1, size - got, f);
            fclose(f);
        }
    }
#endif
    if (got < size) {
        /* Fail closed. Wipe whatever partial draw we made so a caller that
         * ignores the return value cannot use half a nonce. */
        memset(dest, 0, size);
        return 0;
    }
    return 1;
}

/* uECC.c:189-192 leaves g_rng_function null because uecc_config.h elides
 * upstream's platform-specific.inc, so this must run before any operation that
 * draws randomness. Installed on every call rather than from an init function:
 * an init the caller can forget is an init that silently disables key
 * generation, and the assignment is a single store. */
static void ensure_rng(void) {
    uECC_set_rng(&strict_entropy);
}

static uECC_Curve p256(void) {
    return uECC_secp256r1();
}

/* ---- SHA-256 (FIPS 180-4) ----------------------------------------------
 *
 * A second SHA-256 in this tree, alongside the pure-Vyto one in
 * ../../crypto.vt. It is here because uECC_sign_deterministic needs a hash it
 * can drive through a C function-pointer table (uECC.h:303-312) many times per
 * signature, and routing that through Vyto would mean a callback per HMAC
 * block. Both implementations are held to the same known-answer vectors in
 * examples/86_crypto.vt precisely so this duplication cannot drift.
 */

typedef struct {
    uint32_t h[8];
    uint64_t len;       /* total message bytes */
    uint8_t  buf[64];
    unsigned n;         /* bytes currently in buf */
} sha256_ctx;

static const uint32_t K256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_ctx *c, const uint8_t *p) {
    uint32_t w[64], a, b, cc, d, e, f, g, h;
    unsigned i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_init(sha256_ctx *c) {
    c->h[0] = 0x6a09e667u; c->h[1] = 0xbb67ae85u;
    c->h[2] = 0x3c6ef372u; c->h[3] = 0xa54ff53au;
    c->h[4] = 0x510e527fu; c->h[5] = 0x9b05688cu;
    c->h[6] = 0x1f83d9abu; c->h[7] = 0x5be0cd19u;
    c->len = 0;
    c->n = 0;
}

static void sha256_update(sha256_ctx *c, const uint8_t *p, size_t n) {
    c->len += n;
    while (n > 0) {
        unsigned take = 64 - c->n;
        if (take > n) take = (unsigned)n;
        memcpy(c->buf + c->n, p, take);
        c->n += take;
        p += take;
        n -= take;
        if (c->n == 64) {
            sha256_block(c, c->buf);
            c->n = 0;
        }
    }
}

static void sha256_final(sha256_ctx *c, uint8_t *out) {
    uint64_t bits = c->len * 8;
    unsigned i;
    c->buf[c->n++] = 0x80;
    if (c->n > 56) {
        memset(c->buf + c->n, 0, 64 - c->n);
        sha256_block(c, c->buf);
        c->n = 0;
    }
    memset(c->buf + c->n, 0, 56 - c->n);
    for (i = 0; i < 8; i++)
        c->buf[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_block(c, c->buf);
    for (i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->h[i]);
    }
}

void vecc_sha256(const char *data, int len, char *out32) {
    sha256_ctx c;
    if (len < 0) len = 0;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)data, (size_t)len);
    sha256_final(&c, (uint8_t *)out32);
}

/* micro-ecc's hash vtable, wrapping the above. The context is a uECC_HashContext
 * with our sha256_ctx hanging off the end (uECC.h:275-301 uses the same trick). */
typedef struct {
    uECC_HashContext uECC;
    sha256_ctx ctx;
} vecc_hash_ctx;

static void h_init(const uECC_HashContext *base) {
    sha256_init(&((vecc_hash_ctx *)base)->ctx);
}
static void h_update(const uECC_HashContext *base, const uint8_t *msg, unsigned size) {
    sha256_update(&((vecc_hash_ctx *)base)->ctx, msg, size);
}
static void h_finish(const uECC_HashContext *base, uint8_t *out) {
    sha256_final(&((vecc_hash_ctx *)base)->ctx, out);
}

/* ---- key material -------------------------------------------------------- */

int vecc_make_key(char *pub, char *priv) {
    ensure_rng();
    return uECC_make_key((uint8_t *)pub, (uint8_t *)priv, p256());
}

int vecc_public_from_private(const char *priv, char *pub) {
    return uECC_compute_public_key((const uint8_t *)priv, (uint8_t *)pub, p256());
}

/* Full SEC1 point validation: on the curve, not the point at infinity, and in
 * the correct subgroup. Callers must run this on any public key that arrived
 * from outside before feeding it to vecc_shared_secret — an attacker-chosen
 * invalid point is the standard way to extract a static ECDH private key. */
int vecc_valid_public(const char *pub) {
    return uECC_valid_public_key((const uint8_t *)pub, p256());
}

/* Raw X-coordinate ECDH. This is a *shared secret*, not a key: it is a point
 * coordinate with structure, and it must go through a KDF before anything uses
 * it as one. vyto/crypto's hkdf_sha256 is the intended next hop. */
int vecc_shared_secret(const char *pub, const char *priv, char *secret) {
    ensure_rng();   /* enables micro-ecc's scalar blinding, uECC.c:1275 */
    if (!uECC_valid_public_key((const uint8_t *)pub, p256())) return 0;
    return uECC_shared_secret((const uint8_t *)pub, (const uint8_t *)priv,
                              (uint8_t *)secret, p256());
}

/* ---- signing ------------------------------------------------------------- */

int vecc_sign(const char *priv, const char *hash, int hashlen, char *sig) {
    if (hashlen <= 0) return 0;
    ensure_rng();
    return uECC_sign((const uint8_t *)priv, (const uint8_t *)hash, (unsigned)hashlen,
                     (uint8_t *)sig, p256());
}

/* Deterministic ECDSA: the nonce is derived from the private key and the message
 * hash by an HMAC-SHA256 DRBG rather than drawn from the RNG, so a signature
 * cannot be compromised by a weak or repeated draw.
 *
 * This follows RFC 6979's K/V construction but is NOT byte-compatible with it.
 * uECC.c:1450-1468 fills the candidate nonce through a (uint8_t *) view of a
 * machine-word array and masks the top word, where the RFC specifies a
 * big-endian bits2int; on a little-endian target the two derive different k
 * from the same inputs. Deliberately not corrected here — "fix" it and every
 * signature this module has ever produced becomes unreproducible, for a
 * property (matching the RFC's test vectors) that nothing needs. The security
 * argument for determinism does not depend on the RFC's exact byte order.
 *
 * ensure_rng() still runs because micro-ecc folds the RNG into scalar blinding
 * when one is present (uECC.h:317-319) — determinism and side-channel masking
 * are not exclusive. */
int vecc_sign_det(const char *priv, const char *hash, int hashlen, char *sig) {
    vecc_hash_ctx hc;
    uint8_t tmp[32 + 32 + 64];

    if (hashlen <= 0) return 0;
    ensure_rng();
    hc.uECC.init_hash   = &h_init;
    hc.uECC.update_hash = &h_update;
    hc.uECC.finish_hash = &h_finish;
    hc.uECC.block_size  = 64;
    hc.uECC.result_size = 32;
    hc.uECC.tmp         = tmp;
    return uECC_sign_deterministic((const uint8_t *)priv, (const uint8_t *)hash,
                                   (unsigned)hashlen, &hc.uECC, (uint8_t *)sig, p256());
}

int vecc_verify(const char *pub, const char *hash, int hashlen, const char *sig) {
    if (hashlen <= 0) return 0;
    return uECC_verify((const uint8_t *)pub, (const uint8_t *)hash, (unsigned)hashlen,
                       (const uint8_t *)sig, p256());
}

/* ---- point compression --------------------------------------------------- */

void vecc_compress(const char *pub, char *comp) {
    uECC_compress((const uint8_t *)pub, (uint8_t *)comp, p256());
}

/* Upstream returns void and does not validate: a compressed point whose X is
 * not on the curve decompresses to garbage rather than failing. Callers that
 * decompress untrusted input must follow with vecc_valid_public. */
void vecc_decompress(const char *comp, char *pub) {
    uECC_decompress((const uint8_t *)comp, (uint8_t *)pub, p256());
}
