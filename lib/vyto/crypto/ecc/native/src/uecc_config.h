/* uecc_config.h — micro-ecc build configuration for vyto/crypto/ecc.
 *
 * Written by hand and included by BOTH translation units that see uECC.h
 * (vecc_uecc.c and ecc_shim.c), because micro-ecc's public header changes
 * shape based on these macros: a TU that saw different values would disagree
 * with the compiled library about which curves exist. vytoc has no #cflags
 * pragma and the native compile line is fixed (src/main.c:616-635), so every
 * -D has to come from a C file — the same constraint that produced
 * lib/vyto/regex/native/src/config.h.
 *
 * Every value here is identical on linux-x64, linux-arm64, macos-* and
 * windows-x64. uECC_WORD_SIZE is left to micro-ecc's own detection, which
 * picks 8 on every triple vytoc supports (all are 64-bit); it changes only
 * performance, never the bytes a key or signature is made of.
 */

#ifndef VYTO_UECC_CONFIG_H
#define VYTO_UECC_CONFIG_H

/* ---- curves -------------------------------------------------------------
 * P-256 only. WebAuthn/FIDO2 mandate ES256 (ECDSA-P256-SHA256) and every
 * other curve here is dead weight in the binary — secp256k1 in particular is
 * a Bitcoin curve that no WebAuthn relying party will ever ask for, and the
 * short curves (160/192/224) are below contemporary strength. A caller who
 * needs one of those should say so and flip a flag, not get it by default.
 */
#define uECC_SUPPORTS_secp160r1 0
#define uECC_SUPPORTS_secp192r1 0
#define uECC_SUPPORTS_secp224r1 0
#define uECC_SUPPORTS_secp256r1 1
#define uECC_SUPPORTS_secp256k1 0

/* Point compression: 33 bytes instead of 65 for a stored public key. Cheap to
 * carry and a vault that keeps one row per credential notices the difference. */
#define uECC_SUPPORT_COMPRESSED_POINT 1

/* ---- wire format --------------------------------------------------------
 * MUST stay 0. At 1, micro-ecc stores scalars in native word order and the
 * keys and signatures it produces stop being the big-endian byte strings that
 * SEC1, COSE and WebAuthn all specify — they would round-trip only against
 * another micro-ecc built the same way, on the same endianness. Interop is the
 * entire point of this module, so this is not a tunable.
 */
#define uECC_VLI_NATIVE_LITTLE_ENDIAN 0

/* ---- entropy ------------------------------------------------------------
 * Defining micro-ecc's include guard *before* it is reached elides
 * platform-specific.inc wholesale (uECC.c:89) without touching the vendored
 * tree. That drops upstream's default_RNG, and it is deliberate:
 *
 *   - The POSIX default opens /dev/urandom by hand. vyto/util/uuid already
 *     solved entropy properly (getrandom(2), then BCryptGenRandom, then
 *     urandom) and having a second, weaker source in the same binary is how
 *     the two drift apart.
 *   - The Windows default pulls in <wincrypt.h> and asks for advapi32 and
 *     crypt32 via `#pragma comment(lib, ...)`, which is MSVC-only — mingw-w64,
 *     which is how vytoc cross-builds windows-x64, ignores it silently and the
 *     link fails later with no explanation.
 *
 * uECC.c:189-192 then leaves g_rng_function null, so key generation refuses to
 * run until ecc_shim.c installs ours. Failing closed is the right default for
 * a keygen: a silent fallback to a weak source is the failure nobody notices.
 *
 * types.h still arrives — uECC_vli.h:7 includes it at uECC.c:4, long before
 * the elided include.
 */
#define _UECC_PLATFORM_SPECIFIC_H_

#endif /* VYTO_UECC_CONFIG_H */
