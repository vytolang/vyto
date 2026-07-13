/* vyto_rt_str.c — string builtin-method runtime helpers.
 *
 * Amalgamated into vyto_rt.c (included at its end); shares its host hooks and
 * mem* availability. All operations are byte-oriented; case + whitespace are
 * ASCII-only (multibyte sequences are passed through untouched, and reverse()
 * is a raw byte reversal — documented at the call surface). */

#include "vyto_rt.h"

static bool str_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/* first index >= from where needle occurs in hay, else -1. Empty needle -> from. */
static int64_t str_find(const char *hay, int64_t hlen, const char *ndl, int64_t nlen,
                        int64_t from) {
    if (nlen == 0) return from <= hlen ? from : -1;
    if (nlen > hlen) return -1;
    for (int64_t i = from; i + nlen <= hlen; i++)
        if (memcmp(hay + i, ndl, (size_t)nlen) == 0) return i;
    return -1;
}

bool vt_str_starts_with(VtString *s, VtString *p) {
    return p->len <= s->len && memcmp(s->data, p->data, (size_t)p->len) == 0;
}
bool vt_str_ends_with(VtString *s, VtString *p) {
    return p->len <= s->len &&
           memcmp(s->data + s->len - p->len, p->data, (size_t)p->len) == 0;
}
int64_t vt_str_index_of(VtString *s, VtString *sub) {
    return str_find(s->data, s->len, sub->data, sub->len, 0);
}
int64_t vt_str_last_index_of(VtString *s, VtString *sub) {
    if (sub->len == 0) return s->len;
    if (sub->len > s->len) return -1;
    for (int64_t i = s->len - sub->len; i >= 0; i--)
        if (memcmp(s->data + i, sub->data, (size_t)sub->len) == 0) return i;
    return -1;
}
bool vt_str_contains(VtString *s, VtString *sub) {
    return vt_str_index_of(s, sub) >= 0;
}
int64_t vt_str_count(VtString *s, VtString *sub) {
    if (sub->len == 0) return 0;
    int64_t n = 0, i = 0;
    while (i + sub->len <= s->len) {
        if (memcmp(s->data + i, sub->data, (size_t)sub->len) == 0) { n++; i += sub->len; }
        else i++;
    }
    return n;
}

VtString *vt_str_char_at(VtString *s, int64_t i, const char *file, int line) {
    if (i < 0 || i >= s->len) vt_panic_c(file, line, "char_at index out of bounds");
    return vt_str_new(s->data + i, 1);
}

VtString *vt_str_to_upper(VtString *s) {
    VtString *r = vt_str_new(s->data, s->len);
    for (int64_t i = 0; i < r->len; i++)
        if (r->data[i] >= 'a' && r->data[i] <= 'z') r->data[i] = (char)(r->data[i] - 32);
    return r;
}
VtString *vt_str_to_lower(VtString *s) {
    VtString *r = vt_str_new(s->data, s->len);
    for (int64_t i = 0; i < r->len; i++)
        if (r->data[i] >= 'A' && r->data[i] <= 'Z') r->data[i] = (char)(r->data[i] + 32);
    return r;
}

VtString *vt_str_trim_start(VtString *s) {
    int64_t lo = 0;
    while (lo < s->len && str_is_space(s->data[lo])) lo++;
    return vt_str_new(s->data + lo, s->len - lo);
}
VtString *vt_str_trim_end(VtString *s) {
    int64_t hi = s->len;
    while (hi > 0 && str_is_space(s->data[hi - 1])) hi--;
    return vt_str_new(s->data, hi);
}
VtString *vt_str_trim(VtString *s) {
    int64_t lo = 0, hi = s->len;
    while (lo < hi && str_is_space(s->data[lo])) lo++;
    while (hi > lo && str_is_space(s->data[hi - 1])) hi--;
    return vt_str_new(s->data + lo, hi - lo);
}

VtString *vt_str_repeat(VtString *s, int64_t n, const char *file, int line) {
    if (n <= 0 || s->len == 0) return vt_str_new("", 0);
    if (n > INT64_MAX / s->len) vt_panic_c(file, line, "repeat result too large");
    int64_t total = s->len * n;
    char *buf = vt_host_alloc((size_t)total);
    if (!buf) vt_oom();
    for (int64_t i = 0; i < n; i++) memcpy(buf + i * s->len, s->data, (size_t)s->len);
    VtString *r = vt_str_new(buf, total);
    vt_host_free(buf);
    return r;
}

/* pad `s` to width `w` by prepending/appending copies of `ch` (truncated to
 * fill exactly). Returns a copy of `s` when already wide enough or `ch` empty. */
static VtString *str_pad(VtString *s, int64_t w, VtString *ch, bool at_start) {
    if (s->len >= w || ch->len == 0) return vt_str_new(s->data, s->len);
    int64_t padlen = w - s->len;
    char *buf = vt_host_alloc((size_t)w);
    if (!buf) vt_oom();
    char *pad_dst = at_start ? buf : buf + s->len;
    for (int64_t i = 0; i < padlen; i++) pad_dst[i] = ch->data[i % ch->len];
    memcpy(at_start ? buf + padlen : buf, s->data, (size_t)s->len);
    VtString *r = vt_str_new(buf, w);
    vt_host_free(buf);
    return r;
}
VtString *vt_str_pad_start(VtString *s, int64_t w, VtString *ch) { return str_pad(s, w, ch, true); }
VtString *vt_str_pad_end(VtString *s, int64_t w, VtString *ch) { return str_pad(s, w, ch, false); }

VtString *vt_str_replace(VtString *s, VtString *old, VtString *neu) {
    if (old->len == 0) return vt_str_new(s->data, s->len);
    int64_t occ = vt_str_count(s, old);
    if (occ == 0) return vt_str_new(s->data, s->len);
    int64_t total = s->len + occ * (neu->len - old->len);
    char *buf = total > 0 ? vt_host_alloc((size_t)total) : NULL;
    if (total > 0 && !buf) vt_oom();
    int64_t si = 0, di = 0;
    while (si < s->len) {
        if (si + old->len <= s->len && memcmp(s->data + si, old->data, (size_t)old->len) == 0) {
            if (neu->len) memcpy(buf + di, neu->data, (size_t)neu->len);
            di += neu->len;
            si += old->len;
        } else {
            buf[di++] = s->data[si++];
        }
    }
    VtString *r = vt_str_new(buf ? buf : "", total);
    if (buf) vt_host_free(buf);
    return r;
}

VtString *vt_str_reverse(VtString *s) {
    VtString *r = vt_str_new(s->data, s->len);
    for (int64_t i = 0, j = r->len - 1; i < j; i++, j--) {
        char t = r->data[i]; r->data[i] = r->data[j]; r->data[j] = t;
    }
    return r;
}

/* split on `sep`. Empty `sep` yields a single element equal to `s`. */
VtArray *vt_str_split(VtString *s, VtString *sep) {
    VtArray *a = vt_arr_new(sizeof(VtString *), true);
    if (sep->len == 0) {
        VtString *whole = vt_str_new(s->data, s->len);
        vt_arr_push(a, &whole);
        vt_release(whole);
        return a;
    }
    int64_t start = 0;
    for (;;) {
        int64_t hit = str_find(s->data, s->len, sep->data, sep->len, start);
        int64_t stop = hit < 0 ? s->len : hit;
        VtString *piece = vt_str_new(s->data + start, stop - start);
        vt_arr_push(a, &piece);
        vt_release(piece);
        if (hit < 0) break;
        start = hit + sep->len;
    }
    return a;
}

/* split into lines on '\n', stripping a trailing '\r' (CRLF) and not emitting a
 * spurious empty final element when the text ends in a newline. */
VtArray *vt_str_lines(VtString *s) {
    VtArray *a = vt_arr_new(sizeof(VtString *), true);
    const char *p = s->data, *end = s->data + s->len;
    while (p < end) {
        const char *nl = p;
        while (nl < end && *nl != '\n') nl++;
        if (nl == end) nl = NULL;
        const char *stop = nl ? nl : end;
        int64_t l = (int64_t)(stop - p);
        if (l > 0 && stop[-1] == '\r') l--;
        VtString *line = vt_str_new(p, l);
        vt_arr_push(a, &line);
        vt_release(line);
        if (!nl) break;
        p = nl + 1;
    }
    return a;
}

int64_t vt_str_to_int(VtString *s, const char *file, int line) {
    int64_t i = 0, n = s->len;
    while (i < n && str_is_space(s->data[i])) i++;
    bool neg = false;
    if (i < n && (s->data[i] == '+' || s->data[i] == '-')) { neg = s->data[i] == '-'; i++; }
    if (i >= n || s->data[i] < '0' || s->data[i] > '9')
        vt_panic_c(file, line, "to_int: not an integer");
    /* accumulate unsigned so the limit check covers INT64_MIN exactly */
    uint64_t v = 0, lim = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    while (i < n && s->data[i] >= '0' && s->data[i] <= '9') {
        uint64_t d = (uint64_t)(s->data[i++] - '0');
        if (v > (lim - d) / 10) vt_panic_c(file, line, "to_int: integer overflow");
        v = v * 10 + d;
    }
    while (i < n && str_is_space(s->data[i])) i++;
    if (i != n) vt_panic_c(file, line, "to_int: trailing characters");
    if (neg) return v == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)v;
    return (int64_t)v;
}

double vt_str_to_float(VtString *s, const char *file, int line) {
    int64_t i = 0, n = s->len;
    while (i < n && str_is_space(s->data[i])) i++;
    bool neg = false;
    if (i < n && (s->data[i] == '+' || s->data[i] == '-')) { neg = s->data[i] == '-'; i++; }
    bool any = false;
    double v = 0.0;
    while (i < n && s->data[i] >= '0' && s->data[i] <= '9') { v = v * 10.0 + (s->data[i++] - '0'); any = true; }
    if (i < n && s->data[i] == '.') {
        i++;
        double scale = 0.1;
        while (i < n && s->data[i] >= '0' && s->data[i] <= '9') {
            v += (s->data[i++] - '0') * scale; scale *= 0.1; any = true;
        }
    }
    if (!any) vt_panic_c(file, line, "to_float: not a number");
    if (i < n && (s->data[i] == 'e' || s->data[i] == 'E')) {
        i++;
        bool eneg = false;
        if (i < n && (s->data[i] == '+' || s->data[i] == '-')) { eneg = s->data[i] == '-'; i++; }
        if (i >= n || s->data[i] < '0' || s->data[i] > '9')
            vt_panic_c(file, line, "to_float: bad exponent");
        int exp = 0;
        while (i < n && s->data[i] >= '0' && s->data[i] <= '9') exp = exp * 10 + (s->data[i++] - '0');
        double p10 = 1.0;
        for (int k = 0; k < exp; k++) p10 *= 10.0;
        v = eneg ? v / p10 : v * p10;
    }
    while (i < n && str_is_space(s->data[i])) i++;
    if (i != n) vt_panic_c(file, line, "to_float: trailing characters");
    return neg ? -v : v;
}
