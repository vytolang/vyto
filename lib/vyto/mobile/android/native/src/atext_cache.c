/* atext_cache.c — string interning and the text-measurement memo.
 *
 * Pure C, no JNI. The caller does the Paint.measureText round trip on a miss
 * and inserts the answer, which keeps the one expensive path visible at the
 * call site rather than buried inside a lookup.
 *
 * STATUS: written, never compiled — no NDK toolchain in this clone. The body
 * is wrapped in #ifdef __ANDROID__, so it is empty on every other target.
 * Design of record: local/docs/ANDROID.md.
 *
 * -- why this file exists separately ------------------------------------------
 *
 * text_width is a synchronous query on the hot layout path, called thousands
 * of times per layout (Painter.text_width, ui/core.vt). It cannot be batched
 * into the per-frame command buffer, so every miss is a blocking JNI round
 * trip. This cache is what stands between that and a frame budget, and it is
 * one of the two prototypes ANDROID.md says to build before anything else —
 * split out so it can be measured on its own.
 *
 * Both tables are open-addressed with linear probing and never delete. UI text
 * is a small, highly repetitive set (labels, numbers, menu items), so the
 * working set is bounded in practice and the tables converge after a few
 * frames. If an app generates unbounded distinct strings — a log viewer, a
 * chat scrollback — the width table grows without limit; that is a real
 * scenario and the eviction story is deferred, not solved. Interning is
 * separate precisely so the id space can stay small even if widths get evicted.
 */
/* vytoc globs native/src/*.c flat and compiles every file for every
 * target, with no per-file platform filter, so an Android-only source
 * must exclude its own body. Same pattern as vsurf_android.c. */
#ifdef __ANDROID__

#include "vyto_android.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- hashing */

static uint32_t hash_str(const char *s) {
    /* FNV-1a */
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

static uint32_t hash_u64(uint64_t k) {
    k ^= k >> 33; k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return (uint32_t)k;
}

/* Size and weight fold into one key. Sizes come from a small type scale, so
 * quantising to 1/16 px loses nothing real and keeps the key dense. */
static uint64_t metric_key(double size, int weight) {
    int64_t q = (int64_t)(size * 16.0 + 0.5);
    return ((uint64_t)q << 8) | (uint64_t)(weight & 0xFF);
}

static uint64_t width_key(int32_t id, double size, int weight) {
    int64_t q = (int64_t)(size * 16.0 + 0.5);
    return ((uint64_t)(uint32_t)id << 32) | ((uint64_t)(q & 0xFFFFFF) << 8)
           | (uint64_t)(weight & 0xFF);
}

/* ----------------------------------------------------------------- tables */

typedef struct {
    char    *key;    /* owned; NULL = empty slot */
    int32_t  id;
} InternSlot;

typedef struct {
    uint64_t key;    /* 0 = empty slot */
    double   value;
} WidthSlot;

typedef struct {
    uint64_t key;    /* 0 = empty */
    double   ascent;
    double   height;
} MetricSlot;

struct VtaText {
    InternSlot *intern;
    size_t      intern_cap;
    size_t      intern_len;
    int32_t     next_id;

    WidthSlot  *widths;
    size_t      widths_cap;
    size_t      widths_len;

    MetricSlot  metrics[64];   /* fixed: the type scale is tiny */
};

#define INTERN_CAP0 256
#define WIDTH_CAP0  1024

VtaText *vta_text_new(void) {
    VtaText *t = (VtaText *)calloc(1, sizeof(VtaText));
    if (!t) return NULL;
    t->intern = (InternSlot *)calloc(INTERN_CAP0, sizeof(InternSlot));
    t->widths = (WidthSlot *)calloc(WIDTH_CAP0, sizeof(WidthSlot));
    if (!t->intern || !t->widths) {
        free(t->intern); free(t->widths); free(t);
        return NULL;
    }
    t->intern_cap = INTERN_CAP0;
    t->widths_cap = WIDTH_CAP0;
    /* ids start at 1 so 0 can mean "not interned" on the Vyto side. */
    t->next_id = 1;
    return t;
}

void vta_text_free(VtaText *t) {
    if (!t) return;
    for (size_t i = 0; i < t->intern_cap; i++) free(t->intern[i].key);
    free(t->intern);
    free(t->widths);
    free(t);
}

/* ---------------------------------------------------------------- interning */

static int intern_grow(VtaText *t) {
    size_t ncap = t->intern_cap * 2;
    InternSlot *n = (InternSlot *)calloc(ncap, sizeof(InternSlot));
    if (!n) return 0;
    for (size_t i = 0; i < t->intern_cap; i++) {
        if (!t->intern[i].key) continue;
        size_t j = hash_str(t->intern[i].key) & (ncap - 1);
        while (n[j].key) j = (j + 1) & (ncap - 1);
        n[j] = t->intern[i];
    }
    free(t->intern);
    t->intern = n;
    t->intern_cap = ncap;
    return 1;
}

int32_t vta_text_intern(VtaText *t, const char *s, int *out_is_new) {
    if (out_is_new) *out_is_new = 0;
    if (!t || !s) return 0;

    if ((t->intern_len + 1) * 4 >= t->intern_cap * 3) {  /* load factor 0.75 */
        if (!intern_grow(t)) return 0;
    }

    size_t mask = t->intern_cap - 1;
    size_t i = hash_str(s) & mask;
    while (t->intern[i].key) {
        if (strcmp(t->intern[i].key, s) == 0) return t->intern[i].id;
        i = (i + 1) & mask;
    }

    char *copy = strdup(s);
    if (!copy) return 0;
    t->intern[i].key = copy;
    t->intern[i].id = t->next_id++;
    t->intern_len++;
    if (out_is_new) *out_is_new = 1;
    return t->intern[i].id;
}

/* ------------------------------------------------------------------ widths */

static int widths_grow(VtaText *t) {
    size_t ncap = t->widths_cap * 2;
    WidthSlot *n = (WidthSlot *)calloc(ncap, sizeof(WidthSlot));
    if (!n) return 0;
    for (size_t i = 0; i < t->widths_cap; i++) {
        if (!t->widths[i].key) continue;
        size_t j = hash_u64(t->widths[i].key) & (ncap - 1);
        while (n[j].key) j = (j + 1) & (ncap - 1);
        n[j] = t->widths[i];
    }
    free(t->widths);
    t->widths = n;
    t->widths_cap = ncap;
    return 1;
}

double vta_text_get_width(VtaText *t, int32_t id, double size, int weight) {
    if (!t || id <= 0) return -1.0;
    uint64_t k = width_key(id, size, weight);
    size_t mask = t->widths_cap - 1;
    size_t i = hash_u64(k) & mask;
    while (t->widths[i].key) {
        if (t->widths[i].key == k) return t->widths[i].value;
        i = (i + 1) & mask;
    }
    return -1.0;
}

void vta_text_put_width(VtaText *t, int32_t id, double size, int weight, double w) {
    if (!t || id <= 0) return;
    if ((t->widths_len + 1) * 4 >= t->widths_cap * 3) {
        if (!widths_grow(t)) return;
    }
    uint64_t k = width_key(id, size, weight);
    size_t mask = t->widths_cap - 1;
    size_t i = hash_u64(k) & mask;
    while (t->widths[i].key) {
        if (t->widths[i].key == k) { t->widths[i].value = w; return; }
        i = (i + 1) & mask;
    }
    t->widths[i].key = k;
    t->widths[i].value = w;
    t->widths_len++;
}

/* ----------------------------------------------------------------- metrics */

int vta_text_get_metrics(VtaText *t, double size, int weight,
                         double *out_ascent, double *out_height) {
    if (!t) return 0;
    uint64_t k = metric_key(size, weight);
    size_t n = sizeof t->metrics / sizeof t->metrics[0];
    size_t i = hash_u64(k) & (n - 1);
    for (size_t probe = 0; probe < n; probe++) {
        if (!t->metrics[i].key) return 0;
        if (t->metrics[i].key == k) {
            if (out_ascent) *out_ascent = t->metrics[i].ascent;
            if (out_height) *out_height = t->metrics[i].height;
            return 1;
        }
        i = (i + 1) & (n - 1);
    }
    return 0;
}

void vta_text_put_metrics(VtaText *t, double size, int weight,
                          double ascent, double height) {
    if (!t) return;
    uint64_t k = metric_key(size, weight);
    size_t n = sizeof t->metrics / sizeof t->metrics[0];
    size_t i = hash_u64(k) & (n - 1);
    for (size_t probe = 0; probe < n; probe++) {
        if (!t->metrics[i].key || t->metrics[i].key == k) {
            t->metrics[i].key = k;
            t->metrics[i].ascent = ascent;
            t->metrics[i].height = height;
            return;
        }
        i = (i + 1) & (n - 1);
    }
    /* Table full: the type scale exceeded 64 distinct (size, weight) pairs,
     * which means something is generating sizes rather than picking roles.
     * Dropping the insert is correct — it degrades to an uncached round trip
     * rather than evicting something hotter. */
}

#endif /* __ANDROID__ */
