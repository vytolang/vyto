/* volt_rt_arr.c — array builtin-method runtime helpers.
 *
 * Amalgamated into volt_rt.c; shares its host hooks, vt_arr_* primitives and
 * vt_snprintf. Element equality is directed by the checker via a VT_EQ_* kind
 * (the runtime only knows elem_size/elem_ref, not string-vs-object refs). */

#include "volt_rt.h"

/* nth with Python-style negative indexing (-1 == last); bounds-checked. */
void *vt_arr_nth(VtArray *a, int64_t i, const char *file, int line) {
    if (!a) vt_panic_c(file, line, "index into null array");
    return vt_arr_at(a, i < 0 ? a->len + i : i, file, line);
}

/* element compare directed by the checker's eq kind: floats compare by value
 * (so -0.0 == 0.0 and NaN never matches, same as `==`), strings by content,
 * everything else by bytes. */
static bool arr_elem_eq(const void *slot, const void *elem, int eq, int32_t es) {
    switch (eq) {
    case VT_EQ_STR: return vt_str_eq(*(VtString *const *)slot, *(VtString *const *)elem);
    case VT_EQ_F64: return *(const double *)slot == *(const double *)elem;
    case VT_EQ_F32: return *(const float *)slot == *(const float *)elem;
    default: return memcmp(slot, elem, (size_t)es) == 0;
    }
}

int64_t vt_arr_index_of(VtArray *a, const void *elem, int eq, const char *file, int line) {
    if (!a) vt_panic_c(file, line, "index_of on null array");
    int32_t es = a->elem_size;
    for (int64_t i = 0; i < a->len; i++)
        if (arr_elem_eq(a->data + i * es, elem, eq, es)) return i;
    return -1;
}
bool vt_arr_contains(VtArray *a, const void *elem, int eq, const char *file, int line) {
    if (!a) vt_panic_c(file, line, "contains on null array");
    return vt_arr_index_of(a, elem, eq, file, line) >= 0;
}

void vt_arr_reverse(VtArray *a, const char *file, int line) {
    if (!a) vt_panic_c(file, line, "reverse on null array");
    if (a->len < 2) return;
    int32_t es = a->elem_size;
    char *tmp = vt_host_alloc((size_t)es);
    if (!tmp) vt_oom();
    for (int64_t i = 0, j = a->len - 1; i < j; i++, j--) {
        char *pi = a->data + i * es, *pj = a->data + j * es;
        memcpy(tmp, pi, (size_t)es);
        memcpy(pi, pj, (size_t)es);
        memcpy(pj, tmp, (size_t)es);
    }
    vt_host_free(tmp);
}

void vt_arr_clear(VtArray *a, const char *file, int line) {
    if (!a) vt_panic_c(file, line, "clear on null array");
    if (a->elem_ref)
        for (int64_t i = 0; i < a->len; i++) vt_release(*(void **)(a->data + i * a->elem_size));
    a->len = 0;
}

void vt_arr_insert(VtArray *a, int64_t i, const void *elem, const char *file, int line) {
    if (!a) vt_panic_c(file, line, "insert into null array");
    if (i < 0 || i > a->len) vt_panic_c(file, line, "insert index out of bounds");
    int32_t es = a->elem_size;
    if (a->len == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->data = vt_host_realloc(a->data, (size_t)(a->cap * es));
        if (!a->data) vt_oom();
    }
    memmove(a->data + (i + 1) * es, a->data + i * es, (size_t)((a->len - i) * es));
    memcpy(a->data + i * es, elem, (size_t)es);
    if (a->elem_ref) vt_retain(*(void **)elem);
    a->len++;
}

/* removes and returns (via out) the element at i; ownership transfers to the
 * caller (no release), mirroring vt_arr_pop. */
void vt_arr_remove_at(VtArray *a, int64_t i, void *out, const char *file, int line) {
    if (!a) vt_panic_c(file, line, "remove from null array");
    if (i < 0 || i >= a->len) vt_panic_c(file, line, "remove_at index out of bounds");
    int32_t es = a->elem_size;
    memcpy(out, a->data + i * es, (size_t)es);
    memmove(a->data + i * es, a->data + (i + 1) * es, (size_t)((a->len - 1 - i) * es));
    a->len--;
}

void vt_arr_extend(VtArray *a, VtArray *o, const char *file, int line) {
    if (!a || !o) vt_panic_c(file, line, "extend on null array");
    /* snapshot the length: a.extend(a) must not chase its own growth. Reading
     * o->data inside the loop is deliberate — when a == o, push may realloc the
     * shared buffer and the first n elements land intact in the new block. */
    int64_t n = o->len;
    for (int64_t i = 0; i < n; i++) vt_arr_push(a, o->data + i * o->elem_size);
}

VtArray *vt_arr_concat(VtArray *a, VtArray *o, const char *file, int line) {
    if (!a || !o) vt_panic_c(file, line, "concat on null array");
    VtArray *r = vt_arr_new(a->elem_size, a->elem_ref);
    for (int64_t i = 0; i < a->len; i++) vt_arr_push(r, a->data + i * a->elem_size);
    for (int64_t i = 0; i < o->len; i++) vt_arr_push(r, o->data + i * o->elem_size);
    return r;
}

VtArray *vt_arr_slice(VtArray *a, int64_t lo, int64_t hi, const char *file, int line) {
    if (!a) vt_panic_c(file, line, "slice of null array");
    if (lo < 0 || hi < lo || hi > a->len) {
        char buf[96];
        vt_snprintf(buf, sizeof buf, "slice [%lld, %lld) out of bounds (len %lld)", (long long)lo,
                    (long long)hi, (long long)a->len);
        vt_panic_c(file, line, buf);
    }
    VtArray *r = vt_arr_new(a->elem_size, a->elem_ref);
    for (int64_t i = lo; i < hi; i++) vt_arr_push(r, a->data + i * a->elem_size);
    return r;
}

void vt_arr_fill(VtArray *a, const void *elem, const char *file, int line) {
    if (!a) vt_panic_c(file, line, "fill on null array");
    int32_t es = a->elem_size;
    for (int64_t i = 0; i < a->len; i++) {
        void *slot = a->data + i * es;
        if (a->elem_ref) {
            vt_retain(*(void **)elem);
            vt_release(*(void **)slot);
        }
        memcpy(slot, elem, (size_t)es);
    }
}

/* join a string[] with `sep`. Assumes elements are VtString* (checker enforces). */
VtString *vt_arr_join(VtArray *a, VtString *sep, const char *file, int line) {
    if (!a) vt_panic_c(file, line, "join on null array");
    if (a->len == 0) return vt_str_new("", 0);
    int64_t total = sep->len * (a->len - 1);
    for (int64_t i = 0; i < a->len; i++)
        total += (*(VtString **)(a->data + i * a->elem_size))->len;
    char *buf = vt_host_alloc((size_t)(total ? total : 1));
    if (!buf) vt_oom();
    int64_t di = 0;
    for (int64_t i = 0; i < a->len; i++) {
        if (i && sep->len) { memcpy(buf + di, sep->data, (size_t)sep->len); di += sep->len; }
        VtString *s = *(VtString **)(a->data + i * a->elem_size);
        memcpy(buf + di, s->data, (size_t)s->len);
        di += s->len;
    }
    VtString *r = vt_str_new(buf, total);
    vt_host_free(buf);
    return r;
}
