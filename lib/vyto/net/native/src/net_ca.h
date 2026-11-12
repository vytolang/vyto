#ifndef NET_CA_H
#define NET_CA_H

/* Windows links the vendored static curl (native/windows-x64); without this
   curl.h declares every API function __declspec(dllimport). */
#ifdef _WIN32
#define CURL_STATICLIB 1
#endif
#include <curl/curl.h>

/* Trust configuration shared by every curl handle vyto/net creates
   (net_shim.c and http_shim.c). See net_ca.c. */

/* Replace the trust store with a PEM bundle at `path` (UTF-8), or go back to
   the default with NULL/"". Affects handles configured after the call. */
void vnet_ca_file_set(const char *path);

/* Apply the current trust configuration to a fresh easy handle. */
void vnet_ca_apply(CURL *curl);

#endif
