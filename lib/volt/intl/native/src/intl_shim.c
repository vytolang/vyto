/* volt/intl native backing — ICU C API (icuuc + icui18n). See intl_shim.h.
 *
 * The whole file speaks C linkage only (no <unicode/*.h> C++ headers), so the
 * final link needs just -licuuc -licui18n; libstdc++ is pulled transitively by
 * the ICU shared objects and never has to appear on voltc's link line.
 *
 * Buffer protocol for the text-producing entries: they write UTF-8 into the
 * caller's `out`/`cap` and return the *full* number of bytes the result needs
 * (NUL-terminated when it fits). If the return value is >= cap the output was
 * truncated and the Volt caller retries with a bytes(ret+1) buffer; a negative
 * return is a hard ICU failure. */

#include <stdlib.h>
#include <string.h>

#include <unicode/utypes.h>
#include <unicode/ustring.h>
#include <unicode/unorm2.h>
#include <unicode/ubrk.h>
#include <unicode/ucol.h>
#include <unicode/unum.h>
#include <unicode/udat.h>
#include <unicode/upluralrules.h>
#include <unicode/utf8.h>
#include <unicode/utf16.h>

/* ---- UTF-8 <-> UTF-16 helpers ------------------------------------------ */

/* Convert UTF-8 -> a freshly malloc'd UTF-16 buffer (caller frees). slen<0
   means NUL-terminated. Sets *outLen to the UTF-16 length. NULL on failure. */
static UChar *to_u16(const char *s, int32_t slen, int32_t *outLen) {
    UErrorCode e = U_ZERO_ERROR;
    int32_t need = 0;
    u_strFromUTF8(NULL, 0, &need, s, slen, &e);
    if (e != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(e)) return NULL;
    UChar *u = (UChar *)malloc(sizeof(UChar) * (size_t)(need + 1));
    if (!u) return NULL;
    e = U_ZERO_ERROR;
    u_strFromUTF8(u, need + 1, outLen, s, slen, &e);
    if (U_FAILURE(e)) { free(u); return NULL; }
    return u;
}

/* Write UTF-16 -> caller's UTF-8 buffer. Returns the full needed byte length
   (>= cap means truncated), or -1 on a hard failure. NUL-terminates when it
   fits. */
static int32_t u16_to_u8(const UChar *u, int32_t ul, char *out, int32_t cap) {
    UErrorCode e = U_ZERO_ERROR;
    int32_t need = 0;
    u_strToUTF8(out, cap, &need, u, ul, &e);
    if (U_FAILURE(e) && e != U_BUFFER_OVERFLOW_ERROR &&
        e != U_STRING_NOT_TERMINATED_WARNING)
        return -1;
    if (need < cap) out[need] = 0;
    else if (cap > 0) out[cap - 1] = 0;
    return need;
}

/* ---- Unicode text ------------------------------------------------------- */

int32_t intl_utf8_to_cp(const char *s, int32_t slen, int32_t *out, int32_t cap) {
    if (slen < 0) slen = (int32_t)strlen(s);
    int32_t i = 0, n = 0;
    while (i < slen) {
        UChar32 c;
        U8_NEXT(s, i, slen, c);
        if (c < 0) return -1; /* ill-formed UTF-8 */
        if (n < cap) out[n] = (int32_t)c;
        n++;
    }
    return n;
}

int32_t intl_normalize(const char *s, int32_t mode, char *out, int32_t cap) {
    UErrorCode e = U_ZERO_ERROR;
    const UNormalizer2 *nm;
    switch (mode) {
        case 0: nm = unorm2_getNFCInstance(&e);  break;
        case 1: nm = unorm2_getNFDInstance(&e);  break;
        case 2: nm = unorm2_getNFKCInstance(&e); break;
        case 3: nm = unorm2_getNFKDInstance(&e); break;
        default: return -1;
    }
    if (U_FAILURE(e)) return -1;
    int32_t ul;
    UChar *u = to_u16(s, -1, &ul);
    if (!u) return -1;
    int32_t dcap = ul * 3 + 16;
    UChar *d = (UChar *)malloc(sizeof(UChar) * (size_t)dcap);
    if (!d) { free(u); return -1; }
    e = U_ZERO_ERROR;
    int32_t dl = unorm2_normalize(nm, u, ul, d, dcap, &e);
    if (e == U_BUFFER_OVERFLOW_ERROR) {
        free(d);
        d = (UChar *)malloc(sizeof(UChar) * (size_t)(dl + 1));
        if (!d) { free(u); return -1; }
        e = U_ZERO_ERROR;
        dl = unorm2_normalize(nm, u, ul, d, dl + 1, &e);
    }
    free(u);
    if (U_FAILURE(e)) { free(d); return -1; }
    int32_t r = u16_to_u8(d, dl, out, cap);
    free(d);
    return r;
}

int32_t intl_case(const char *s, const char *locale, int32_t op, char *out, int32_t cap) {
    int32_t ul;
    UChar *u = to_u16(s, -1, &ul);
    if (!u) return -1;
    int32_t dcap = ul * 2 + 16;
    UChar *d = (UChar *)malloc(sizeof(UChar) * (size_t)dcap);
    if (!d) { free(u); return -1; }
    UErrorCode e = U_ZERO_ERROR;
    int32_t dl;
    if (op == 0)      dl = u_strToUpper(d, dcap, u, ul, locale, &e);
    else if (op == 1) dl = u_strToLower(d, dcap, u, ul, locale, &e);
    else              dl = u_strFoldCase(d, dcap, u, ul, U_FOLD_CASE_DEFAULT, &e);
    if (e == U_BUFFER_OVERFLOW_ERROR) {
        free(d);
        d = (UChar *)malloc(sizeof(UChar) * (size_t)(dl + 1));
        if (!d) { free(u); return -1; }
        e = U_ZERO_ERROR;
        if (op == 0)      dl = u_strToUpper(d, dl + 1, u, ul, locale, &e);
        else if (op == 1) dl = u_strToLower(d, dl + 1, u, ul, locale, &e);
        else              dl = u_strFoldCase(d, dl + 1, u, ul, U_FOLD_CASE_DEFAULT, &e);
    }
    free(u);
    if (U_FAILURE(e)) { free(d); return -1; }
    int32_t r = u16_to_u8(d, dl, out, cap);
    free(d);
    return r;
}

/* Break-iterator handle: owns the UTF-16 text (ubrk references it) plus a
   UTF-16-index -> UTF-8-byte-offset table so boundaries come back as byte
   offsets into the caller's original UTF-8 string. */
typedef struct {
    UBreakIterator *bi;
    UChar          *u16;
    int32_t         u16len;
    int32_t        *u8off; /* length u16len+1 */
} BrkH;

void *intl_brk_open(int32_t kind, const char *locale, const char *s) {
    UBreakIteratorType t = kind == 0 ? UBRK_CHARACTER
                         : kind == 1 ? UBRK_WORD
                         : kind == 2 ? UBRK_LINE
                                     : UBRK_SENTENCE;
    int32_t ul;
    UChar *u = to_u16(s, -1, &ul);
    if (!u) return NULL;
    UErrorCode e = U_ZERO_ERROR;
    UBreakIterator *bi = ubrk_open(t, locale, u, ul, &e);
    if (U_FAILURE(e)) { free(u); return NULL; }
    int32_t *off = (int32_t *)malloc(sizeof(int32_t) * (size_t)(ul + 1));
    if (!off) { ubrk_close(bi); free(u); return NULL; }
    int32_t i = 0, byte = 0;
    while (i < ul) {
        int32_t prev = i;
        UChar32 c;
        U16_NEXT(u, i, ul, c);
        off[prev] = byte;
        if (i == prev + 2) off[prev + 1] = byte; /* trailing surrogate unit */
        byte += U8_LENGTH(c);
    }
    off[ul] = byte;
    BrkH *h = (BrkH *)malloc(sizeof(BrkH));
    if (!h) { ubrk_close(bi); free(u); free(off); return NULL; }
    h->bi = bi; h->u16 = u; h->u16len = ul; h->u8off = off;
    ubrk_first(bi);
    return h;
}

int32_t intl_brk_next(void *hv) {
    BrkH *h = (BrkH *)hv;
    int32_t idx = ubrk_next(h->bi);
    if (idx == UBRK_DONE) return -1;
    return h->u8off[idx];
}

void intl_brk_close(void *hv) {
    if (!hv) return;
    BrkH *h = (BrkH *)hv;
    ubrk_close(h->bi);
    free(h->u16);
    free(h->u8off);
    free(h);
}

void *intl_col_open(const char *locale) {
    UErrorCode e = U_ZERO_ERROR;
    UCollator *c = ucol_open(locale, &e);
    if (U_FAILURE(e)) { if (c) ucol_close(c); return NULL; }
    return c;
}

int32_t intl_col_compare(void *h, const char *a, const char *b) {
    UErrorCode e = U_ZERO_ERROR;
    UCollationResult r = ucol_strcollUTF8((const UCollator *)h, a, -1, b, -1, &e);
    if (U_FAILURE(e)) return -2;
    return r == UCOL_LESS ? -1 : r == UCOL_GREATER ? 1 : 0;
}

int32_t intl_col_sortkey(void *h, const char *s, char *out, int32_t cap) {
    int32_t ul;
    UChar *u = to_u16(s, -1, &ul);
    if (!u) return -1;
    /* full needed length (>= cap => truncated; caller retries) */
    int32_t n = ucol_getSortKey((const UCollator *)h, u, ul, (uint8_t *)out, cap);
    free(u);
    return n;
}

void intl_col_close(void *h) { if (h) ucol_close((UCollator *)h); }

/* ---- Number / currency -------------------------------------------------- */

void *intl_num_open(const char *locale, int32_t style) {
    UNumberFormatStyle st = style == 0 ? UNUM_DECIMAL
                          : style == 1 ? UNUM_CURRENCY
                          : style == 2 ? UNUM_PERCENT
                                       : UNUM_SCIENTIFIC;
    UErrorCode e = U_ZERO_ERROR;
    UNumberFormat *f = unum_open(st, NULL, 0, locale, NULL, &e);
    if (U_FAILURE(e)) { if (f) unum_close(f); return NULL; }
    return f;
}

void intl_num_close(void *h) { if (h) unum_close((UNumberFormat *)h); }

int32_t intl_num_fmt_double(void *h, double v, char *out, int32_t cap) {
    UErrorCode e = U_ZERO_ERROR;
    UChar d[256];
    int32_t dl = unum_formatDouble((const UNumberFormat *)h, v, d, 256, NULL, &e);
    if (U_FAILURE(e)) return -1;
    return u16_to_u8(d, dl, out, cap);
}

int32_t intl_num_fmt_int(void *h, int64_t v, char *out, int32_t cap) {
    UErrorCode e = U_ZERO_ERROR;
    UChar d[256];
    int32_t dl = unum_formatInt64((const UNumberFormat *)h, v, d, 256, NULL, &e);
    if (U_FAILURE(e)) return -1;
    return u16_to_u8(d, dl, out, cap);
}

int32_t intl_num_fmt_currency(void *h, double v, const char *iso3, char *out, int32_t cap) {
    UChar cur[8];
    int32_t cl;
    UErrorCode e = U_ZERO_ERROR;
    u_strFromUTF8(cur, 8, &cl, iso3, -1, &e);
    if (U_FAILURE(e)) return -1;
    UChar d[256];
    e = U_ZERO_ERROR;
    int32_t dl = unum_formatDoubleCurrency((const UNumberFormat *)h, v, cur, d, 256, NULL, &e);
    if (U_FAILURE(e)) return -1;
    return u16_to_u8(d, dl, out, cap);
}

void intl_num_set_fraction(void *h, int32_t minFrac, int32_t maxFrac) {
    if (minFrac >= 0) unum_setAttribute((UNumberFormat *)h, UNUM_MIN_FRACTION_DIGITS, minFrac);
    if (maxFrac >= 0) unum_setAttribute((UNumberFormat *)h, UNUM_MAX_FRACTION_DIGITS, maxFrac);
}

/* ---- Date / time -------------------------------------------------------- */

static UDateFormatStyle map_dstyle(int32_t s) {
    switch (s) {
        case 0:  return UDAT_FULL;
        case 1:  return UDAT_LONG;
        case 2:  return UDAT_MEDIUM;
        case 3:  return UDAT_SHORT;
        default: return UDAT_NONE;
    }
}

void *intl_dat_open(const char *locale, int32_t dateStyle, int32_t timeStyle, const char *tz) {
    UChar tzbuf[128];
    UChar *ztz = NULL;
    int32_t ztzl = -1;
    if (tz && tz[0]) {
        UErrorCode e0 = U_ZERO_ERROR;
        u_strFromUTF8(tzbuf, 128, &ztzl, tz, -1, &e0);
        if (U_SUCCESS(e0)) ztz = tzbuf; else ztzl = -1;
    }
    UErrorCode e = U_ZERO_ERROR;
    /* udat_open takes timeStyle first, then dateStyle */
    UDateFormat *f = udat_open(map_dstyle(timeStyle), map_dstyle(dateStyle),
                              locale, ztz, ztzl, NULL, 0, &e);
    if (U_FAILURE(e)) { if (f) udat_close(f); return NULL; }
    return f;
}

void intl_dat_close(void *h) { if (h) udat_close((UDateFormat *)h); }

int32_t intl_dat_fmt(void *h, int64_t unix_ms, char *out, int32_t cap) {
    UErrorCode e = U_ZERO_ERROR;
    UChar d[512];
    int32_t dl = udat_format((const UDateFormat *)h, (UDate)unix_ms, d, 512, NULL, &e);
    if (U_FAILURE(e)) return -1;
    return u16_to_u8(d, dl, out, cap);
}

/* ---- Plural rules ------------------------------------------------------- */

void *intl_plural_open(const char *locale, int32_t kind) {
    UErrorCode e = U_ZERO_ERROR;
    UPluralType t = kind == 1 ? UPLURAL_TYPE_ORDINAL : UPLURAL_TYPE_CARDINAL;
    UPluralRules *p = uplrules_openForType(locale, t, &e);
    if (U_FAILURE(e)) { if (p) uplrules_close(p); return NULL; }
    return p;
}

void intl_plural_close(void *h) { if (h) uplrules_close((UPluralRules *)h); }

int32_t intl_plural_select(void *h, double n, char *out, int32_t cap) {
    UErrorCode e = U_ZERO_ERROR;
    UChar d[32];
    int32_t dl = uplrules_select((const UPluralRules *)h, n, d, 32, &e);
    if (U_FAILURE(e)) return -1;
    return u16_to_u8(d, dl, out, cap);
}
