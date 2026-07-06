/* Volt runtime library — reference counting, strings, arrays, maps, closures. */
#ifndef VOLT_RT_H
#define VOLT_RT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct VtType VtType;

typedef struct VtObj {
    int64_t rc; /* negative = immortal */
    const VtType *type;
} VtObj;

struct VtType {
    const char *name;
    void (*deinit)(void *self); /* may be NULL; called once, on the most-derived type */
    const VtType *parent;
    void **vtbl;
};

void *vt_alloc(size_t size, const VtType *type); /* zeroed, rc = 1 */
void vt_release(void *p);
void vt_free_now(void *p);

static inline void *vt_retain(void *p) {
    if (p) {
        VtObj *o = (VtObj *)p;
        if (o->rc >= 0) o->rc++;
    }
    return p;
}

/* release + null in one expression; x must be an lvalue */
#define VT_RELEASE(x) (vt_release(x), (x) = 0)

bool vt_isa(const void *p, const VtType *t);
void *vt_checked_cast(void *p, const VtType *t, const char *file, int line);

/* ---- panic ---- */
void vt_panic_c(const char *file, int line, const char *msg);

/* ---- strings (immutable) ---- */

typedef struct VtString {
    VtObj hdr;
    int64_t len;
    char data[]; /* always NUL-terminated */
} VtString;

VtString *vt_str_new(const char *bytes, int64_t len);      /* fresh, rc=1 */
VtString *vt_str_immortal(const char *bytes, int64_t len); /* interned literal */
VtString *vt_str_concat(VtString *a, VtString *b);
bool vt_str_eq(VtString *a, VtString *b);
VtString *vt_str_from_int(int64_t v);
VtString *vt_str_from_float(double v);
VtString *vt_str_from_bool(bool v);
VtString *vt_str_from_cstr(const char *p); /* NULL → empty string */
VtString *vt_str_slice(VtString *s, int64_t lo, int64_t hi, const char *file, int line);
int64_t vt_str_index(VtString *s, int64_t i, const char *file, int line);
static inline const char *vt_str_cstr(VtString *s) { return s ? s->data : ""; }

void vt_print(VtString *s);
void vt_panic(const char *file, int line, VtString *msg);

/* ---- dynamic arrays ---- */

typedef struct VtArray {
    VtObj hdr;
    int64_t len, cap;
    int32_t elem_size;
    bool elem_ref;
    char *data;
} VtArray;

VtArray *vt_arr_new(int32_t elem_size, bool elem_ref);
void vt_arr_push(VtArray *a, const void *elem);                        /* retains if ref */
void vt_arr_pop(VtArray *a, void *out, const char *file, int line);    /* transfers ownership */
void *vt_arr_at(VtArray *a, int64_t i, const char *file, int line);    /* bounds-checked slot ptr */
void vt_arr_set(VtArray *a, int64_t i, const void *elem, const char *file, int line);

/* ---- maps (string keys, 8-byte value slots) ---- */

typedef struct VtMapEntry {
    VtString *key;
    uint64_t val;
    struct VtMapEntry *next;
} VtMapEntry;

typedef struct VtMap {
    VtObj hdr;
    int64_t len;
    int64_t nbuckets;
    VtMapEntry **buckets;
    bool val_ref;
} VtMap;

VtMap *vt_map_new(bool val_ref);
void vt_map_set(VtMap *m, VtString *key, uint64_t val);
uint64_t vt_map_get(VtMap *m, VtString *key, const char *file, int line); /* borrowed */
bool vt_map_has(VtMap *m, VtString *key);
void vt_map_remove(VtMap *m, VtString *key);

static inline uint64_t vt_f64bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static inline double vt_bits2f64(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }

/* ---- closures ---- */

typedef struct VtClosure {
    VtObj hdr;
    void *fn;   /* RET (*)(VtObj *env, ...) */
    VtObj *env; /* owned; may be NULL */
} VtClosure;

VtClosure *vt_closure_new(void *fn, VtObj *env); /* takes ownership of env */

#endif
