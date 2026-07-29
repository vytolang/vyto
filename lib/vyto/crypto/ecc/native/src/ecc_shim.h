/* ecc_shim.h — the C half of vyto/crypto/ecc. See ecc_shim.c for the contract. */

#ifndef VYTO_ECC_SHIM_H
#define VYTO_ECC_SHIM_H

/* Fixed sizes for P-256, in bytes. The Vyto side mirrors these as consts. */
#define VECC_PRIV_SIZE  32
#define VECC_PUB_SIZE   64
#define VECC_COMP_SIZE  33
#define VECC_SIG_SIZE   64
#define VECC_SECRET_SIZE 32

int  vecc_make_key(char *pub, char *priv);
int  vecc_public_from_private(const char *priv, char *pub);
int  vecc_valid_public(const char *pub);
int  vecc_shared_secret(const char *pub, const char *priv, char *secret);
int  vecc_sign(const char *priv, const char *hash, int hashlen, char *sig);
int  vecc_sign_det(const char *priv, const char *hash, int hashlen, char *sig);
int  vecc_verify(const char *pub, const char *hash, int hashlen, const char *sig);
void vecc_compress(const char *pub, char *comp);
void vecc_decompress(const char *comp, char *pub);

/* SHA-256 over one buffer, used by RFC 6979 above and exported so the test
 * suite can hold this implementation and the pure-Vyto one to the same
 * vectors. Writes 32 bytes. */
void vecc_sha256(const char *data, int len, char *out32);

#endif /* VYTO_ECC_SHIM_H */
