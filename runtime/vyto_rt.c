#include "vyto_rt.h"
#include "vyto_host.h"

#include <stdarg.h> /* vt_snprintf; part of freestanding C */

/* VT_NO_LIBC (freestanding) implies no filesystem unless the embedder opts
   back in by defining host file hooks (a future extension). */
#if defined(VT_NO_LIBC) && !defined(VT_NO_FS)
#define VT_NO_FS
#endif

/* Hosted host hooks: thin wrappers over libc, kept here (not in vyto_host.h)
   so no stdio/stdlib type leaks into the module headers that include vyto_rt.h.
   Static, so the compiler inlines them and the hosted path stays zero-overhead
   and byte-for-byte the same as the old direct libc calls. */
#ifndef VT_NO_LIBC
#include <stdio.h>
#include <stdlib.h>
static void *vt_host_alloc(size_t n) { return calloc(1, n ? n : 1); }
static void *vt_host_realloc(void *p, size_t n) { return realloc(p, n); }
static void vt_host_free(void *p) { free(p); }
static void vt_host_write(const char *buf, size_t len) { fwrite(buf, 1, len, stdout); }
static void vt_host_write_err(const char *buf, size_t len) { fwrite(buf, 1, len, stderr); }
VT_NORETURN static void vt_host_abort(void) { exit(101); }
#endif

#ifndef VT_NO_FS
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif
#endif

/* ---- freestanding libc shims ----
   A bare toolchain provides no library. Supply the handful of mem/str routines
   the runtime and the C compiler own codegen (struct copies, array init) rely
   on. The driver compiles the runtime with -fno-builtin under VT_NO_LIBC so the
   optimizer cannot turn these loops back into self-calls. */
#ifdef VT_NO_LIBC
void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}
void *memset(void *dst, int c, size_t n) {
    unsigned char *d = dst;
    for (size_t i = 0; i < n; i++) d[i] = (unsigned char)c;
    return dst;
}
void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) { for (size_t i = 0; i < n; i++) d[i] = s[i]; }
    else if (d > s) { for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1]; }
    return dst;
}
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}
size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (!a[i]) break;
    }
    return 0;
}
#endif /* VT_NO_LIBC */

/* ---- diagnostics: byte output + abort, always via the host hooks ---- */

static void vt_puts_err(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    vt_host_write_err(s, len);
}

VT_NORETURN static void vt_oom(void) {
    static const char m[] = "vyto: out of memory\n";
    vt_host_write_err(m, sizeof m - 1);
    vt_host_abort();
}

/* ---- minimal formatter ----
   Replaces libc snprintf across the runtime so the freestanding build needs no
   stdio, and so host CI exercises the exact same integer/message formatting an
   MCU runs. Supports only the conversions the runtime uses: %s %d %lld %llu %g
   and %%. On hosted builds %g delegates to libc so float output stays
   byte-for-byte identical to before; freestanding uses the compact formatter
   below (and emits "<float>" under VT_NO_FLOAT). */

static void fmt_putc(char *buf, size_t n, size_t *pos, char c) {
    if (*pos + 1 < n) buf[*pos] = c; /* always leave room for the NUL */
    (*pos)++;
}

static void fmt_puts(char *buf, size_t n, size_t *pos, const char *s) {
    if (!s) s = "(null)";
    while (*s) fmt_putc(buf, n, pos, *s++);
}

static void fmt_u64(char *buf, size_t n, size_t *pos, uint64_t v) {
    char tmp[20];
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) fmt_putc(buf, n, pos, tmp[--i]);
}

static void fmt_i64(char *buf, size_t n, size_t *pos, int64_t v) {
    if (v < 0) {
        fmt_putc(buf, n, pos, '-');
        fmt_u64(buf, n, pos, (uint64_t)(-(v + 1)) + 1); /* safe at INT64_MIN */
    } else {
        fmt_u64(buf, n, pos, (uint64_t)v);
    }
}

#if defined(VT_NO_LIBC) && !defined(VT_NO_FLOAT)
/* Compact float -> decimal, ~6 significant digits with rounding. Follows the
   %g fixed-vs-scientific rule but is not shortest-round-trip; adequate for
   diagnostics and embedded UI. */
static void fmt_g(char *buf, size_t n, size_t *pos, double v) {
    if (__builtin_isnan(v)) { fmt_puts(buf, n, pos, "nan"); return; }
    if (v < 0) { fmt_putc(buf, n, pos, '-'); v = -v; }
    if (__builtin_isinf(v)) { fmt_puts(buf, n, pos, "inf"); return; }
    if (v == 0.0) { fmt_putc(buf, n, pos, '0'); return; }

    int e = 0; /* decimal exponent: v in [10^e, 10^(e+1)) */
    while (v >= 10.0) { v /= 10.0; e++; }
    while (v < 1.0) { v *= 10.0; e--; }

    int digits[6];
    int nd = 6;
    for (int i = 0; i < nd; i++) {
        int d = (int)v;
        if (d > 9) d = 9;
        digits[i] = d;
        v = (v - d) * 10.0;
    }
    if ((int)v >= 5) { /* round up on the discarded 7th digit */
        int i = nd - 1;
        for (;;) {
            if (++digits[i] < 10) break;
            digits[i] = 0;
            if (i == 0) { /* 9.99999 -> 10.0000: shift and bump exponent */
                for (int k = nd - 1; k > 0; k--) digits[k] = digits[k - 1];
                digits[0] = 1;
                e++;
                break;
            }
            i--;
        }
    }
    while (nd > 1 && digits[nd - 1] == 0) nd--; /* trim trailing zeros */

    if (e < -4 || e >= 6) { /* scientific */
        fmt_putc(buf, n, pos, (char)('0' + digits[0]));
        if (nd > 1) {
            fmt_putc(buf, n, pos, '.');
            for (int i = 1; i < nd; i++) fmt_putc(buf, n, pos, (char)('0' + digits[i]));
        }
        fmt_putc(buf, n, pos, 'e');
        fmt_putc(buf, n, pos, e < 0 ? '-' : '+');
        int ae = e < 0 ? -e : e;
        char eb[4];
        int ei = 0;
        if (ae == 0) eb[ei++] = '0';
        while (ae) { eb[ei++] = (char)('0' + ae % 10); ae /= 10; }
        if (ei < 2) eb[ei++] = '0'; /* printf-style two-digit exponent */
        while (ei) fmt_putc(buf, n, pos, eb[--ei]);
    } else if (e >= 0) { /* fixed, magnitude >= 1 */
        for (int i = 0; i < nd; i++) {
            if (i == e + 1) fmt_putc(buf, n, pos, '.');
            fmt_putc(buf, n, pos, (char)('0' + digits[i]));
        }
        for (int i = nd; i <= e; i++) fmt_putc(buf, n, pos, '0'); /* pad integer part */
    } else { /* fixed, magnitude < 1: 0.00ddd */
        fmt_putc(buf, n, pos, '0');
        fmt_putc(buf, n, pos, '.');
        for (int i = 0; i < -e - 1; i++) fmt_putc(buf, n, pos, '0');
        for (int i = 0; i < nd; i++) fmt_putc(buf, n, pos, (char)('0' + digits[i]));
    }
}
#endif

static int vt_snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t pos = 0;
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') { fmt_putc(buf, n, &pos, *f); continue; }
        f++;
        if (*f == '%') { fmt_putc(buf, n, &pos, '%'); continue; }
        if (*f == 's') { fmt_puts(buf, n, &pos, va_arg(ap, const char *)); continue; }
        if (*f == 'd') { fmt_i64(buf, n, &pos, (int64_t)va_arg(ap, int)); continue; }
        if (*f == 'g') {
            double dv = va_arg(ap, double);
#if defined(VT_NO_LIBC)
#if defined(VT_NO_FLOAT)
            (void)dv;
            fmt_puts(buf, n, &pos, "<float>");
#else
            fmt_g(buf, n, &pos, dv);
#endif
#else
            char t[40];
            snprintf(t, sizeof t, "%g", dv);
            fmt_puts(buf, n, &pos, t);
#endif
            continue;
        }
        if (*f == 'l' && f[1] == 'l') {
            f += 2;
            if (*f == 'd') fmt_i64(buf, n, &pos, va_arg(ap, long long));
            else if (*f == 'u') fmt_u64(buf, n, &pos, va_arg(ap, unsigned long long));
            continue;
        }
        /* unrecognized specifier: emit it verbatim */
        fmt_putc(buf, n, &pos, '%');
        fmt_putc(buf, n, &pos, *f);
    }
    if (n) buf[pos < n ? pos : n - 1] = '\0';
    va_end(ap);
    return (int)pos;
}

/* ---- core RC ---- */

void *vt_alloc(size_t size, const VtType *type) {
    VtObj *o = vt_host_alloc(size); /* zeroed */
    if (!o) vt_oom();
    o->rc = 1;
    o->type = type;
    return o;
}

/* ---- auto-zeroing weak references ----
   A `weak` field stores a raw, non-owning T*. We record the address of every
   such slot in a side table keyed by the target object; when the target is
   freed, every slot that pointed at it is nulled. A stale weak read then yields
   null (a clean crash on use) instead of a dangling pointer into freed memory.
   Single-threaded: the table is unguarded, matching the current RC model. */
typedef struct WeakNode {
    void **slot;
    struct WeakNode *next;
} WeakNode;

typedef struct WeakEntry {
    void *target;
    WeakNode *slots;
    struct WeakEntry *next; /* bucket chain */
} WeakEntry;

#define VT_WEAK_BUCKETS 1024
static WeakEntry *vt_weak_tab[VT_WEAK_BUCKETS];
static int64_t vt_weak_live; /* registered slots; 0 => release skips the probe */

static size_t weak_hash(void *p) {
    uintptr_t x = (uintptr_t)p >> 4; /* drop alignment bits */
    return (size_t)(x & (VT_WEAK_BUCKETS - 1));
}

static void weak_detach(void **slot, void *target) {
    WeakEntry **pe = &vt_weak_tab[weak_hash(target)];
    for (WeakEntry *e = *pe; e; pe = &e->next, e = e->next) {
        if (e->target != target) continue;
        for (WeakNode **pn = &e->slots; *pn; pn = &(*pn)->next) {
            if ((*pn)->slot == slot) {
                WeakNode *n = *pn;
                *pn = n->next;
                vt_host_free(n);
                vt_weak_live--;
                break;
            }
        }
        if (!e->slots) { *pe = e->next; vt_host_free(e); }
        return;
    }
}

static void weak_attach(void **slot, void *target) {
    size_t h = weak_hash(target);
    WeakEntry *e = vt_weak_tab[h];
    for (; e; e = e->next) if (e->target == target) break;
    if (!e) {
        e = vt_host_alloc(sizeof *e);
        if (!e) vt_oom();
        e->target = target;
        e->next = vt_weak_tab[h];
        vt_weak_tab[h] = e;
    }
    WeakNode *n = vt_host_alloc(sizeof *n);
    if (!n) vt_oom();
    n->slot = slot;
    n->next = e->slots;
    e->slots = n;
    vt_weak_live++;
}

/* Assign `target` into a weak slot, keeping the registry in step. */
void vt_weak_set(void **slot, void *target) {
    void *old = *slot;
    if (old == target) return;
    if (old) weak_detach(slot, old);
    *slot = target;
    if (target) weak_attach(slot, target);
}

/* Unregister a weak slot whose owner is being torn down, so the registry never
   holds the address of freed memory. Called from a class deinit per weak field. */
void vt_weak_drop(void **slot) {
    void *old = *slot;
    if (old) weak_detach(slot, old);
    *slot = 0;
}

/* Null every weak slot pointing at `target`; called as `target` is freed. */
static void vt_weak_on_free(void *target) {
    WeakEntry **pe = &vt_weak_tab[weak_hash(target)];
    for (WeakEntry *e = *pe; e; pe = &e->next, e = e->next) {
        if (e->target != target) continue;
        for (WeakNode *n = e->slots; n;) {
            WeakNode *nx = n->next;
            *n->slot = 0;
            vt_host_free(n);
            vt_weak_live--;
            n = nx;
        }
        *pe = e->next;
        vt_host_free(e);
        return;
    }
}

void vt_release(void *p) {
    if (!p) return;
    VtObj *o = (VtObj *)p;
    if (o->rc < 0) return; /* immortal */
#ifdef VT_ATOMIC_RC
    /* Release on the decrement, acquire before running the destructor, so a
       concurrent releaser's writes are visible to the thread that frees. */
    if (__atomic_fetch_sub(&o->rc, 1, __ATOMIC_RELEASE) == 1) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (vt_weak_live) vt_weak_on_free(o);
        if (o->type && o->type->deinit) o->type->deinit(o);
        vt_host_free(o);
    }
#else
    if (--o->rc == 0) {
        if (vt_weak_live) vt_weak_on_free(o);
        if (o->type && o->type->deinit) o->type->deinit(o);
        vt_host_free(o);
    }
#endif
}

void vt_free_now(void *p) { vt_host_free(p); }

bool vt_isa(const void *p, const VtType *t) {
    if (!p) return false;
    for (const VtType *ty = ((const VtObj *)p)->type; ty; ty = ty->parent)
        if (ty == t) return true;
    return false;
}

void *vt_checked_cast(void *p, const VtType *t, const char *file, int line) {
    if (!p) return NULL;
    if (!vt_isa(p, t)) {
        char buf[256];
        vt_snprintf(buf, sizeof buf, "%s:%d: panic: invalid cast from %s to %s\n", file, line,
                    ((VtObj *)p)->type->name, t->name);
        vt_puts_err(buf);
        vt_host_abort();
    }
    return p;
}

VT_NORETURN void vt_panic_c(const char *file, int line, const char *msg) {
    char buf[256];
    vt_snprintf(buf, sizeof buf, "%s:%d: panic: %s\n", file, line, msg);
    vt_puts_err(buf);
    vt_host_abort();
}

/* ---- command-line arguments ---- */
static int g_argc;
static char **g_argv;
void vt_set_args(int argc, char **argv) { g_argc = argc; g_argv = argv; }
VtArray *vt_args(void) {
    VtArray *a = vt_arr_new(sizeof(VtString *), true);
    for (int i = 1; i < g_argc; i++) { /* skip argv[0], the program name */
        VtString *s = vt_str_from_cstr(g_argv[i]);
        vt_arr_push(a, &s); /* push retains */
        vt_release(s);
    }
    return a;
}

/* ---- checked signed integer arithmetic ----
   Compute in the int64 domain (which holds any i8..i32 sum/product), detect
   i64 overflow, then range-check against the target type's [lo,hi]. GCC/Clang
   use __builtin_*_overflow; tcc/others fall back to portable sign tests. */
#if defined(__GNUC__) || defined(__clang__)
#define VT_HAS_OVF_BUILTINS 1
#endif

static void vt_overflow(const char *file, int line, const char *op) {
    char msg[64];
    vt_snprintf(msg, sizeof msg, "integer overflow in '%s'", op);
    vt_panic_c(file, line, msg);
}

static void vt_div_zero(const char *file, int line, const char *op) {
    char msg[64];
    vt_snprintf(msg, sizeof msg, "division by zero in '%s'", op);
    vt_panic_c(file, line, msg);
}

/* Signed divide/modulo: trap zero divisor (would SIGFPE) and the INT_MIN/-1
   overflow (C UB), then range-check the quotient against the target type. */
int64_t vt_ck_div(int64_t a, int64_t b, int64_t lo, int64_t hi, const char *file, int line) {
    if (b == 0) vt_div_zero(file, line, "/");
    if (b == -1 && a == INT64_MIN) vt_overflow(file, line, "/");
    int64_t r = a / b;
    if (r < lo || r > hi) vt_overflow(file, line, "/");
    return r;
}

int64_t vt_ck_mod(int64_t a, int64_t b, const char *file, int line) {
    if (b == 0) vt_div_zero(file, line, "%");
    if (b == -1) return 0; /* a % -1 is 0; dodge the INT_MIN/-1 UB */
    return a % b;
}

uint64_t vt_ck_udiv(uint64_t a, uint64_t b, const char *file, int line) {
    if (b == 0) vt_div_zero(file, line, "/");
    return a / b;
}

uint64_t vt_ck_umod(uint64_t a, uint64_t b, const char *file, int line) {
    if (b == 0) vt_div_zero(file, line, "%");
    return a % b;
}

int64_t vt_ck_add(int64_t a, int64_t b, int64_t lo, int64_t hi, const char *file, int line) {
    int64_t r;
#ifdef VT_HAS_OVF_BUILTINS
    bool of = __builtin_add_overflow(a, b, &r);
#else
    r = (int64_t)((uint64_t)a + (uint64_t)b);
    bool of = ((a ^ r) & (b ^ r)) < 0; /* operands same sign, result differs */
#endif
    if (of || r < lo || r > hi) vt_overflow(file, line, "+");
    return r;
}

int64_t vt_ck_sub(int64_t a, int64_t b, int64_t lo, int64_t hi, const char *file, int line) {
    int64_t r;
#ifdef VT_HAS_OVF_BUILTINS
    bool of = __builtin_sub_overflow(a, b, &r);
#else
    r = (int64_t)((uint64_t)a - (uint64_t)b);
    bool of = ((a ^ b) & (a ^ r)) < 0;
#endif
    if (of || r < lo || r > hi) vt_overflow(file, line, "-");
    return r;
}

int64_t vt_ck_mul(int64_t a, int64_t b, int64_t lo, int64_t hi, const char *file, int line) {
    int64_t r;
#ifdef VT_HAS_OVF_BUILTINS
    bool of = __builtin_mul_overflow(a, b, &r);
#else
    r = (int64_t)((uint64_t)a * (uint64_t)b);
    bool of = (a != 0) && (r / a != b || (a == -1 && b == INT64_MIN));
#endif
    if (of || r < lo || r > hi) vt_overflow(file, line, "*");
    return r;
}

int64_t vt_ck_neg(int64_t a, int64_t lo, int64_t hi, const char *file, int line) {
    if (a == INT64_MIN) vt_overflow(file, line, "-");
    int64_t r = -a;
    if (r < lo || r > hi) vt_overflow(file, line, "-");
    return r;
}

static void vt_bad_shift(const char *file, int line, int64_t b, int bits) {
    char msg[64];
    vt_snprintf(msg, sizeof msg, "shift amount %lld out of range [0, %d)", (long long)b, bits);
    vt_panic_c(file, line, msg);
}

int64_t vt_ck_shl(int64_t a, int64_t b, int bits, const char *file, int line) {
    if (b < 0 || b >= bits) vt_bad_shift(file, line, b, bits);
    return (int64_t)((uint64_t)a << b); /* wraps like the release-mode C operator */
}

int64_t vt_ck_shr(int64_t a, int64_t b, int bits, const char *file, int line) {
    if (b < 0 || b >= bits) vt_bad_shift(file, line, b, bits);
    return a >> b;
}

uint64_t vt_ck_shru(uint64_t a, int64_t b, int bits, const char *file, int line) {
    if (b < 0 || b >= bits) vt_bad_shift(file, line, b, bits);
    return a >> b;
}

VT_NORETURN void vt_panic(const char *file, int line, VtString *msg) {
    vt_panic_c(file, line, msg ? msg->data : "(null)");
}

/* ---- strings ---- */

static void str_deinit(void *self) { (void)self; }
static const VtType vt_string_type = {"string", str_deinit, NULL, NULL};

static VtString *str_alloc(int64_t len) {
    VtString *s = vt_host_alloc(sizeof(VtString) + (size_t)len + 1);
    if (!s) vt_oom();
    s->hdr.rc = 1;
    s->hdr.type = &vt_string_type;
    s->len = len;
    s->data[len] = 0;
    return s;
}

VtString *vt_str_new(const char *bytes, int64_t len) {
    VtString *s = str_alloc(len);
    memcpy(s->data, bytes, (size_t)len);
    return s;
}

VtString *vt_str_immortal(const char *bytes, int64_t len) {
    VtString *s = vt_str_new(bytes, len);
    s->hdr.rc = -1;
    return s;
}

VtString *vt_str_concat(VtString *a, VtString *b) {
    int64_t la = a ? a->len : 0, lb = b ? b->len : 0;
    VtString *s = str_alloc(la + lb);
    if (a) memcpy(s->data, a->data, (size_t)la);
    if (b) memcpy(s->data + la, b->data, (size_t)lb);
    return s;
}

bool vt_str_eq(VtString *a, VtString *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return a->len == b->len && memcmp(a->data, b->data, (size_t)a->len) == 0;
}

VtString *vt_str_from_int(int64_t v) {
    char buf[32];
    int n = vt_snprintf(buf, sizeof buf, "%lld", (long long)v);
    return vt_str_new(buf, n);
}

VtString *vt_str_from_float(double v) {
    char buf[40];
    int n = vt_snprintf(buf, sizeof buf, "%g", v);
    return vt_str_new(buf, n);
}

VtString *vt_str_from_bool(bool v) { return vt_str_new(v ? "true" : "false", v ? 4 : 5); }

VtString *vt_str_from_cstr(const char *p) {
    if (!p) return vt_str_new("", 0);
    return vt_str_new(p, (int64_t)strlen(p));
}

VtString *vt_str_slice(VtString *s, int64_t lo, int64_t hi, const char *file, int line) {
    if (!s) vt_panic_c(file, line, "slice of null string");
    int64_t len = s->len;
    if (lo < 0 || hi < lo || hi > len) {
        char buf[96];
        vt_snprintf(buf, sizeof buf, "slice [%lld, %lld) out of bounds (len %lld)", (long long)lo,
                    (long long)hi, (long long)len);
        vt_panic_c(file, line, buf);
    }
    return vt_str_new(s->data + lo, hi - lo);
}

int64_t vt_str_index(VtString *s, int64_t i, const char *file, int line) {
    if (!s || i < 0 || i >= s->len) vt_panic_c(file, line, "string index out of bounds");
    return (int64_t)(unsigned char)s->data[i];
}

void vt_print(VtString *s) {
    if (s) vt_host_write(s->data, (size_t)s->len);
    vt_host_write("\n", 1);
}

/* ---- file I/O ----
   Needs a hosted filesystem. Under VT_NO_FS the symbols remain (so programs
   that never touch File still link) but each panics or reports absence. */
#ifndef VT_NO_FS

static FILE *file_open(VtString *path, const char *mode, const char *file, int line) {
    FILE *f = fopen(vt_str_cstr(path), mode);
    if (!f) {
        char buf[512];
        vt_snprintf(buf, sizeof buf, "cannot open file: %s", vt_str_cstr(path));
        vt_panic_c(file, line, buf);
    }
    return f;
}

VtString *vt_file_read(VtString *path, const char *file, int line) {
    /* Embedded asset (--with-assets) shadows disk: readfile/readlines and any
       config/JSON built on them resolve from the binary when a key matches. */
    const unsigned char *ad; long alen;
    if (vt_vfs_get(vt_str_cstr(path), &ad, &alen)) {
        VtString *s = str_alloc((int64_t)alen);
        memcpy(s->data, ad, (size_t)alen);
        return s;
    }
    FILE *f = file_open(path, "rb", file, line);
    /* Read to EOF into a growing buffer rather than trusting the stat size:
       /proc and /sys files report size 0, and pipes aren't seekable. */
    size_t cap = 65536, len = 0;
    char *buf = vt_host_alloc(cap);
    if (!buf) { fclose(f); vt_panic_c(file, line, "out of memory reading file"); }
    for (;;) {
        if (len == cap) {
            cap *= 2;
            char *nb = vt_host_realloc(buf, cap);
            if (!nb) { vt_host_free(buf); fclose(f); vt_panic_c(file, line, "out of memory reading file"); }
            buf = nb;
        }
        size_t got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (got == 0) break; /* EOF or error */
    }
    if (ferror(f)) { vt_host_free(buf); fclose(f); vt_panic_c(file, line, "error reading file"); }
    fclose(f);
    VtString *s = str_alloc((int64_t)len);
    memcpy(s->data, buf, len);
    vt_host_free(buf);
    return s;
}

bool vt_file_write(VtString *path, VtString *data, bool append) {
    FILE *f = fopen(vt_str_cstr(path), append ? "ab" : "wb");
    if (!f) return false;
    size_t n = data ? (size_t)data->len : 0;
    bool ok = fwrite(data ? data->data : "", 1, n, f) == n;
    return (fclose(f) == 0) && ok;
}

VtArray *vt_file_lines(VtString *path, const char *file, int line) {
    VtString *whole = vt_file_read(path, file, line);
    VtArray *a = vt_arr_new(sizeof(VtString *), true);
    const char *p = whole->data, *end = whole->data + whole->len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *stop = nl ? nl : end;
        size_t l = (size_t)(stop - p);
        if (l > 0 && stop[-1] == '\r') l--; /* CRLF */
        VtString *s = vt_str_new(p, (int64_t)l);
        vt_arr_push(a, &s); /* push retains */
        vt_release(s);
        if (!nl) break;
        p = nl + 1;
    }
    vt_release(whole);
    return a;
}

static int cstr_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static char **dir_names_push(char **names, int *n, int *cap, const char *name) {
    if (*n == *cap) {
        *cap *= 2;
        names = vt_host_realloc(names, (size_t)*cap * sizeof *names);
        if (!names) vt_oom();
    }
    size_t len = strlen(name) + 1;
    names[*n] = vt_host_alloc(len);
    if (!names[*n]) vt_oom();
    memcpy(names[*n], name, len);
    (*n)++;
    return names;
}

/* directory entry names (excluding ".", including ".."), sorted ascending */
VtArray *vt_dir_list(VtString *path, const char *file, int line) {
    const char *dir = vt_str_cstr(path);
    int n = 0, cap = 256;
    char **names = vt_host_alloc((size_t)cap * sizeof *names);
    if (!names) vt_oom();
#ifdef _WIN32
    char pat[1024];
    vt_snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        char buf[512];
        vt_snprintf(buf, sizeof buf, "cannot open directory: %s", dir);
        vt_panic_c(file, line, buf);
    }
    do {
        if (strcmp(fd.cFileName, ".") == 0) continue;
        names = dir_names_push(names, &n, &cap, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) {
        char buf[512];
        vt_snprintf(buf, sizeof buf, "cannot open directory: %s", dir);
        vt_panic_c(file, line, buf);
    }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") == 0) continue;
        names = dir_names_push(names, &n, &cap, de->d_name);
    }
    closedir(d);
#endif
    qsort(names, (size_t)n, sizeof names[0], cstr_cmp);
    VtArray *a = vt_arr_new(sizeof(VtString *), true);
    for (int i = 0; i < n; i++) {
        VtString *s = vt_str_from_cstr(names[i]);
        vt_arr_push(a, &s); /* push retains */
        vt_release(s);
        vt_host_free(names[i]);
    }
    vt_host_free(names);
    return a;
}

bool vt_is_dir(VtString *path) {
    const char *p = vt_str_cstr(path);
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

#else /* VT_NO_FS: no hosted filesystem */

static const char vt_no_fs_msg[] = "no filesystem in this build";
VtString *vt_file_read(VtString *path, const char *file, int line) {
    (void)path;
    vt_panic_c(file, line, vt_no_fs_msg);
    return NULL;
}
bool vt_file_write(VtString *path, VtString *data, bool append) {
    (void)path; (void)data; (void)append;
    return false;
}
VtArray *vt_file_lines(VtString *path, const char *file, int line) {
    (void)path;
    vt_panic_c(file, line, vt_no_fs_msg);
    return NULL;
}
VtArray *vt_dir_list(VtString *path, const char *file, int line) {
    (void)path;
    vt_panic_c(file, line, vt_no_fs_msg);
    return NULL;
}
bool vt_is_dir(VtString *path) { (void)path; return false; }

#endif /* VT_NO_FS */

/* ---- arrays ---- */

static void arr_deinit(void *self) {
    VtArray *a = self;
    if (a->elem_ref)
        for (int64_t i = 0; i < a->len; i++)
            vt_release(*(void **)(a->data + i * a->elem_size));
    vt_host_free(a->data);
}
static const VtType vt_array_type = {"array", arr_deinit, NULL, NULL};

VtArray *vt_arr_new(int32_t elem_size, bool elem_ref) {
    VtArray *a = vt_alloc(sizeof(VtArray), &vt_array_type);
    a->elem_size = elem_size;
    a->elem_ref = elem_ref;
    return a;
}

VtArray *vt_arr_bytes(int64_t n) {
    VtArray *a = vt_arr_new(1, false);
    if (n < 0) n = 0;
    a->data = vt_host_alloc((size_t)n ? (size_t)n : 1); /* zeroed */
    if (!a->data) vt_oom();
    a->len = a->cap = n;
    return a;
}

void vt_arr_push(VtArray *a, const void *elem) {
    if (a->len == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->data = vt_host_realloc(a->data, (size_t)(a->cap * a->elem_size));
        if (!a->data) vt_oom();
    }
    memcpy(a->data + a->len * a->elem_size, elem, (size_t)a->elem_size);
    if (a->elem_ref) vt_retain(*(void **)elem);
    a->len++;
}

void vt_arr_push_at(VtArray *a, const void *elem, const char *file, int line) {
    if (!a) vt_panic_c(file, line, "push to null array");
    vt_arr_push(a, elem);
}

void vt_arr_pop(VtArray *a, void *out, const char *file, int line) {
    if (!a) vt_panic_c(file, line, "pop from null array");
    if (a->len == 0) vt_panic_c(file, line, "pop from empty array");
    a->len--;
    memcpy(out, a->data + a->len * a->elem_size, (size_t)a->elem_size);
    /* ownership transfers to caller: no release, no retain */
}

/* Cold failure path for the inlined vt_arr_at (see vyto_rt.h). */
VT_NORETURN void vt_arr_oob(VtArray *a, int64_t i, const char *file, int line) {
    if (!a) vt_panic_c(file, line, "index into null array");
    char buf[96];
    vt_snprintf(buf, sizeof buf, "array index %lld out of bounds (len %lld)",
                (long long)i, (long long)a->len);
    vt_panic_c(file, line, buf);
}

void vt_arr_set(VtArray *a, int64_t i, const void *elem, const char *file, int line) {
    void *slot = vt_arr_at(a, i, file, line);
    if (a->elem_ref) {
        vt_retain(*(void **)elem);
        vt_release(*(void **)slot);
    }
    memcpy(slot, elem, (size_t)a->elem_size);
}

/* ---- maps ---- */

static void map_deinit(void *self) {
    VtMap *m = self;
    for (int64_t b = 0; b < m->nbuckets; b++) {
        VtMapEntry *e = m->buckets[b];
        while (e) {
            VtMapEntry *next = e->next;
            vt_release(e->key);
            if (m->val_ref) vt_release((void *)(uintptr_t)e->val);
            vt_host_free(e);
            e = next;
        }
    }
    vt_host_free(m->buckets);
}
static const VtType vt_map_type = {"map", map_deinit, NULL, NULL};

VtMap *vt_map_new(bool val_ref) {
    VtMap *m = vt_alloc(sizeof(VtMap), &vt_map_type);
    m->nbuckets = 64;
    m->buckets = vt_host_alloc((size_t)m->nbuckets * sizeof(VtMapEntry *));
    if (!m->buckets) vt_oom();
    m->val_ref = val_ref;
    return m;
}

static uint64_t map_hash(VtString *k) {
    uint64_t h = 1469598103934665603ull;
    for (int64_t i = 0; i < k->len; i++) { h ^= (unsigned char)k->data[i]; h *= 1099511628211ull; }
    return h;
}

static VtMapEntry **map_slot(VtMap *m, VtString *key) {
    VtMapEntry **pe = &m->buckets[map_hash(key) % (uint64_t)m->nbuckets];
    while (*pe && !vt_str_eq((*pe)->key, key)) pe = &(*pe)->next;
    return pe;
}

/* double the table once chains average 4 deep; entries are relinked in place */
static void map_grow(VtMap *m) {
    int64_t nb = m->nbuckets * 2;
    VtMapEntry **nbk = vt_host_alloc((size_t)nb * sizeof *nbk);
    if (!nbk) return; /* keep the old (correct, slower) table on OOM */
    for (int64_t i = 0; i < m->nbuckets; i++) {
        VtMapEntry *e = m->buckets[i];
        while (e) {
            VtMapEntry *next = e->next;
            uint64_t h = map_hash(e->key) % (uint64_t)nb;
            e->next = nbk[h];
            nbk[h] = e;
            e = next;
        }
    }
    vt_host_free(m->buckets);
    m->buckets = nbk;
    m->nbuckets = nb;
}

void vt_map_set(VtMap *m, VtString *key, uint64_t val, const char *file, int line) {
    if (!m) vt_panic_c(file, line, "set on null map");
    VtMapEntry **pe = map_slot(m, key);
    if (*pe) {
        if (m->val_ref) {
            vt_retain((void *)(uintptr_t)val);
            vt_release((void *)(uintptr_t)(*pe)->val);
        }
        (*pe)->val = val;
        return;
    }
    VtMapEntry *e = vt_host_alloc(sizeof *e);
    if (!e) vt_oom();
    vt_retain(key);
    e->key = key;
    e->val = val;
    e->next = NULL;
    if (m->val_ref) vt_retain((void *)(uintptr_t)val);
    *pe = e;
    m->len++;
    if (m->len > m->nbuckets * 4) map_grow(m);
}

uint64_t vt_map_get(VtMap *m, VtString *key, const char *file, int line) {
    if (!m) vt_panic_c(file, line, "get on null map");
    VtMapEntry **pe = map_slot(m, key);
    if (!*pe) {
        char buf[160];
        vt_snprintf(buf, sizeof buf, "map key not found: %s", key ? key->data : "(null)");
        vt_panic_c(file, line, buf);
    }
    return (*pe)->val;
}

bool vt_map_has(VtMap *m, VtString *key, const char *file, int line) {
    if (!m) vt_panic_c(file, line, "has on null map");
    return *map_slot(m, key) != NULL;
}

void vt_map_remove(VtMap *m, VtString *key, const char *file, int line) {
    if (!m) vt_panic_c(file, line, "remove on null map");
    VtMapEntry **pe = map_slot(m, key);
    if (!*pe) return;
    VtMapEntry *e = *pe;
    *pe = e->next;
    vt_release(e->key);
    if (m->val_ref) vt_release((void *)(uintptr_t)e->val);
    vt_host_free(e);
    m->len--;
}

/* ---- closures ---- */

static void closure_deinit(void *self) {
    VtClosure *c = self;
    vt_release(c->env);
}
static const VtType vt_closure_type = {"closure", closure_deinit, NULL, NULL};

VtClosure *vt_closure_new(void *fn, VtObj *env) {
    VtClosure *c = vt_alloc(sizeof(VtClosure), &vt_closure_type);
    c->fn = fn;
    c->env = env; /* ownership transferred */
    return c;
}

/* ---- builtin-method helper units (amalgamated) ----
 * Included here so they share every static host hook / helper above and compile
 * into the single vyto_rt translation unit. Each file's mtime is tracked by the
 * driver (src/main.c) so edits trigger a runtime rebuild. */
#include "vyto_rt_num.c"
#include "vyto_rt_str.c"
#include "vyto_rt_arr.c"
#include "vyto_rt_map.c"
#include "vyto_vfs.c"
