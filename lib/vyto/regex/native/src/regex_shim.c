/* regex_shim.c — the C half of vyto/regex.
 *
 * PCRE2 lives behind this file. Its types (pcre2_code, pcre2_match_data,
 * PCRE2_SPTR, PCRE2_SIZE) and its option and error numbering never cross into
 * Vyto: every entry point below takes and returns plain scalars, cstrings and
 * caller-owned byte buffers, and translates our own RX_* bitset to PCRE2's
 * options on the way in and PCRE2's error codes to our own on the way out.
 * That way a PCRE2 version bump cannot change a number the Vyto side sees.
 *
 * Two protocols, both shared with the rest of the stdlib:
 *
 *   Strings cross as an explicit (pointer, byte length) pair, never as a
 *   NUL-terminated cstring alone. Vyto strings carry their length, so patterns
 *   and subjects containing NUL bytes work — vyto/regex deliberately does not
 *   inherit the "truncates at the first NUL" caveat that vyto/util/text.vt:12
 *   documents for the cstr()-only path.
 *
 *   Buffer-filling entries (vre_error_message, vre_name_at, vre_substitute)
 *   write into the caller's out/cap and return the *full* number of bytes the
 *   result needs, NUL-terminated when it fits. A return >= cap means the output
 *   was truncated and the Vyto caller retries once with a bytes(ret+1) buffer.
 *   Negative is a hard failure. Same contract as
 *   lib/vyto/intl/native/src/intl_shim.c:7-11.
 *
 * This file is first-party. Everything under native/src/pcre2/ is upstream and
 * byte-identical to the release; see native/refresh-pcre2.sh.
 */

#define PCRE2_CODE_UNIT_WIDTH 8
#define HAVE_CONFIG_H
#include "config.h"          /* PCRE2_STATIC, SUPPORT_JIT, … */
#include "pcre2.h"

#include <stdlib.h>
#include <string.h>

/* ---- our own option bits (must match the RX_* consts in regex.vt) ---- */
#define RX_CASELESS         1
#define RX_MULTILINE        2
#define RX_DOTALL           4
#define RX_EXTENDED         8
#define RX_UNGREEDY        16
#define RX_ANCHORED        32
#define RX_DOLLAR_ENDONLY  64
#define RX_FIRSTLINE      128
#define RX_NO_AUTO_CAPTURE 256
#define RX_BYTES           512   /* no UTF, no UCP: treat the subject as bytes */

#define RX_SUB_GLOBAL        1
#define RX_SUB_EXTENDED      2
#define RX_SUB_UNSET_EMPTY   4

/* ---- vre_match / vre_cached_find return codes ----
   >0 is the number of ovector pairs filled. The negatives are ours, not
   PCRE2's, so the Vyto side never sees a PCRE2 error number. */
#define VRE_NOMATCH   (-1)
#define VRE_LIMIT     (-2)
#define VRE_BADUTF    (-3)
#define VRE_INTERNAL  (-4)

/* Runtime limits applied to every context we create. Deliberately far below
   PCRE2's compile-time ceilings in config.h: those are "don't hang forever"
   backstops, these are "a hostile pattern must fail in bounded time". */
#define VRE_MATCH_LIMIT   1000000
#define VRE_DEPTH_LIMIT     10000
#define VRE_HEAP_LIMIT_KB   16384

typedef struct {
    pcre2_code         *code;
    pcre2_match_data   *md;
    pcre2_match_context *mc;
    uint32_t            ngroups;
    int                 jit;        /* JIT actually compiled for this pattern */
    int                 utf;        /* UTF/UCP mode on */
    /* Subject the ovector currently describes, and whether it has been UTF
       validated. See the note above vre_match. */
    const void         *val_ptr;
    size_t              val_len;
    int                 validated;
    int                 rc;         /* pair count from the last successful match */
} VRe;

/* Last compile failure. Single-threaded by construction — see the cache note. */
static int    g_errcode;
static size_t g_erroffset;

/* ------------------------------------------------------------------ options */

static uint32_t compile_opts(int flags, int *utf_out)
{
    uint32_t o = 0;
    if (flags & RX_CASELESS)         o |= PCRE2_CASELESS;
    if (flags & RX_MULTILINE)        o |= PCRE2_MULTILINE;
    if (flags & RX_DOTALL)           o |= PCRE2_DOTALL;
    if (flags & RX_EXTENDED)         o |= PCRE2_EXTENDED;
    if (flags & RX_UNGREEDY)         o |= PCRE2_UNGREEDY;
    if (flags & RX_ANCHORED)         o |= PCRE2_ANCHORED;
    if (flags & RX_DOLLAR_ENDONLY)   o |= PCRE2_DOLLAR_ENDONLY;
    if (flags & RX_FIRSTLINE)        o |= PCRE2_FIRSTLINE;
    if (flags & RX_NO_AUTO_CAPTURE)  o |= PCRE2_NO_AUTO_CAPTURE;

    /* Unicode is the default: vyto strings are UTF-8 and vyto/intl already
       commits the stdlib to Unicode semantics, so \w and \b must mean the
       Unicode thing unless the caller explicitly asks for bytes. */
    if (flags & RX_BYTES) { *utf_out = 0; }
    else                  { *utf_out = 1; o |= PCRE2_UTF | PCRE2_UCP; }
    return o;
}

/* PCRE2's error numbers, collapsed to the four outcomes the Vyto side models.
   Anything that means "this pattern would run forever" becomes VRE_LIMIT,
   including the JIT running out of stack — from the caller's point of view it
   is the same failure with the same remedy. */
static int classify(int rc)
{
    switch (rc) {
        case PCRE2_ERROR_NOMATCH:
        case PCRE2_ERROR_PARTIAL:
            return VRE_NOMATCH;
        case PCRE2_ERROR_MATCHLIMIT:
        case PCRE2_ERROR_DEPTHLIMIT:
        case PCRE2_ERROR_HEAPLIMIT:
        case PCRE2_ERROR_JIT_STACKLIMIT:
            return VRE_LIMIT;
        default:
            /* The 21 UTF-8 validity errors are a contiguous block running from
               PCRE2_ERROR_UTF8_ERR1 (-3) down to PCRE2_ERROR_UTF8_ERR21 (-23),
               so ERR21 is the *lower* bound. */
            if (rc >= PCRE2_ERROR_UTF8_ERR21 && rc <= PCRE2_ERROR_UTF8_ERR1)
                return VRE_BADUTF;
            return VRE_INTERNAL;
    }
}

/* ------------------------------------------------------------- the JIT stack
   One per process, created on first use, never freed. Freeing it at exit would
   be busywork: the OS reclaims it, and there is no point in the teardown order
   risk. sljit allocates executable memory here; if that is refused (a hardened
   runtime, a W^X kiosk) pcre2_jit_compile fails and pcre2_match falls back to
   the interpreter on its own, which is why no caller treats it as an error. */
#ifdef SUPPORT_JIT
static pcre2_jit_stack *g_jit_stack;
static int g_jit_disabled = -1;      /* -1 = not yet read from the environment */

static int jit_enabled(void)
{
    if (g_jit_disabled < 0) {
        const char *e = getenv("VYTO_REGEX_JIT");
        g_jit_disabled = (e != NULL && e[0] == '0' && e[1] == '\0');
    }
    return !g_jit_disabled;
}

static void jit_attach(VRe *h)
{
    if (!jit_enabled()) return;
    if (pcre2_jit_compile(h->code, PCRE2_JIT_COMPLETE) != 0) return;
    if (g_jit_stack == NULL)
        g_jit_stack = pcre2_jit_stack_create(32 * 1024, 512 * 1024, NULL);
    if (g_jit_stack != NULL)
        pcre2_jit_stack_assign(h->mc, NULL, g_jit_stack);
    h->jit = 1;
}
#else
static void jit_attach(VRe *h) { (void)h; }
#endif

/* ---------------------------------------------------------- compile and free */

static VRe *compile_one(const char *pat, long patlen, int flags)
{
    int utf = 0;
    uint32_t opts = compile_opts(flags, &utf);
    VRe *h;

    g_errcode = 0;
    g_erroffset = 0;

    h = (VRe *)calloc(1, sizeof *h);
    if (h == NULL) return NULL;

    h->code = pcre2_compile((PCRE2_SPTR)pat, (PCRE2_SIZE)patlen, opts,
                            &g_errcode, (PCRE2_SIZE *)&g_erroffset, NULL);
    if (h->code == NULL) { free(h); return NULL; }

    h->mc = pcre2_match_context_create(NULL);
    h->md = pcre2_match_data_create_from_pattern(h->code, NULL);
    if (h->mc == NULL || h->md == NULL) {
        if (h->mc) pcre2_match_context_free(h->mc);
        if (h->md) pcre2_match_data_free(h->md);
        pcre2_code_free(h->code);
        free(h);
        return NULL;
    }

    pcre2_set_match_limit(h->mc, VRE_MATCH_LIMIT);
    pcre2_set_depth_limit(h->mc, VRE_DEPTH_LIMIT);
    pcre2_set_heap_limit(h->mc, VRE_HEAP_LIMIT_KB);

    pcre2_pattern_info(h->code, PCRE2_INFO_CAPTURECOUNT, &h->ngroups);
    h->utf = utf;
    jit_attach(h);
    return h;
}

static void free_one(VRe *h)
{
    if (h == NULL) return;
    pcre2_match_data_free(h->md);
    pcre2_match_context_free(h->mc);
    pcre2_code_free(h->code);
    free(h);
}

void *vre_compile(const char *pat, long patlen, int flags)
{
    return compile_one(pat, patlen, flags);
}

void vre_free(void *vh) { free_one((VRe *)vh); }

int  vre_last_errcode(void)   { return g_errcode; }
long vre_last_erroffset(void) { return (long)g_erroffset; }

/* PCRE2's own message text. Only ever shown to a human: the Vyto side keys its
   own behaviour off the RX_ERR_* classification, never off this string, so a
   PCRE2 release rewording a message cannot break a golden test. */
long vre_error_message(int code, char *out, long cap)
{
    PCRE2_UCHAR buf[256];
    int n = pcre2_get_error_message(code, buf, sizeof buf);
    long len;
    if (n < 0) { buf[0] = 0; n = 0; }
    len = (long)strlen((char *)buf);
    if (out != NULL && cap > 0) {
        long copy = len < cap - 1 ? len : cap - 1;
        memcpy(out, buf, (size_t)copy);
        out[copy] = 0;
    }
    return len;
}

int vre_jit_ok(void *vh) { return vh != NULL && ((VRe *)vh)->jit; }

void vre_set_limits(void *vh, long match, long depth, long heapKB)
{
    VRe *h = (VRe *)vh;
    if (h == NULL) return;
    if (match  > 0) pcre2_set_match_limit(h->mc, (uint32_t)match);
    if (depth  > 0) pcre2_set_depth_limit(h->mc, (uint32_t)depth);
    if (heapKB > 0) pcre2_set_heap_limit(h->mc, (uint32_t)heapKB);
}

int vre_config_jit(void)
{
    uint32_t v = 0;
    pcre2_config(PCRE2_CONFIG_JIT, &v);
    return (int)v;
}

/* ----------------------------------------------------------------- matching */

/* With PCRE2_UTF set, pcre2_match validates the *whole* subject on every call.
   In a findAll loop that is quadratic in the subject length, which is the
   single biggest performance trap in this module. Upstream's documented
   scanning idiom is to validate once and pass PCRE2_NO_UTF_CHECK for the rest
   of the scan.
   NO_UTF_CHECK on genuinely invalid UTF-8 is undefined behaviour, so the skip
   is deliberately narrow: it applies only to a continuation call (start > 0) on
   the exact same buffer that a previous call already validated successfully. A
   fresh match at offset 0 always revalidates. */
static int match_at(VRe *h, const char *subj, long len, long start, uint32_t extra)
{
    uint32_t opts = extra;
    int rc;

    if (h->utf && start > 0 && h->validated &&
        h->val_ptr == (const void *)subj && h->val_len == (size_t)len)
        opts |= PCRE2_NO_UTF_CHECK;

    rc = pcre2_match(h->code, (PCRE2_SPTR)subj, (PCRE2_SIZE)len,
                     (PCRE2_SIZE)start, opts, h->md, h->mc);

    if (rc > 0) {
        h->rc = rc;
        h->val_ptr = subj;
        h->val_len = (size_t)len;
        h->validated = 1;
        return rc;
    }
    if (rc == 0) return VRE_INTERNAL;   /* ovector sized from the pattern: impossible */
    return classify(rc);
}

int vre_match(void *vh, const char *subj, long len, long start, int flags)
{
    VRe *h = (VRe *)vh;
    (void)flags;
    if (h == NULL) return VRE_INTERNAL;
    if (start < 0 || start > len) return VRE_NOMATCH;
    return match_at(h, subj, len, start, 0);
}

/* The zero-width-match step. A pattern that can match empty (a*, \b, (?=x))
   returns the same empty match at the same offset forever if the caller just
   retries. Upstream's fix, which this implements, is to first retry at the same
   offset forbidding an empty match there, and only then to step forward — by a
   whole character, not a byte, and over a CRLF pair as one unit. Doing it here
   rather than in Vyto keeps the code-unit knowledge on the side that has it. */
int vre_match_next(void *vh, const char *subj, long len, long start)
{
    VRe *h = (VRe *)vh;
    int rc;
    if (h == NULL) return VRE_INTERNAL;
    if (start > len) return VRE_NOMATCH;
    rc = match_at(h, subj, len, start,
                  PCRE2_NOTEMPTY_ATSTART | PCRE2_ANCHORED);
    if (rc != VRE_NOMATCH) return rc;
    return VRE_NOMATCH;
}

long vre_advance(void *vh, const char *subj, long len, long pos)
{
    VRe *h = (VRe *)vh;
    if (pos >= len) return len + 1;

    /* A CRLF is one newline; never split it. */
    if (subj[pos] == '\r' && pos + 1 < len && subj[pos + 1] == '\n')
        return pos + 2;

    if (h != NULL && h->utf) {
        long p = pos + 1;
        while (p < len && ((unsigned char)subj[p] & 0xc0) == 0x80) p++;
        return p;
    }
    return pos + 1;
}

/* Count matches without building anything.
 *
 * This exists in C rather than as findAll(s).len on the Vyto side for one
 * reason: allocation. Counting through findAll allocates a Match object and
 * four arrays per hit, which for a scan over a large log is the whole cost.
 * Here the loop touches no memory at all.
 *
 * The stepping below is the same three-step protocol vre_match /
 * vre_match_next / vre_advance implement, deliberately spelled out against the
 * same helpers rather than reimplemented: if the two ever diverge, count() and
 * findAll().len would disagree on any pattern that can match empty (a*, \b,
 * (?=x)), which is exactly the kind of bug nobody finds for a year. */
static long count_in(VRe *h, const char *subj, long len)
{
    long pos = 0, n = 0;
    PCRE2_SIZE *ov;

    if (h == NULL) return VRE_INTERNAL;

    while (pos <= len) {
        long s, e;
        int rc = match_at(h, subj, len, pos, 0);
        if (rc == VRE_NOMATCH) break;
        if (rc < 0) return rc;                  /* limit / bad UTF-8 / internal */

        ov = pcre2_get_ovector_pointer(h->md);
        s = (long)ov[0];
        e = (long)ov[1];
        n++;

        if (e > s) { pos = e; continue; }

        /* Empty match: retry anchored and non-empty at the same offset, and
           only then step forward by a whole character. */
        rc = match_at(h, subj, len, e, PCRE2_NOTEMPTY_ATSTART | PCRE2_ANCHORED);
        if (rc > 0) {
            ov = pcre2_get_ovector_pointer(h->md);
            n++;
            pos = (long)ov[1];
            continue;
        }
        if (rc < 0 && rc != VRE_NOMATCH) return rc;
        pos = vre_advance(h, subj, len, e);
    }
    return n;
}

long vre_count(void *vh, const char *subj, long len)
{
    return count_in((VRe *)vh, subj, len);
}

/* Group offsets come out of the match data, which the *next* match overwrites —
   including a failed one. Read every group you want before calling vre_match,
   vre_match_next or vre_advance again. The Vyto side builds a whole Match up
   front for exactly this reason. */
int  vre_group_count(void *vh) { return vh ? (int)((VRe *)vh)->ngroups : 0; }
int  vre_pair_count(void *vh)  { return vh ? ((VRe *)vh)->rc : 0; }

long vre_group_start(void *vh, int i)
{
    VRe *h = (VRe *)vh;
    PCRE2_SIZE *ov;
    if (h == NULL || i < 0 || i >= h->rc) return -1;
    ov = pcre2_get_ovector_pointer(h->md);
    return ov[2 * i] == PCRE2_UNSET ? -1 : (long)ov[2 * i];
}

long vre_group_end(void *vh, int i)
{
    VRe *h = (VRe *)vh;
    PCRE2_SIZE *ov;
    if (h == NULL || i < 0 || i >= h->rc) return -1;
    ov = pcre2_get_ovector_pointer(h->md);
    return ov[2 * i + 1] == PCRE2_UNSET ? -1 : (long)ov[2 * i + 1];
}

/* ------------------------------------------------------------- named groups */

int vre_name_count(void *vh)
{
    VRe *h = (VRe *)vh;
    uint32_t n = 0;
    if (h == NULL) return 0;
    pcre2_pattern_info(h->code, PCRE2_INFO_NAMECOUNT, &n);
    return (int)n;
}

/* The name table is a flat array of fixed-width entries: two bytes of group
   number, big-endian, then the NUL-terminated name. */
long vre_name_at(void *vh, int idx, char *out, long cap)
{
    VRe *h = (VRe *)vh;
    uint32_t count = 0, esize = 0;
    PCRE2_SPTR table = NULL;
    const char *name;
    long len;

    if (h == NULL) return -1;
    pcre2_pattern_info(h->code, PCRE2_INFO_NAMECOUNT, &count);
    if (idx < 0 || (uint32_t)idx >= count) return -1;
    pcre2_pattern_info(h->code, PCRE2_INFO_NAMEENTRYSIZE, &esize);
    pcre2_pattern_info(h->code, PCRE2_INFO_NAMETABLE, &table);
    if (table == NULL) return -1;

    name = (const char *)(table + (size_t)idx * esize + 2);
    len = (long)strlen(name);
    if (out != NULL && cap > 0) {
        long copy = len < cap - 1 ? len : cap - 1;
        memcpy(out, name, (size_t)copy);
        out[copy] = 0;
    }
    return len;
}

int vre_name_number(void *vh, int idx)
{
    VRe *h = (VRe *)vh;
    uint32_t count = 0, esize = 0;
    PCRE2_SPTR table = NULL, e;

    if (h == NULL) return -1;
    pcre2_pattern_info(h->code, PCRE2_INFO_NAMECOUNT, &count);
    if (idx < 0 || (uint32_t)idx >= count) return -1;
    pcre2_pattern_info(h->code, PCRE2_INFO_NAMEENTRYSIZE, &esize);
    pcre2_pattern_info(h->code, PCRE2_INFO_NAMETABLE, &table);
    if (table == NULL) return -1;

    e = table + (size_t)idx * esize;
    return (e[0] << 8) | e[1];
}

/* ------------------------------------------------------------- substitution */

long vre_substitute(void *vh, const char *subj, long slen,
                    const char *repl, long rlen, int flags,
                    char *out, long cap)
{
    VRe *h = (VRe *)vh;
    uint32_t opts = PCRE2_SUBSTITUTE_OVERFLOW_LENGTH;
    PCRE2_SIZE outlen;
    char dummy[1];
    int rc;

    if (h == NULL) return -1;
    if (flags & RX_SUB_GLOBAL)      opts |= PCRE2_SUBSTITUTE_GLOBAL;
    if (flags & RX_SUB_EXTENDED)    opts |= PCRE2_SUBSTITUTE_EXTENDED;
    if (flags & RX_SUB_UNSET_EMPTY) opts |= PCRE2_SUBSTITUTE_UNSET_EMPTY;

    /* PCRE2 refuses a NULL output buffer even when it is only measuring. */
    if (out == NULL || cap <= 0) { out = dummy; cap = 1; }
    outlen = (PCRE2_SIZE)cap;

    /* NULL match data: pcre2_substitute allocates its own. Handing it h->md
       would leave the ovector describing the substitution's last internal match
       instead of the caller's last vre_match, silently corrupting any group
       read that followed. */
    rc = pcre2_substitute(h->code, (PCRE2_SPTR)subj, (PCRE2_SIZE)slen, 0, opts,
                          NULL, h->mc, (PCRE2_SPTR)repl, (PCRE2_SIZE)rlen,
                          (PCRE2_UCHAR *)out, &outlen);

    /* With OVERFLOW_LENGTH, a too-small buffer still reports the length needed
       (including the terminating NUL, which the caller's retry allows for). */
    if (rc == PCRE2_ERROR_NOMEMORY) return (long)outlen;
    if (rc < 0) return classify(rc);   /* -2 limit, -3 bad utf, -4 internal */
    return (long)outlen;
}

/* ------------------------------------------------------------- pattern cache
   Vyto has no module-level mutable state — the declaration kinds in
   src/ast.h:307 are functions, types, constants and imports, with no global
   variable — so the cache behind rx_test/rx_find/rx_replace_all/rx_split has to
   live here.
   No locking, and none needed: vyto/os/worker is fork-based, so a worker gets
   its own copy of this table and there are no threads to race.
   Ownership rule: an entry here is owned by the cache and is never handed out
   as a Regex handle. If it were, Regex.deinit would free a pcre2_code the table
   still points at. That is why the cached path is a separate family of entry
   points rather than a memoising vre_compile. */

#define VRE_CACHE_SLOTS 64

typedef struct {
    VRe   *h;
    char  *pat;
    long   patlen;
    int    flags;
    unsigned hash;
    unsigned clock;      /* bumped on use; lowest is evicted */
} CacheSlot;

static CacheSlot g_cache[VRE_CACHE_SLOTS];
static unsigned  g_clock;
static int       g_cache_used;
static VRe      *g_cache_last;   /* target of the vre_cached_group_* readers */

static unsigned fnv1a(const char *p, long n, int flags)
{
    unsigned h = 2166136261u;
    long i;
    for (i = 0; i < n; i++) { h ^= (unsigned char)p[i]; h *= 16777619u; }
    h ^= (unsigned)flags; h *= 16777619u;
    return h;
}

static VRe *cache_get(const char *pat, long patlen, int flags)
{
    unsigned hash = fnv1a(pat, patlen, flags);
    int i, victim = 0;
    unsigned oldest = 0xffffffffu;

    for (i = 0; i < VRE_CACHE_SLOTS; i++) {
        CacheSlot *s = &g_cache[i];
        if (s->h != NULL && s->hash == hash && s->flags == flags &&
            s->patlen == patlen && memcmp(s->pat, pat, (size_t)patlen) == 0) {
            s->clock = ++g_clock;
            return s->h;
        }
        if (s->h == NULL) { victim = i; oldest = 0; }
        else if (s->clock < oldest) { oldest = s->clock; victim = i; }
    }

    {
        CacheSlot *s = &g_cache[victim];
        VRe *h = compile_one(pat, patlen, flags);
        if (h == NULL) return NULL;          /* leave the slot as it was */
        if (s->h != NULL) { free_one(s->h); free(s->pat); g_cache_used--; }
        s->pat = (char *)malloc((size_t)patlen ? (size_t)patlen : 1);
        if (s->pat == NULL) { free_one(h); return NULL; }
        memcpy(s->pat, pat, (size_t)patlen);
        s->patlen = patlen;
        s->flags  = flags;
        s->hash   = hash;
        s->h      = h;
        s->clock  = ++g_clock;
        g_cache_used++;
        return h;
    }
}

int vre_cache_count(void) { return g_cache_used; }

void vre_cache_clear(void)
{
    int i;
    for (i = 0; i < VRE_CACHE_SLOTS; i++) {
        if (g_cache[i].h != NULL) { free_one(g_cache[i].h); free(g_cache[i].pat); }
        g_cache[i].h = NULL;
        g_cache[i].pat = NULL;
        g_cache[i].patlen = 0;
        g_cache[i].flags = 0;
        g_cache[i].hash = 0;
        g_cache[i].clock = 0;
    }
    g_cache_used = 0;
    g_cache_last = NULL;
}

/* -1 here means "the pattern would not compile"; the caller reads
   vre_last_errcode/vre_last_erroffset exactly as for vre_compile. */
int vre_cached_find(const char *pat, long patlen, int flags,
                    const char *subj, long len, long start)
{
    VRe *h = cache_get(pat, patlen, flags);
    g_cache_last = h;
    if (h == NULL) return VRE_INTERNAL;
    if (start < 0 || start > len) return VRE_NOMATCH;
    return match_at(h, subj, len, start, 0);
}

int vre_cached_next(const char *pat, long patlen, int flags,
                    const char *subj, long len, long start)
{
    VRe *h = cache_get(pat, patlen, flags);
    g_cache_last = h;
    if (h == NULL) return VRE_INTERNAL;
    return vre_match_next(h, subj, len, start);
}

long vre_cached_advance(const char *subj, long len, long pos)
{
    return vre_advance(g_cache_last, subj, len, pos);
}

int  vre_cached_group_count(void)      { return vre_group_count(g_cache_last); }
int  vre_cached_pair_count(void)       { return vre_pair_count(g_cache_last); }
long vre_cached_group_start(int i)     { return vre_group_start(g_cache_last, i); }
long vre_cached_group_end(int i)       { return vre_group_end(g_cache_last, i); }
int  vre_cached_name_count(void)       { return vre_name_count(g_cache_last); }
long vre_cached_name_at(int i, char *out, long cap)
                                       { return vre_name_at(g_cache_last, i, out, cap); }
int  vre_cached_name_number(int i)     { return vre_name_number(g_cache_last, i); }

long vre_cached_substitute(const char *pat, long patlen, int flags,
                           const char *subj, long slen,
                           const char *repl, long rlen, int sflags,
                           char *out, long cap)
{
    VRe *h = cache_get(pat, patlen, flags);
    g_cache_last = h;
    if (h == NULL) return -1;
    return vre_substitute(h, subj, slen, repl, rlen, sflags, out, cap);
}

long vre_cached_count(const char *pat, long patlen, int flags,
                      const char *subj, long len)
{
    VRe *h = cache_get(pat, patlen, flags);
    g_cache_last = h;
    if (h == NULL) return VRE_INTERNAL;
    return count_in(h, subj, len);
}
