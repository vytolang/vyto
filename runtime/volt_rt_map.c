/* volt_rt_map.c — map builtin-method runtime helpers.
 *
 * Amalgamated into volt_rt.c; shares its host hooks and vt_map_* primitives.
 * Values are stored in 8-byte slots (uint64_t); pushing &e->val into an array
 * of elem_size copies the low bytes (little-endian) — correct for the sized
 * scalar/ref value types the checker permits.
 *
 * Null maps panic with a source location, matching vt_map_set/get/has/remove. */

#include "volt_rt.h"

/* string[] of the map's keys, in bucket order. */
VtArray *vt_map_keys(VtMap *m, const char *file, int line) {
    if (!m) vt_panic_c(file, line, "keys on null map");
    VtArray *a = vt_arr_new(sizeof(VtString *), true);
    for (int64_t b = 0; b < m->nbuckets; b++)
        for (VtMapEntry *e = m->buckets[b]; e; e = e->next)
            vt_arr_push(a, &e->key); /* push retains the key */
    return a;
}

/* T[] of the map's values. elem_size/elem_ref come from the checker so the
 * result array matches the map's value type; push retains ref values. */
VtArray *vt_map_values(VtMap *m, int32_t elem_size, bool elem_ref, const char *file, int line) {
    if (!m) vt_panic_c(file, line, "values on null map");
    VtArray *a = vt_arr_new(elem_size, elem_ref);
    for (int64_t b = 0; b < m->nbuckets; b++)
        for (VtMapEntry *e = m->buckets[b]; e; e = e->next)
            vt_arr_push(a, &e->val);
    return a;
}

/* value for key, or `defbits` when absent (no panic on a missing key — that is
 * the point of the method; a null map still panics). Borrowed, like vt_map_get. */
uint64_t vt_map_get_or(VtMap *m, VtString *key, uint64_t defbits, const char *file, int line) {
    if (!m) vt_panic_c(file, line, "get_or on null map");
    VtMapEntry **pe = map_slot(m, key);
    return *pe ? (*pe)->val : defbits;
}

void vt_map_clear(VtMap *m, const char *file, int line) {
    if (!m) vt_panic_c(file, line, "clear on null map");
    for (int64_t b = 0; b < m->nbuckets; b++) {
        VtMapEntry *e = m->buckets[b];
        while (e) {
            VtMapEntry *next = e->next;
            vt_release(e->key);
            if (m->val_ref) vt_release((void *)(uintptr_t)e->val);
            vt_host_free(e);
            e = next;
        }
        m->buckets[b] = NULL;
    }
    m->len = 0;
}

/* copy every entry of `o` into `m` (overwriting on key collision); vt_map_set
 * handles key/value retains. m.merge(m) is a no-op-safe overwrite of existing
 * keys: no new entries, so no rehash invalidates the walk. */
void vt_map_merge(VtMap *m, VtMap *o, const char *file, int line) {
    if (!m || !o) vt_panic_c(file, line, "merge on null map");
    for (int64_t b = 0; b < o->nbuckets; b++)
        for (VtMapEntry *e = o->buckets[b]; e; e = e->next)
            vt_map_set(m, e->key, e->val, file, line);
}
