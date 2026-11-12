/* Build flags for every vendored mbedTLS TU (vmbed_*.c). First-party; a
 * refresh leaves it alone. The options themselves are in vyto_mbedtls_user.h,
 * applied on top of mbedTLS's default configuration. Must match the
 * MBEDTLS_USER_CONFIG_FILE in vyto_curl_build.h. */
#ifndef VYTO_MBEDTLS_BUILD_H
#define VYTO_MBEDTLS_BUILD_H
#define MBEDTLS_USER_CONFIG_FILE "vyto_mbedtls_user.h"
#endif
