/* vyto/net: mbedTLS trimmed to what libcurl's HTTPS client needs on
 * windows-x64. Applied on top of mbedTLS's default mbedtls_config.h through
 * MBEDTLS_USER_CONFIG_FILE, so a refresh to a newer 3.6.x keeps every default
 * this file does not mention.
 *
 * Kept: TLS 1.2 and 1.3 client, ECDHE key exchange (RSA and ECDSA certs),
 * AES-GCM, ChaCha20-Poly1305, AES-CBC for older TLS 1.2 servers, X.509 chain
 * verification, PEM (the CA bundle), SNI, ALPN, PSA (TLS 1.3 needs it).
 * Entropy is BCryptGenRandom (mbedTLS's own Windows source; bcrypt.dll is in
 * the audited Windows 7 export list).
 *
 * Removed: every server-side piece, DTLS, PSK, static-RSA and finite-field
 * DHE key exchanges (no forward secrecy / rarely offered and slow), legacy
 * ciphers, certificate writing, PSA key storage on disk, self tests, and
 * mbedTLS's own sockets and timers (curl brings its own). Unreferenced code
 * would be dropped by --gc-sections anyway; turning it off here also keeps it
 * out of the compile and out of the attack surface.
 */
#undef MBEDTLS_SSL_SRV_C
#undef MBEDTLS_SSL_PROTO_DTLS
#undef MBEDTLS_SSL_DTLS_ANTI_REPLAY
#undef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#undef MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID
#undef MBEDTLS_SSL_CACHE_C
#undef MBEDTLS_SSL_COOKIE_C
#undef MBEDTLS_SSL_TICKET_C
#undef MBEDTLS_SSL_RENEGOTIATION
#undef MBEDTLS_SSL_CONTEXT_SERIALIZATION

#undef MBEDTLS_KEY_EXCHANGE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_DHE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECDHE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_RSA_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_DHE_RSA_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL_ENABLED
#undef MBEDTLS_ECJPAKE_C
#undef MBEDTLS_DHM_C

#undef MBEDTLS_DES_C
#undef MBEDTLS_ARIA_C
#undef MBEDTLS_CAMELLIA_C
#undef MBEDTLS_CCM_C
#undef MBEDTLS_NIST_KW_C
#undef MBEDTLS_CMAC_C
#undef MBEDTLS_RIPEMD160_C
#undef MBEDTLS_PADLOCK_C
#undef MBEDTLS_LMS_C

#undef MBEDTLS_PKCS7_C
#undef MBEDTLS_PKCS12_C
#undef MBEDTLS_X509_CSR_PARSE_C
#undef MBEDTLS_X509_CSR_WRITE_C
#undef MBEDTLS_X509_CRT_WRITE_C
#undef MBEDTLS_X509_CREATE_C
#undef MBEDTLS_PEM_WRITE_C

#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_ITS_FILE_C

#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_SELF_TEST
#undef MBEDTLS_VERSION_FEATURES
