/* volt/intl native backing — ICU (icuuc + icui18n).
 *
 * The boundary is strictly UTF-8: every entry takes UTF-8 `char*` in and writes
 * UTF-8 into a caller-provided buffer (`out`,`cap` = a Volt `bytes(cap)`), and
 * ICU's own types (UChar, UErrorCode, UCollator*, …) never cross into Volt.
 * Stateful ICU objects (collators, break iterators, formatters, message
 * formats, plural rules) are handed back as opaque `void*` handles.
 *
 * Return convention for the *_fmt/*_case/*_normalize family: the number of
 * UTF-8 bytes written (NUL-terminated, never exceeding cap-1), or a negative
 * value on failure. Handle openers return NULL on failure. This mirrors
 * volt/util/fmt's bounded-buffer shim and volt/gfx's opaque-handle model.
 */
#ifndef VOLT_INTL_SHIM_H
#define VOLT_INTL_SHIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Unicode text ------------------------------------------------------- */

/* Decode UTF-8 into codepoints. `out` is an int32_t buffer of `cap` entries.
   Returns the codepoint count (may exceed cap — then out is filled to cap and
   the count still reports the true length), or -1 on a decode error. */
int32_t intl_utf8_to_cp(const char *s, int32_t slen, int32_t *out, int32_t cap);

/* Normalize `s` to a form: 0=NFC 1=NFD 2=NFKC 3=NFKD. */
int32_t intl_normalize(const char *s, int32_t mode, char *out, int32_t cap);

/* Case map `s` under `locale` ("" = root). op: 0=upper 1=lower 2=foldCase. */
int32_t intl_case(const char *s, const char *locale, int32_t op, char *out, int32_t cap);

/* Break iterator over `s`. kind: 0=CHARACTER 1=WORD 2=LINE 3=SENTENCE.
   The handle owns an internal UTF-16 copy of `s`; free with intl_brk_close. */
void   *intl_brk_open(int32_t kind, const char *locale, const char *s);
/* Next boundary as a UTF-8 byte offset into the original string, or -1 at end. */
int32_t intl_brk_next(void *h);
void    intl_brk_close(void *h);

/* Collator for `locale` ("" = root). */
void   *intl_col_open(const char *locale);
/* -1 / 0 / 1 ordering of UTF-8 `a` vs `b`. */
int32_t intl_col_compare(void *h, const char *a, const char *b);
/* Binary sort key of `s` into `out` (raw bytes, NUL-terminated by ICU). */
int32_t intl_col_sortkey(void *h, const char *s, char *out, int32_t cap);
void    intl_col_close(void *h);

/* ---- Number / currency -------------------------------------------------- */

/* style: 0=DECIMAL 1=CURRENCY 2=PERCENT 3=SCIENTIFIC. */
void   *intl_num_open(const char *locale, int32_t style);
void    intl_num_close(void *h);
int32_t intl_num_fmt_double(void *h, double v, char *out, int32_t cap);
int32_t intl_num_fmt_int(void *h, int64_t v, char *out, int32_t cap);
/* iso3 = 3-letter ISO 4217 code ("EUR"); uses the formatter's locale rules. */
int32_t intl_num_fmt_currency(void *h, double v, const char *iso3, char *out, int32_t cap);
/* Clamp fraction digits (applies to subsequent formats); <0 leaves default. */
void    intl_num_set_fraction(void *h, int32_t minFrac, int32_t maxFrac);

/* ---- Date / time -------------------------------------------------------- */

/* dateStyle/timeStyle: 0=FULL 1=LONG 2=MEDIUM 3=SHORT 4=NONE.
   tz: IANA id ("Asia/Tokyo") or "" for the default zone. */
void   *intl_dat_open(const char *locale, int32_t dateStyle, int32_t timeStyle, const char *tz);
void    intl_dat_close(void *h);
/* Format an instant given as Unix milliseconds. */
int32_t intl_dat_fmt(void *h, int64_t unix_ms, char *out, int32_t cap);

/* ---- Plural rules ------------------------------------------------------- */
/* ICU's C MessageFormat entry (umsg_format) is varargs and cannot be driven
   from a tagged argument array in portable C, so volt/intl/message.vt builds
   the MessageFormat subset itself on top of these plural-selection and the
   number-formatting primitives above — ICU still owns the CLDR-hard parts. */

/* Plural rules. kind: 0=CARDINAL 1=ORDINAL. */
void   *intl_plural_open(const char *locale, int32_t kind);
void    intl_plural_close(void *h);
/* Select keyword ("zero"/"one"/"two"/"few"/"many"/"other") for `n`. */
int32_t intl_plural_select(void *h, double n, char *out, int32_t cap);

#ifdef __cplusplus
}
#endif

#endif /* VOLT_INTL_SHIM_H */
