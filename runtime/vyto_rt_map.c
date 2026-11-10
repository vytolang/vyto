/* vyto_rt_map.c — map builtin-method runtime helpers.
 *
 * Amalgamated into vyto_rt.c; shares its host hooks and vt_map_* primitives.
 * Values are stored in 8-byte slots (uint64_t); pushing &e->val into an array
 * of elem_size copies the low bytes (little-endian) — correct for the sized
 * scalar/ref value types the checker permits.
 *
 * Null maps panic with a source location, matching vt_map_set/get/has/remove. */

#include "vyto_rt.h"

/* string[] of the map's keys, in insertion order. Sized once: len is exact. */
VtArray *vt_map_keys(VtMap *m, const char *file, int line) {
    if (!m) vt_panic_c(file, line, "keys on null map");
    VtArray *a = vt_arr_new(sizeof(VtString *), true);
    if (m->len) vt_arr_reserve(a, m->len, file, line);
    for (int64_t i = 0; i < m->nent; i++)
        if (m->ents[i].key) vt_arr_push(a, &m->ents[i].key); /* push retains the key */
    return a;
}

/* T[] of the map's values, in insertion order. elem_size/elem_ref come from the
 * checker so the result array matches the map's value type; push retains ref
 * values. */
VtArray *vt_map_values(VtMap *m, int32_t elem_size, bool elem_ref, const char *file, int line) {
    if (!m) vt_panic_c(file, line, "values on null map");
    VtArray *a = vt_arr_new(elem_size, elem_ref);
    if (m->len) vt_arr_reserve(a, m->len, file, line);
    for (int64_t i = 0; i < m->nent; i++)
        if (m->ents[i].key) vt_arr_push(a, &m->ents[i].val);
    return a;
}

/* value for key, or `defbits` when absent (no panic on a missing key — that is
 * the point of the method; a null map still panics). Borrowed, like vt_map_get. */
uint64_t vt_map_get_or(VtMap *m, VtString *key, uint64_t defbits, const char *file, int line) {
    if (!m) vt_panic_c(file, line, "get_or on null map");
    int64_t ei = map_lookup(m, key);
    return ei >= 0 ? m->ents[ei].val : defbits;
}

/* Drop every entry but keep both buffers, so a map refilled in a loop does not
 * reallocate — the same contract as StringBuilder.clear(). */
void vt_map_clear(VtMap *m, const char *file, int line) {
    if (!m) vt_panic_c(file, line, "clear on null map");
    for (int64_t i = 0; i < m->nent; i++) {
        VtMapEntry *e = &m->ents[i];
        if (!e->key) continue;
        vt_release(e->key);
        if (m->val_ref) vt_release((void *)(uintptr_t)e->val);
    }
    m->nent = 0;
    m->len = 0;
    if (m->index) memset(m->index, 0, (size_t)(m->mask + 1) * sizeof *m->index);
}

/* copy every entry of `o` into `m`, in `o`'s insertion order (overwriting on
 * key collision, which keeps the key's existing position in `m`); vt_map_set
 * handles key/value retains. m.merge(m) only overwrites existing keys, so no
 * rebuild can move the entries under the walk. */
void vt_map_merge(VtMap *m, VtMap *o, const char *file, int line) {
    if (!m || !o) vt_panic_c(file, line, "merge on null map");
    for (int64_t i = 0; i < o->nent; i++)
        if (o->ents[i].key) vt_map_set(m, o->ents[i].key, o->ents[i].val, file, line);
}
