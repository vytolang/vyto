/* volt/util/fmt native backing — number formatting only.

   Each function forwards a single value to snprintf. The Volt side builds the
   conversion spec (it inserts the "ll" length modifier and picks the matching
   entry point), so C never sees a mismatched %-conversion — no varargs are
   exposed across the FFI boundary and there is no format-string-injection path.
   Output is bounded by the caller-provided `cap` (a Volt `bytes(cap)` buffer);
   the returned length never exceeds cap-1, and snprintf always NUL-terminates,
   so the Volt side can `str(buf as cstring)` safely (numeric output has no
   interior NUL). */

#include <stdio.h>

static int clamp(int n, int cap) {
    if (n < 0) return 0;
    if (n >= cap) return cap > 0 ? cap - 1 : 0; /* truncated; keep in-bounds */
    return n;
}

int fmt_i64(const char *spec, long long v, char *out, int cap) {
    return clamp(snprintf(out, (size_t)cap, spec, v), cap);
}

int fmt_u64(const char *spec, unsigned long long v, char *out, int cap) {
    return clamp(snprintf(out, (size_t)cap, spec, v), cap);
}

int fmt_f64(const char *spec, double v, char *out, int cap) {
    return clamp(snprintf(out, (size_t)cap, spec, v), cap);
}
