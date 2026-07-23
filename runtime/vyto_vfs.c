/* Vyto embedded-asset registry — see vyto_vfs.h. */
#include "vyto_vfs.h"
#include "vyto_host.h"   /* vt_host_realloc: freestanding-safe (no bare libc realloc) */
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *logical;
    const unsigned char *data;
    long len;
} VtVfsEntry;

static VtVfsEntry *g_vfs;
static int g_vfs_n, g_vfs_cap;

void vt_vfs_register(const char *logical, const unsigned char *data, long len) {
    if (g_vfs_n == g_vfs_cap) {
        int cap = g_vfs_cap ? g_vfs_cap * 2 : 16;
        VtVfsEntry *n = (VtVfsEntry *)vt_host_realloc(g_vfs, (size_t)cap * sizeof *n);
        if (!n) return; /* drop silently: a missing asset falls through to disk */
        g_vfs = n;
        g_vfs_cap = cap;
    }
    g_vfs[g_vfs_n].logical = logical;
    g_vfs[g_vfs_n].data = data;
    g_vfs[g_vfs_n].len = len;
    g_vfs_n++;
}

int vt_vfs_get(const char *key, const unsigned char **out, long *out_len) {
    if (!key) return 0;
    size_t kn = strlen(key);
    for (int i = 0; i < g_vfs_n; i++) {
        const char *e = g_vfs[i].logical;
        size_t en = strlen(e);
        int hit = (kn == en && memcmp(key, e, en) == 0) ||
                  (kn > en && key[kn - en - 1] == '/' &&
                   memcmp(key + kn - en, e, en) == 0);
        if (hit) {
            if (out) *out = g_vfs[i].data;
            if (out_len) *out_len = g_vfs[i].len;
            return 1;
        }
    }
    return 0;
}

int vt_vfs_has(const char *key) { return vt_vfs_get(key, NULL, NULL); }

const unsigned char *vt_vfs_ptr(const char *key) {
    const unsigned char *p = NULL;
    return vt_vfs_get(key, &p, NULL) ? p : NULL;
}

long vt_vfs_size(const char *key) {
    long n = -1;
    return vt_vfs_get(key, NULL, &n) ? n : -1;
}

long vt_vfs_read(const char *key, unsigned char *buf, long cap) {
    const unsigned char *p; long n;
    if (!vt_vfs_get(key, &p, &n)) return -1;
    if (n > cap) n = cap;
    if (n > 0) memcpy(buf, p, (size_t)n);
    return n;
}

int vt_vfs_count(void) { return g_vfs_n; }

const char *vt_vfs_key(int i) {
    return (i >= 0 && i < g_vfs_n) ? g_vfs[i].logical : NULL;
}
