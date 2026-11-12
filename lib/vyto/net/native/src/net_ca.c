/* Where vyto/net's HTTPS trust anchors come from.
 *
 * Linux / macOS: the system libcurl and its TLS library already know the
 * platform's CA store, so the default is to set nothing. http_set_ca_file()
 * replaces it with CURLOPT_CAINFO.
 *
 * Windows: curl is the vendored build (native/windows-x64), with mbedTLS doing
 * TLS. mbedTLS has no idea where Windows keeps certificates, so without help
 * every HTTPS request would fail verification. The default trust store is one
 * PEM blob, built once per process and handed over as CURLOPT_CAINFO_BLOB:
 *
 *   1. every certificate in the Windows ROOT store (what the OS trusts), plus
 *   2. cacert.pem beside the exe — the Mozilla bundle vytoc stages there from
 *      native/windows-x64/stage/.
 *
 * Both, because neither is enough alone. Windows installs most roots lazily,
 * only when its own TLS stack first meets one, so on a machine that rarely
 * browses (a Windows 7 kiosk, say) the ROOT store can lack a root as common as
 * ISRG Root X1. The bundle covers that, and the ROOT store covers an
 * enterprise CA an administrator deployed by policy. Without cacert.pem (an
 * exe copied on its own) the ROOT store still works.
 *
 * One blob rather than CAINFO + CAINFO_BLOB: curl's mbedTLS backend ignores
 * CAINFO whenever a blob is set (lib/vtls/mbedtls.c), so the two have to be
 * merged here. http_set_ca_file() on Windows also goes through a blob, read
 * with _wfopen: mbedTLS opens CAINFO with narrow fopen, which cannot reach a
 * path like C:\Users\Zoë.
 *
 * What is never done, on any platform: turn verification off. If no root at
 * all can be found the blob stays empty, nothing is set, and every HTTPS
 * request fails verification — loudly, rather than trusting anyone.
 *
 * Not consulted: the Windows "Disallowed" store. mbedTLS has no way to express
 * distrust of a certificate that would otherwise chain; Mozilla's bundle drops
 * distrusted roots itself.
 *
 * crypt32.dll is loaded at run time rather than linked: it is not in the
 * audited Windows 7 export list (lib/vyto/win/win7_exports.txt), and a static
 * import would fail the audit. Same reason vsurf.c loads dwmapi this way. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "net_ca.h"

static char g_ca_file[4096];

void vnet_ca_file_set(const char *path) {
    if (!path || !*path) { g_ca_file[0] = 0; return; }
    size_t n = strlen(path);
    if (n >= sizeof g_ca_file) { g_ca_file[0] = 0; return; }
    memcpy(g_ca_file, path, n + 1);
}

#if defined(_WIN32) && !defined(VT_NO_LIBC)
#include <windows.h>
#include <stdio.h>
#include "vyto_win.h"

/* A growing byte buffer; the blob is NUL-terminated so curl hands it to
   mbedtls_x509_crt_parse without another copy. */
typedef struct { char *p; size_t n, cap; } CaBuf;

static int cabuf_put(CaBuf *b, const void *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 65536;
        while (cap < b->n + n + 1) cap *= 2;
        char *q = realloc(b->p, cap);
        if (!q) return 0;
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
    return 1;
}

static void cabuf_pem(CaBuf *b, const unsigned char *der, size_t len) {
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char line[80];
    int col = 0;
    cabuf_put(b, "-----BEGIN CERTIFICATE-----\n", 28);
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = (unsigned)der[i] << 16;
        if (i + 1 < len) v |= (unsigned)der[i + 1] << 8;
        if (i + 2 < len) v |= der[i + 2];
        line[col++] = tab[(v >> 18) & 63];
        line[col++] = tab[(v >> 12) & 63];
        line[col++] = i + 1 < len ? tab[(v >> 6) & 63] : '=';
        line[col++] = i + 2 < len ? tab[v & 63] : '=';
        if (col == 64) { line[col++] = '\n'; cabuf_put(b, line, (size_t)col); col = 0; }
    }
    if (col) { line[col++] = '\n'; cabuf_put(b, line, (size_t)col); }
    cabuf_put(b, "-----END CERTIFICATE-----\n", 26);
}

typedef HCERTSTORE (WINAPI *open_fn)(HCRYPTPROV_LEGACY, LPCWSTR);
typedef PCCERT_CONTEXT (WINAPI *enum_fn)(HCERTSTORE, PCCERT_CONTEXT);
typedef BOOL (WINAPI *close_fn)(HCERTSTORE, DWORD);

static void add_root_store(CaBuf *b) {
    HMODULE h = LoadLibraryW(L"crypt32.dll");
    if (!h) return;
    open_fn op = (open_fn)(void (*)(void))GetProcAddress(h, "CertOpenSystemStoreW");
    enum_fn en = (enum_fn)(void (*)(void))GetProcAddress(h, "CertEnumCertificatesInStore");
    close_fn cl = (close_fn)(void (*)(void))GetProcAddress(h, "CertCloseStore");
    if (op && en && cl) {
        HCERTSTORE st = op(0, L"ROOT");
        if (st) {
            PCCERT_CONTEXT c = NULL;
            while ((c = en(st, c)) != NULL)
                if ((c->dwCertEncodingType & X509_ASN_ENCODING) && c->cbCertEncoded)
                    cabuf_pem(b, c->pbCertEncoded, c->cbCertEncoded);
            cl(st, 0);
        }
    }
    /* crypt32 stays loaded: FreeLibrary buys nothing for a one-time read. */
}

static void add_wfile(CaBuf *b, const wchar_t *path) {
    FILE *f = _wfopen(path, L"rb");
    if (!f) return;
    char chunk[16384];
    size_t n;
    cabuf_put(b, "\n", 1);
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
        if (!cabuf_put(b, chunk, n)) break;
    fclose(f);
}

static void add_exe_dir_bundle(CaBuf *b) {
    wchar_t p[32768];
    DWORD n = GetModuleFileNameW(NULL, p, (DWORD)(sizeof p / sizeof p[0]));
    if (n == 0 || n >= sizeof p / sizeof p[0]) return;
    while (n > 0 && p[n - 1] != L'\\' && p[n - 1] != L'/') n--;
    static const wchar_t name[] = L"cacert.pem";
    if (n + sizeof name / sizeof name[0] > sizeof p / sizeof p[0]) return;
    memcpy(p + n, name, sizeof name);
    add_wfile(b, p);
}

/* Blobs are never freed: curl's NOCOPY mode keeps a pointer, and a connection
   reused later compares blob contents. A process changes its CA file a handful
   of times at most, so keeping each one costs little and is always safe. */
static CaBuf g_default;
static int g_default_built;
static CaBuf g_custom;
static char g_custom_for[sizeof g_ca_file];

static void set_blob(CURL *curl, const CaBuf *b) {
    if (!b->n) return; /* nothing trusted: leave it to fail verification */
    struct curl_blob blob;
    blob.data = b->p;
    blob.len = b->n + 1; /* include the NUL: curl then skips its own copy */
    blob.flags = CURL_BLOB_NOCOPY;
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
}

void vnet_ca_apply(CURL *curl) {
    if (g_ca_file[0]) {
        if (strcmp(g_custom_for, g_ca_file) != 0) {
            CaBuf nb = {0};
            VtWBuf w;
            if (vt_wbuf_from(&w, g_ca_file)) add_wfile(&nb, w.p);
            vt_wbuf_free(&w);
            g_custom = nb; /* the old one is deliberately kept, see above */
            memcpy(g_custom_for, g_ca_file, sizeof g_ca_file);
        }
        set_blob(curl, &g_custom);
        return;
    }
    if (!g_default_built) {
        add_root_store(&g_default);
        add_exe_dir_bundle(&g_default);
        g_default_built = 1;
    }
    set_blob(curl, &g_default);
}

#else

void vnet_ca_apply(CURL *curl) {
    if (g_ca_file[0]) curl_easy_setopt(curl, CURLOPT_CAINFO, g_ca_file);
}

#endif
