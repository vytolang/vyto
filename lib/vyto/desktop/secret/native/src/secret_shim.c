/* vyto/desktop/secret — a small secret (an API key, a passkey) kept in a file
 * of the app's own, readable only by the user who saved it.
 *
 * File format: one header line, then the payload.
 *
 *   "VTSEC1 P\n" + the plain bytes      POSIX: the file is mode 0600
 *   "VTSEC1 D\n" + a DPAPI blob         Windows: CryptProtectData, user scope
 *
 * Windows: DPAPI ties the blob to the Windows account that saved it, so a
 * copy of the file (a backup, a stolen disk image read elsewhere) does not
 * give the secret back. crypt32.dll is loaded at run time rather than linked:
 * it is not in the audited Windows 7 export list
 * (lib/vyto/win/win7_exports.txt) — the same reason net_ca.c loads it so.
 *
 * POSIX: there is no keyring here (libsecret would pull in D-Bus and GLib for
 * one string). The protection is the file mode: created 0600 through open(),
 * never widened, and checked again by vsec_private().
 *
 * Writes go to "<path>.tmp" and are renamed over the old file, so a crash
 * mid-write leaves the previous secret, never half of one.
 *
 * Freestanding (VT_NO_LIBC): every entry point returns its failure sentinel. */
#include <stdint.h>

#if defined(VT_NO_LIBC)

int32_t vsec_protected(void) { return 0; }
int32_t vsec_save(const char *path, const char *data, int32_t len) { (void)path; (void)data; (void)len; return 0; }
const char *vsec_load(const char *path) { (void)path; return ""; }
int32_t vsec_loaded_ok(void) { return 0; }
int32_t vsec_private(const char *path) { (void)path; return 0; }
int32_t vsec_remove(const char *path) { (void)path; return 0; }

#else

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char HDR_PLAIN[] = "VTSEC1 P\n";
static const char HDR_DPAPI[] = "VTSEC1 D\n";
#define HDR_LEN 9

/* The last secret loaded: vsec_load returns it, Vyto copies it with str(). */
static char *g_loaded = NULL;
static int g_loaded_ok = 0;

static void wipe_loaded(void) {
    if (g_loaded) {
        size_t n = strlen(g_loaded);
        volatile char *p = g_loaded;
        while (n--) *p++ = 0;
        free(g_loaded);
        g_loaded = NULL;
    }
}

static int set_loaded(const char *s, size_t n) {
    wipe_loaded();
    g_loaded = malloc(n + 1);
    if (!g_loaded) return 0;
    memcpy(g_loaded, s, n);
    g_loaded[n] = 0;
    return 1;
}

/* the whole file, or NULL */
static char *read_all(FILE *f, size_t *out_n) {
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        if (n == cap) {
            if (cap > (1u << 20)) { free(buf); return NULL; }   /* a secret is small */
            cap *= 2;
            char *q = realloc(buf, cap);
            if (!q) { free(buf); return NULL; }
            buf = q;
        }
        size_t r = fread(buf + n, 1, cap - n, f);
        if (r == 0) break;
        n += r;
    }
    *out_n = n;
    return buf;
}

#if defined(_WIN32)
#include <windows.h>
#include <wincrypt.h>
#include "vyto_win.h"

typedef BOOL (WINAPI *protect_fn)(DATA_BLOB *, LPCWSTR, DATA_BLOB *, PVOID, CRYPTPROTECT_PROMPTSTRUCT *, DWORD, DATA_BLOB *);
typedef BOOL (WINAPI *unprotect_fn)(DATA_BLOB *, LPWSTR *, DATA_BLOB *, PVOID, CRYPTPROTECT_PROMPTSTRUCT *, DWORD, DATA_BLOB *);

static protect_fn g_protect = NULL;
static unprotect_fn g_unprotect = NULL;

static int load_crypt32(void) {
    static int tried = 0;
    if (!tried) {
        tried = 1;
        HMODULE h = LoadLibraryW(L"crypt32.dll");
        if (h) {
            g_protect = (protect_fn)(void (*)(void))GetProcAddress(h, "CryptProtectData");
            g_unprotect = (unprotect_fn)(void (*)(void))GetProcAddress(h, "CryptUnprotectData");
        }
    }
    return g_protect != NULL && g_unprotect != NULL;
}

int32_t vsec_protected(void) { return load_crypt32() ? 1 : 0; }

int32_t vsec_save(const char *path, const char *data, int32_t len) {
    if (!path || !*path || !data || len < 0) return 0;
    if (!load_crypt32()) return 0;
    DATA_BLOB in, out;
    in.pbData = (BYTE *)data;
    in.cbData = (DWORD)len;
    out.pbData = NULL;
    out.cbData = 0;
    if (!g_protect(&in, L"vyto secret", NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) return 0;
    size_t pn = strlen(path);
    char *tmp = malloc(pn + 5);
    if (!tmp) { LocalFree(out.pbData); return 0; }
    memcpy(tmp, path, pn);
    memcpy(tmp + pn, ".tmp", 5);
    int ok = 0;
    FILE *f = vt_fopen(tmp, "wb");
    if (f) {
        ok = fwrite(HDR_DPAPI, 1, HDR_LEN, f) == HDR_LEN &&
             fwrite(out.pbData, 1, out.cbData, f) == out.cbData;
        if (fclose(f) != 0) ok = 0;
    }
    LocalFree(out.pbData);
    if (ok) {
        VtWBuf a, b;
        ok = vt_wbuf_from(&a, tmp) && vt_wbuf_from(&b, path) &&
             MoveFileExW(a.p, b.p, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        vt_wbuf_free(&a);
        vt_wbuf_free(&b);
    }
    if (!ok) {
        VtWBuf a;
        if (vt_wbuf_from(&a, tmp)) DeleteFileW(a.p);
        vt_wbuf_free(&a);
    }
    free(tmp);
    return ok ? 1 : 0;
}

const char *vsec_load(const char *path) {
    g_loaded_ok = 0;
    wipe_loaded();
    if (!path || !*path || !load_crypt32()) return "";
    FILE *f = vt_fopen(path, "rb");
    if (!f) return "";
    size_t n = 0;
    char *buf = read_all(f, &n);
    fclose(f);
    if (!buf) return "";
    if (n < HDR_LEN || memcmp(buf, HDR_DPAPI, HDR_LEN) != 0) { free(buf); return ""; }
    DATA_BLOB in, out;
    in.pbData = (BYTE *)buf + HDR_LEN;
    in.cbData = (DWORD)(n - HDR_LEN);
    out.pbData = NULL;
    out.cbData = 0;
    int ok = g_unprotect(&in, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out) &&
             set_loaded((const char *)out.pbData, out.cbData);
    if (out.pbData) { SecureZeroMemory(out.pbData, out.cbData); LocalFree(out.pbData); }
    free(buf);
    if (!ok) return "";
    g_loaded_ok = 1;
    return g_loaded;
}

/* The user's profile already belongs to the user; DPAPI is the protection. */
int32_t vsec_private(const char *path) {
    VtWBuf p;
    int ok = vt_wbuf_from(&p, path) && GetFileAttributesW(p.p) != INVALID_FILE_ATTRIBUTES;
    vt_wbuf_free(&p);
    return ok ? 1 : 0;
}

int32_t vsec_remove(const char *path) {
    VtWBuf p;
    int ok = vt_wbuf_from(&p, path) && DeleteFileW(p.p);
    vt_wbuf_free(&p);
    return ok ? 1 : 0;
}

#else  /* POSIX */
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

int32_t vsec_protected(void) { return 0; }

int32_t vsec_save(const char *path, const char *data, int32_t len) {
    if (!path || !*path || !data || len < 0) return 0;
    size_t pn = strlen(path);
    char *tmp = malloc(pn + 5);
    if (!tmp) return 0;
    memcpy(tmp, path, pn);
    memcpy(tmp + pn, ".tmp", 5);
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_TRUNC, 0600);
    int ok = 0;
    if (fd >= 0) {
        ok = fchmod(fd, 0600) == 0 &&
             write(fd, HDR_PLAIN, HDR_LEN) == HDR_LEN &&
             write(fd, data, (size_t)len) == (ssize_t)len &&
             fsync(fd) == 0;
        if (close(fd) != 0) ok = 0;
        if (ok) ok = rename(tmp, path) == 0;
        if (!ok) unlink(tmp);
    }
    free(tmp);
    return ok ? 1 : 0;
}

const char *vsec_load(const char *path) {
    g_loaded_ok = 0;
    wipe_loaded();
    if (!path || !*path) return "";
    FILE *f = fopen(path, "rb");
    if (!f) return "";
    size_t n = 0;
    char *buf = read_all(f, &n);
    fclose(f);
    if (!buf) return "";
    int ok = n >= HDR_LEN && memcmp(buf, HDR_PLAIN, HDR_LEN) == 0 &&
             set_loaded(buf + HDR_LEN, n - HDR_LEN);
    memset(buf, 0, n);
    free(buf);
    if (!ok) return "";
    g_loaded_ok = 1;
    return g_loaded;
}

/* No one but the owner may read or write it. */
int32_t vsec_private(const char *path) {
    struct stat st;
    if (!path || stat(path, &st) != 0) return 0;
    return (st.st_mode & 077) == 0 ? 1 : 0;
}

int32_t vsec_remove(const char *path) { return path && unlink(path) == 0 ? 1 : 0; }

#endif

/* Whether the last vsec_load found a secret: tells "" stored apart from
   "nothing there" (missing, damaged, saved by another Windows account). */
int32_t vsec_loaded_ok(void) { return g_loaded_ok; }

#endif
