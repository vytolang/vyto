/* Volt runtime library — reference counting, strings, arrays, maps, closures. */
#ifndef VOLT_RT_H
#define VOLT_RT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* extern "C" declarations are emitted under private identifiers aliased to the
   real symbol with __asm__, so they can never conflict with system headers.
   Mach-O and 32-bit Windows prefix C symbols with an underscore. */
#if defined(__APPLE__) || (defined(_WIN32) && !defined(_WIN64))
#define VT_SYM(s) "_" s
#else
#define VT_SYM(s) s
#endif

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

/* ---- checked signed integer arithmetic ----
   Emitted for signed int/i8/i16/i32/i64 arithmetic in checked (debug) builds;
   `lo`/`hi` are the target type's range. Panics on overflow instead of
   wrapping silently. Release builds (--release) emit the raw operator. */
int64_t vt_ck_add(int64_t a, int64_t b, int64_t lo, int64_t hi, const char *file, int line);
int64_t vt_ck_sub(int64_t a, int64_t b, int64_t lo, int64_t hi, const char *file, int line);
int64_t vt_ck_mul(int64_t a, int64_t b, int64_t lo, int64_t hi, const char *file, int line);
int64_t vt_ck_neg(int64_t a, int64_t lo, int64_t hi, const char *file, int line);

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

/* ---- file I/O ---- */
VtString *vt_file_read(VtString *path, const char *file, int line); /* panics if unreadable */
bool vt_file_write(VtString *path, VtString *data, bool append);
struct VtArray *vt_file_lines(VtString *path, const char *file, int line); /* string[] */
struct VtArray *vt_dir_list(VtString *path, const char *file, int line);   /* string[], sorted */
bool vt_is_dir(VtString *path);

/* ---- dynamic arrays ---- */

typedef struct VtArray {
    VtObj hdr;
    int64_t len, cap;
    int32_t elem_size;
    bool elem_ref;
    char *data;
} VtArray;

VtArray *vt_arr_new(int32_t elem_size, bool elem_ref);
VtArray *vt_arr_bytes(int64_t n); /* zeroed byte buffer, len = n */
static inline void *vt_arr_data(VtArray *a) { return a ? a->data : NULL; }
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
