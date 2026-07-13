/* vyto_rt_num.c — int and float builtin-method runtime helpers.
 *
 * Amalgamated into vyto_rt.c (included at its end), so every static helper and
 * host hook defined there is visible here. The `#include "vyto_rt.h"` below is
 * guarded/no-op under amalgamation but keeps this file independently parseable.
 *
 * Float helpers use libm on hosted builds; the freestanding profile
 * (VT_NO_LIBC) gets self-contained fallbacks so no transcendental libc symbol
 * is referenced. */

#include "vyto_rt.h"

#ifndef VT_NO_LIBC
#include <math.h>
#endif

/* ---- int methods ----
 * Overflow traps mirror the language's checked arithmetic (vt_ck_*): silent
 * wrapping would be inconsistent with `*`/`+` which panic. */

int64_t vt_int_abs(int64_t x, const char *file, int line) {
    if (x == INT64_MIN) vt_panic_c(file, line, "integer overflow in 'abs'");
    return x < 0 ? -x : x;
}
int64_t vt_int_min(int64_t a, int64_t b) { return a < b ? a : b; }
int64_t vt_int_max(int64_t a, int64_t b) { return a > b ? a : b; }

int64_t vt_int_clamp(int64_t x, int64_t lo, int64_t hi) {
    if (lo > hi) { int64_t t = lo; lo = hi; hi = t; }
    return x < lo ? lo : (x > hi ? hi : x);
}

int64_t vt_int_sign(int64_t x) { return (x > 0) - (x < 0); }

/* checked multiply for pow; same trap semantics as vt_ck_mul */
static int64_t int_pow_mul(int64_t a, int64_t b, const char *file, int line) {
    int64_t r;
#ifdef VT_HAS_OVF_BUILTINS
    bool of = __builtin_mul_overflow(a, b, &r);
#else
    r = (int64_t)((uint64_t)a * (uint64_t)b);
    bool of = (a != 0) && (r / a != b || (a == -1 && b == INT64_MIN));
#endif
    if (of) vt_panic_c(file, line, "integer overflow in 'pow'");
    return r;
}

/* integer exponentiation by squaring; negative exponent yields 0 (no int
 * fractions), exp 0 yields 1. Panics on overflow like checked `*`. */
int64_t vt_int_pow(int64_t base, int64_t exp, const char *file, int line) {
    if (exp < 0) return 0;
    int64_t r = 1;
    while (exp) {
        if (exp & 1) r = int_pow_mul(r, base, file, line);
        exp >>= 1;
        if (exp) base = int_pow_mul(base, base, file, line);
    }
    return r;
}

int64_t vt_int_gcd(int64_t a, int64_t b, const char *file, int line) {
    /* unsigned magnitudes so INT64_MIN doesn't overflow on negation */
    uint64_t ua = a < 0 ? (uint64_t)(-(a + 1)) + 1 : (uint64_t)a;
    uint64_t ub = b < 0 ? (uint64_t)(-(b + 1)) + 1 : (uint64_t)b;
    while (ub) { uint64_t t = ua % ub; ua = ub; ub = t; }
    if (ua > (uint64_t)INT64_MAX) /* gcd(INT64_MIN, 0) == 2^63 */
        vt_panic_c(file, line, "integer overflow in 'gcd'");
    return (int64_t)ua;
}

/* ---- float methods ---- */

double vt_flt_abs(double x) { return x < 0 ? -x : x; }
double vt_flt_min(double a, double b) { return a < b ? a : b; }
double vt_flt_max(double a, double b) { return a > b ? a : b; }

double vt_flt_clamp(double x, double lo, double hi) {
    if (lo > hi) { double t = lo; lo = hi; hi = t; }
    return x < lo ? lo : (x > hi ? hi : x);
}

bool vt_flt_is_nan(double x) { return x != x; }

#ifndef VT_NO_LIBC

double vt_flt_floor(double x) { return floor(x); }
double vt_flt_ceil(double x) { return ceil(x); }
double vt_flt_round(double x) { return round(x); }
double vt_flt_trunc(double x) { return trunc(x); }
double vt_flt_sqrt(double x) { return sqrt(x); }
double vt_flt_pow(double b, double e) { return pow(b, e); }

#else /* freestanding: self-contained fallbacks, no libm */

/* cast-based rounding is exact for |x| < 2^63; larger magnitudes are already
 * integral in double, so return unchanged. */
static double flt_int_ok(double x) { return vt_flt_abs(x) < 9.2233720368547758e18; }
double vt_flt_trunc(double x) { return flt_int_ok(x) ? (double)(int64_t)x : x; }
double vt_flt_floor(double x) {
    double t = vt_flt_trunc(x);
    return (t > x) ? t - 1.0 : t;
}
double vt_flt_ceil(double x) {
    double t = vt_flt_trunc(x);
    return (t < x) ? t + 1.0 : t;
}
/* half away from zero, matching hosted libm round() (floor(x+0.5) would give
 * round(-2.5) == -2 and diverge between profiles) */
double vt_flt_round(double x) {
    double t = vt_flt_trunc(x);
    double f = x - t;
    if (f >= 0.5) return t + 1.0;
    if (f <= -0.5) return t - 1.0;
    return t;
}

double vt_flt_sqrt(double x) {
    if (x < 0) return x != x ? x : (0.0 / 0.0); /* NaN */
    if (x == 0.0) return 0.0;
    double g = x > 1.0 ? x : 1.0;
    for (int i = 0; i < 60; i++) g = 0.5 * (g + x / g);
    return g;
}

/* freestanding pow supports integer exponents (common case) via squaring;
 * fractional exponents return NaN rather than pulling in exp/log. */
double vt_flt_pow(double b, double e) {
    double ei = vt_flt_trunc(e);
    if (ei != e) return 0.0 / 0.0; /* NaN: non-integer exponent unsupported */
    int64_t n = (int64_t)ei;
    bool neg = n < 0;
    if (neg) n = -n;
    double r = 1.0;
    while (n) { if (n & 1) r *= b; n >>= 1; if (n) b *= b; }
    return neg ? 1.0 / r : r;
}

#endif
