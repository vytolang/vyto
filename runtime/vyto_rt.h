/* Vyto runtime library — reference counting, strings, arrays, maps, closures. */
#ifndef VYTO_RT_H
#define VYTO_RT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef VT_NO_LIBC
/* Freestanding: no <string.h>. The runtime defines these (see vyto_rt.c);
   declare them here so header-inline helpers and emitted code can call them. */
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
#else
#include <string.h>
#endif

#include "vyto_host.h"
#include "vyto_vfs.h"

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

/* ---- regions: bump arena for escape-proven-local objects (Phase 2) --------
   Objects allocated from a region carry a negative rc sentinel, so vt_retain /
   vt_release treat them as immortal (no-op) and never free them individually;
   the whole arena is dropped in one vt_region_close. Only emitted when the
   compiler proves an allocation cannot escape its scope (the --regions path) —
   default codegen never references these. MVP restriction: region objects hold
   no external (RC) references, so close needs no per-object deinit walk. */
#define VT_RC_REGION (-2) /* distinct from immortal (-1); still < 0 => RC no-op */
typedef struct VtRegionChunk VtRegionChunk;
typedef struct VtRegion { char *bump, *end; VtRegionChunk *chunks; } VtRegion;
void vt_region_open(VtRegion *r);
void *vt_ralloc(VtRegion *r, size_t size, const VtType *type); /* zeroed, rc sentinel */
void vt_region_close(VtRegion *r); /* frees the whole arena; no per-object deinit */

static inline void *vt_retain(void *p) {
    if (p) {
        VtObj *o = (VtObj *)p;
        if (o->rc >= 0)
#ifdef VT_ATOMIC_RC
            /* Multicore-forward seam: relaxed is sufficient for a retain — it
               only guards the count, not the pointee's contents. */
            __atomic_fetch_add(&o->rc, 1, __ATOMIC_RELAXED);
#else
            o->rc++;
#endif
    }
    return p;
}

/* release + null in one expression; x must be an lvalue */
#define VT_RELEASE(x) (vt_release(x), (x) = 0)

/* Auto-zeroing weak references. vt_weak_set assigns a weak field and registers
   its slot so the target's free nulls it; vt_weak_drop unregisters a slot when
   its owning object is torn down. Both take the slot's address. */
void vt_weak_set(void **slot, void *target);
void vt_weak_drop(void **slot);

bool vt_isa(const void *p, const VtType *t);
void *vt_checked_cast(void *p, const VtType *t, const char *file, int line);

/* ---- command-line arguments ---- */
void vt_set_args(int argc, char **argv); /* stashed by main() */
struct VtArray *vt_args(void);            /* args as string[], excluding argv[0] */

/* ---- panic ---- */
VT_NORETURN void vt_panic_c(const char *file, int line, const char *msg);

/* ---- checked signed integer arithmetic ----
   Emitted for signed int/i8/i16/i32/i64 arithmetic in checked (debug) builds;
   `lo`/`hi` are the target type's range. Panics on overflow instead of
   wrapping silently. Release builds (--release) emit the raw operator. */
int64_t vt_ck_add(int64_t a, int64_t b, int64_t lo, int64_t hi, const char *file, int line);
int64_t vt_ck_sub(int64_t a, int64_t b, int64_t lo, int64_t hi, const char *file, int line);
int64_t vt_ck_mul(int64_t a, int64_t b, int64_t lo, int64_t hi, const char *file, int line);
int64_t vt_ck_neg(int64_t a, int64_t lo, int64_t hi, const char *file, int line);

/* Checked divide/modulo: panic on a zero divisor (SIGFPE otherwise) and on the
   INT_MIN/-1 overflow (C UB). Unsigned variants only guard the zero divisor. */
int64_t vt_ck_div(int64_t a, int64_t b, int64_t lo, int64_t hi, const char *file, int line);
int64_t vt_ck_mod(int64_t a, int64_t b, const char *file, int line);
uint64_t vt_ck_udiv(uint64_t a, uint64_t b, const char *file, int line);
uint64_t vt_ck_umod(uint64_t a, uint64_t b, const char *file, int line);

/* Checked shifts: panic when the shift amount is negative or >= the operand
   width (C UB otherwise). vt_ck_shr is an arithmetic (signed) shift,
   vt_ck_shru a logical (unsigned) one. */
int64_t vt_ck_shl(int64_t a, int64_t b, int bits, const char *file, int line);
int64_t vt_ck_shr(int64_t a, int64_t b, int bits, const char *file, int line);
uint64_t vt_ck_shru(uint64_t a, int64_t b, int bits, const char *file, int line);

/* Strings, arrays, and maps all start with { VtObj hdr; int64_t len; } —
   vt_len is the shared, null-checked `.len` accessor. */
typedef struct VtLenHdr {
    VtObj hdr;
    int64_t len;
} VtLenHdr;

static inline int64_t vt_len(const void *p, const char *file, int line) {
    if (!p) vt_panic_c(file, line, ".len of null value");
    return ((const VtLenHdr *)p)->len;
}

/* Null-checked receiver for a builtin method call. The string/array/map helpers
 * dereference their receiver immediately (`s->len`, `a->data`), so a null one
 * faults inside the runtime with nothing naming the call site. Wrapping the
 * receiver at the CALL SITE keeps the check in one place instead of threading
 * file/line through two dozen helper signatures, and — unlike the debug-only
 * vt_ck_ptr — this one stays in release builds, because a null receiver here is
 * a program bug at any optimisation level. */
VT_NORETURN void vt_null_recv(const char *what, const char *file, int line);
static inline void *vt_ck_recv(void *p, const char *what, const char *file, int line) {
    if (!p) vt_null_recv(what, file, line);
    return p;
}

/* ---- strings (immutable) ---- */

typedef struct VtString {
    VtObj hdr;
    int64_t len;
    char data[]; /* always NUL-terminated */
} VtString;

VtString *vt_str_new(const char *bytes, int64_t len);      /* fresh, rc=1 */
VtString *vt_str_immortal(const char *bytes, int64_t len); /* interned literal */
VtString *vt_str_concat(VtString *a, VtString *b);
VtString *vt_str_concat_int(VtString *a, int64_t v); /* fused: concat(a, from_int(v)), one alloc */
VtString *vt_str_join(int n, VtString **parts);      /* n parts, one alloc; parts are borrowed */
int64_t vt_ck_enum(int64_t v, const char *ename, const char *file, int line);
bool vt_str_eq(VtString *a, VtString *b);
VtString *vt_str_from_int(int64_t v);
VtString *vt_str_from_float(double v);
VtString *vt_str_from_bool(bool v);
VtString *vt_str_from_cstr(const char *p); /* NULL → empty string */
VtString *vt_str_slice(VtString *s, int64_t lo, int64_t hi, const char *file, int line);
int64_t vt_str_index(VtString *s, int64_t i, const char *file, int line);
static inline const char *vt_str_cstr(VtString *s) { return s ? s->data : ""; }

void vt_print(VtString *s);
VT_NORETURN void vt_panic(const char *file, int line, VtString *msg);

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
    bool readonly;  /* view over pages we may not write; see vt_arr_view */
    char *data;
    VtObj *owner;   /* NULL: data is owned, free it on deinit. non-NULL: data is
                       borrowed from this retained object, which outlives the
                       view. Mirrors VtClosure.env below. */
} VtArray;

/* Comparator for vt_arr_msort: <0 if x sorts before y, 0 if equal, >0 after. */
typedef int64_t (*VtCmpFn)(void *env, const void *x, const void *y);

/* Stable O(n log n) sort of an array's elements in place. */
void vt_arr_msort(VtArray *a, VtCmpFn cmp, void *env);

/* Lexicographic comparison of two byte ranges without materialising them. */
int64_t vt_byte_comp(VtString *a, int64_t alo, int64_t ahi,
                     VtString *b, int64_t blo, int64_t bhi);

VtArray *vt_arr_new(int32_t elem_size, bool elem_ref);
VtArray *vt_arr_bytes(int64_t n); /* zeroed byte buffer, len = n */

/* A fixed-length array whose buffer belongs to `owner` rather than to the
   runtime -- an mmap'd file, an embedded asset, any region with a lifetime of
   its own. `owner` is retained, so the lender cannot be destroyed while the
   view is alive; deinit releases it instead of freeing `data`.
      - The view is FIXED LENGTH: push/pop/insert/remove_at/extend/reserve/clear
        panic on it, because there is nothing to realloc.
      - elem_ref is always false. A borrowed region cannot hold VtObj*, and
        pretending otherwise would make deinit release file bytes as pointers.
      - readonly != 0 makes element writes panic instead of faulting.
   This is deliberately reachable from an `extern "C"` block, which means Vyto
   code can forge an array over arbitrary memory. That is not a new kind of hole
   -- an extern block can already declare vt_host_free -- but it is the sharpest
   one, so it is spelled out here rather than discovered. */
VtArray *vt_arr_view(void *data, int64_t len, int32_t elem_size, void *owner, int readonly);
/* Guards for the two things a view forbids. Both are no-ops on a normal array. */
void vt_arr_no_resize(VtArray *a, const char *op, const char *file, int line);
void vt_arr_no_write(VtArray *a, const char *file, int line);
static inline void *vt_arr_data(VtArray *a) { return a ? a->data : NULL; }
void vt_arr_push(VtArray *a, const void *elem);                        /* retains if ref */
void vt_arr_push_at(VtArray *a, const void *elem, const char *file, int line); /* null-checked push */
void vt_arr_pop(VtArray *a, void *out, const char *file, int line);    /* transfers ownership */
/* Bounds/null failure path for vt_arr_at — cold, out-of-line, never returns. */
VT_NORETURN void vt_arr_oob(VtArray *a, int64_t i, const char *file, int line);
/* Bounds-checked element slot. Hot path is a single unsigned compare (folds the
   i<0 and i>=len cases) and inlines at the call site — like C++'s
   vector::operator[]; the panic path stays out-of-line so the loop body stays
   tight and the C compiler can hoist/vectorize around it. */
static inline void *vt_arr_at(VtArray *a, int64_t i, const char *file, int line) {
    if (!a || (uint64_t)i >= (uint64_t)a->len) vt_arr_oob(a, i, file, line);
    return a->data + i * a->elem_size;
}
/* Read-only-view failure path — cold, out-of-line, never returns. */
VT_NORETURN void vt_arr_ro(const char *file, int line);

/* Null-dereference failure path — cold, out-of-line, never returns. */
VT_NORETURN void vt_null_deref(const char *what, const char *file, int line);
/* Debug-build null guard on a reference about to be dereferenced. Same shape as
 * vt_arr_ati: hot path is one predicted-not-taken compare, the panic stays
 * out-of-line, and the emitter drops the call entirely under --release.
 *
 * The checker refuses most null dereferences statically, but two holes are
 * deliberate and documented (docs/memory.md §3): a narrowing survives a call
 * that could null the field, and a possibly-null reference may be passed along.
 * This turns what those produce — a bare segfault at 0x8 inside whatever
 * function touched the field — into a panic naming the file and line, which is
 * what every other memory fault in Vyto already does. */
static inline void *vt_ck_ptr(void *p, const char *what, const char *file, int line) {
    if (!p) vt_null_deref(what, file, line);
    return p;
}
/* The same slot, for a WRITE. One extra branch on a flag that shares a cache
   line with len (already loaded for the bounds check) and is predicted
   not-taken, so a store loop still inlines and vectorizes — while a write to a
   view over read-only pages panics with a file:line instead of faulting with
   nothing. */
static inline void *vt_arr_wr(VtArray *a, int64_t i, const char *file, int line) {
    if (!a || (uint64_t)i >= (uint64_t)a->len) vt_arr_oob(a, i, file, line);
    if (a->readonly) vt_arr_ro(file, line);
    return a->data + i * a->elem_size;
}

/* The same two checks, but returning the INDEX rather than the byte address,
 * so the caller can subscript a typed pointer: `((double*)a->data)[VT_AT(...)]`.
 *
 * Identical safety, materially better code. The address form above must load
 * `elem_size` and multiply by it at runtime, because it is generic over the
 * element type. The emitter always knows the concrete C element type at the
 * access site (`c_type(et)`), so folding the scale into the subscript turns a
 * load-and-multiply into a constant shift the C compiler can fold into an
 * addressing mode -- which is also what lets it vectorize the loop.
 *
 * Measured on a 1920x1080 depth-tested fill: 2.17ms -> 1.73ms per frame, with
 * unchecked raw pointers at 1.69ms. So this reaches raw-pointer speed while
 * keeping every bounds and readonly check. See local/docs/RASTER3D-SPIKE.md.
 *
 * `a` is evaluated once by the caller (the emitter spills it to a temp), so
 * these are safe to use from a macro-free call site. */
static inline int64_t vt_arr_ati(VtArray *a, int64_t i, const char *file, int line) {
    if (!a || (uint64_t)i >= (uint64_t)a->len) vt_arr_oob(a, i, file, line);
    return i;
}
/* The checked ELEMENT POINTER, with the scale passed in by the caller so it
 * stays a compile-time constant. Exists because the obvious spelling at the
 * call site — `((T*)a->data)[vt_arr_ati(a, i, ...)]` — loads `a->data` before
 * the subscript is evaluated, so a null array faults on `->data` before
 * vt_arr_ati can panic. Folding the load in here orders the check first. */
static inline void *vt_arr_atp(VtArray *a, int64_t i, size_t scale, const char *file, int line) {
    if (!a || (uint64_t)i >= (uint64_t)a->len) vt_arr_oob(a, i, file, line);
    return a->data + (size_t)i * scale;
}
/* The same element pointer, for a WRITE — adds the read-only-view check. */
static inline void *vt_arr_wrp(VtArray *a, int64_t i, size_t scale, const char *file, int line) {
    if (!a || (uint64_t)i >= (uint64_t)a->len) vt_arr_oob(a, i, file, line);
    if (a->readonly) vt_arr_ro(file, line);
    return a->data + (size_t)i * scale;
}
static inline int64_t vt_arr_wri(VtArray *a, int64_t i, const char *file, int line) {
    if (!a || (uint64_t)i >= (uint64_t)a->len) vt_arr_oob(a, i, file, line);
    if (a->readonly) vt_arr_ro(file, line);
    return i;
}
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
void vt_map_set(VtMap *m, VtString *key, uint64_t val, const char *file, int line);
uint64_t vt_map_get(VtMap *m, VtString *key, const char *file, int line); /* borrowed */
bool vt_map_has(VtMap *m, VtString *key, const char *file, int line);
void vt_map_remove(VtMap *m, VtString *key, const char *file, int line);

static inline uint64_t vt_f64bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static inline double vt_bits2f64(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
/* f32 map values live in the slot's low 32 bits as raw float bits, so
   vt_map_values (which copies elem_size bytes) reconstitutes them exactly. */
static inline uint64_t vt_f32bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float vt_bits2f32(uint64_t u) { uint32_t v = (uint32_t)u; float f; memcpy(&f, &v, 4); return f; }

/* ---- closures ---- */

typedef struct VtClosure {
    VtObj hdr;
    void *fn;   /* RET (*)(VtObj *env, ...) */
    VtObj *env; /* owned; may be NULL */
} VtClosure;

VtClosure *vt_closure_new(void *fn, VtObj *env); /* takes ownership of env */

/* ---- int / float builtin-method helpers (runtime/vyto_rt_num.c) ---- */
int64_t vt_int_abs(int64_t x, const char *file, int line); /* traps INT64_MIN */
int64_t vt_int_min(int64_t a, int64_t b);
int64_t vt_int_max(int64_t a, int64_t b);
int64_t vt_int_clamp(int64_t x, int64_t lo, int64_t hi);
int64_t vt_int_sign(int64_t x);
int64_t vt_int_pow(int64_t base, int64_t exp, const char *file, int line); /* traps overflow */
int64_t vt_int_gcd(int64_t a, int64_t b, const char *file, int line);

double vt_flt_abs(double x);
double vt_flt_min(double a, double b);
double vt_flt_max(double a, double b);
double vt_flt_clamp(double x, double lo, double hi);
double vt_flt_floor(double x);
double vt_flt_ceil(double x);
double vt_flt_round(double x);
double vt_flt_trunc(double x);
double vt_flt_sqrt(double x);
double vt_flt_pow(double b, double e);
bool vt_flt_is_nan(double x);

/* ---- string builtin-method helpers (runtime/vyto_rt_str.c) ---- */
bool vt_str_starts_with(VtString *s, VtString *p);
bool vt_str_ends_with(VtString *s, VtString *p);
bool vt_str_contains(VtString *s, VtString *sub);
int64_t vt_str_index_of(VtString *s, VtString *sub);
int64_t vt_str_last_index_of(VtString *s, VtString *sub);
int64_t vt_str_count(VtString *s, VtString *sub);
VtString *vt_str_char_at(VtString *s, int64_t i, const char *file, int line);
VtString *vt_str_to_upper(VtString *s);
VtString *vt_str_to_lower(VtString *s);
VtString *vt_str_trim(VtString *s);
VtString *vt_str_trim_start(VtString *s);
VtString *vt_str_trim_end(VtString *s);
VtString *vt_str_repeat(VtString *s, int64_t n, const char *file, int line);
VtString *vt_str_pad_start(VtString *s, int64_t w, VtString *ch);
VtString *vt_str_pad_end(VtString *s, int64_t w, VtString *ch);
VtString *vt_str_replace(VtString *s, VtString *old, VtString *neu);
VtString *vt_str_reverse(VtString *s);
VtArray *vt_str_split(VtString *s, VtString *sep);
VtArray *vt_str_lines(VtString *s);
int64_t vt_str_to_int(VtString *s, const char *file, int line);
double vt_str_to_float(VtString *s, const char *file, int line);
double vt_str_to_float_at(VtString *s, int64_t lo, int64_t hi, const char *file, int line); /* parse [lo,hi), no alloc */

/* ---- array builtin-method helpers (runtime/vyto_rt_arr.c) ---- */
/* element-equality kind, set by the checker (floats compare by value so
   -0.0 == 0.0 and NaN never matches, same as `==`) */
enum { VT_EQ_BITS = 0, VT_EQ_STR = 1, VT_EQ_F64 = 2, VT_EQ_F32 = 3 };
/* null-check pass-through: emitted higher-order loops receive a checked array */
static inline VtArray *vt_arr_nn(VtArray *a, const char *file, int line) {
    if (!a) vt_panic_c(file, line, "method call on null array");
    return a;
}
void *vt_arr_nth(VtArray *a, int64_t i, const char *file, int line);
int64_t vt_arr_index_of(VtArray *a, const void *elem, int eq, const char *file, int line);
bool vt_arr_contains(VtArray *a, const void *elem, int eq, const char *file, int line);
void vt_arr_reverse(VtArray *a, const char *file, int line);
void vt_arr_clear(VtArray *a, const char *file, int line);
void vt_arr_reserve(VtArray *a, int64_t n, const char *file, int line); /* grow cap to >= n */
void vt_arr_insert(VtArray *a, int64_t i, const void *elem, const char *file, int line);
void vt_arr_remove_at(VtArray *a, int64_t i, void *out, const char *file, int line);
void vt_arr_extend(VtArray *a, VtArray *o, const char *file, int line);
VtArray *vt_arr_concat(VtArray *a, VtArray *o, const char *file, int line);
VtArray *vt_arr_slice(VtArray *a, int64_t lo, int64_t hi, const char *file, int line);
void vt_arr_fill(VtArray *a, const void *elem, const char *file, int line);
VtString *vt_arr_join(VtArray *a, VtString *sep, const char *file, int line);

/* ---- map builtin-method helpers (runtime/vyto_rt_map.c) ---- */
VtArray *vt_map_keys(VtMap *m, const char *file, int line);
VtArray *vt_map_values(VtMap *m, int32_t elem_size, bool elem_ref, const char *file, int line);
uint64_t vt_map_get_or(VtMap *m, VtString *key, uint64_t defbits, const char *file, int line);
void vt_map_clear(VtMap *m, const char *file, int line);
void vt_map_merge(VtMap *m, VtMap *o, const char *file, int line);

#endif
