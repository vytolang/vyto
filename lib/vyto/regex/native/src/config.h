/* config.h — PCRE2 build configuration for vyto/regex, written by hand.
 *
 * PCRE2 normally gets this from autoconf. We cannot run autoconf: vytoc has no
 * #cflags pragma (src/parse.c:860-878 — #link is the only one) and the native
 * compile line is fixed (src/main.c:616-635), so every -D PCRE2 needs must come
 * from a C file. The wrapper TUs in this directory supply PCRE2_CODE_UNIT_WIDTH
 * and HAVE_CONFIG_H; everything else is here.
 *
 * Every value below is deliberately identical on linux-x64, linux-arm64,
 * macos-*, and windows-x64. A host-derived value would let the same pattern
 * behave differently per platform and break the golden tests.
 *
 * This file lives outside native/src/pcre2/ so that tree stays byte-identical
 * to the upstream release (checked by native/refresh-pcre2.sh --verify).
 * pcre2_internal.h does #include "config.h", which misses pcre2/src/ and falls
 * through to -Inative/src, landing here.
 */

#ifndef VYTO_REGEX_CONFIG_H
#define VYTO_REGEX_CONFIG_H

/* ---- identity (kept in sync by native/refresh-pcre2.sh) ---- */
#define PACKAGE         "pcre2"
#define PACKAGE_NAME    "PCRE2"
#define PACKAGE_TARNAME "pcre2"
#define PACKAGE_VERSION "10.45"
#define PACKAGE_STRING  "PCRE2 10.45"
#define VERSION         "10.45"

/* Static linkage. Without this pcre2.h decorates every prototype with
   __declspec(dllimport) under _WIN32 and the mingw link fails on
   __imp_pcre2_compile_8. */
#define PCRE2_STATIC 1

/* Symbol visibility attribute, normally filled in by m4/pcre2_visibility.m4.
   PCRE2 is linked straight into the executable and nothing outside it calls in,
   so empty is correct — and it keeps -Wl,--gc-sections (src/main.c:829) free to
   drop everything vyto/regex does not reference. */
#define PCRE2_EXPORT

/* ---- feature set ---- */
#define SUPPORT_PCRE2_8 1       /* 8-bit code units only; no _16 / _32 */
#define SUPPORT_UNICODE 1       /* \p{...}, UCP, UTF-8 mode */
#define SUPPORT_JIT     1

/* sljit emits and runs machine code. Under TCC (which vytoc picks for every
   non-release build when it is installed — src/main.c:485) that is unvalidated,
   and a freestanding build has no libc to run it on. Fall back to the
   interpreter: pcre2_match() does this transparently. */
#if defined(__TINYC__) || defined(VT_NO_LIBC)
#undef SUPPORT_JIT
#endif

/* ---- tunables, at upstream defaults ----
   These are compile-time ceilings. regex_shim.c sets much lower *runtime*
   limits on every match context; see vre_set_limits. */
#define LINK_SIZE          2
#define HEAP_LIMIT         20000000
#define MATCH_LIMIT        10000000
#define MATCH_LIMIT_DEPTH  MATCH_LIMIT
#define PARENS_NEST_LIMIT  250
#define MAX_VARLOOKBEHIND  255
#define MAX_NAME_SIZE      32
#define MAX_NAME_COUNT     10000

/* LF, pinned. Upstream's configure derives this from the build host; that would
   make "$" and "." mean different things on Windows and Linux. */
#define NEWLINE_DEFAULT 2

/* BSR_ANYCRLF deliberately not defined -> \R matches any Unicode newline. */

/* ---- libc facts ----
   Only things C99 guarantees on every target we build for, so no probing is
   needed. Everything else autoconf tests for (mkostemp, secure_getenv, realpath,
   HAVE_BUILTIN_MUL_OVERFLOW, readline, zlib/bzip2) is used only by pcre2grep and
   pcre2test, which are not vendored, or is an optimization hint whose absence
   degrades silently. */
#define HAVE_MEMMOVE   1
#define HAVE_STRERROR  1
#define HAVE_LIMITS_H  1
#define HAVE_STDINT_H  1
#define HAVE_INTTYPES_H 1
#define HAVE_STDLIB_H  1
#define HAVE_STRING_H  1

#ifndef _WIN32
#define HAVE_UNISTD_H     1
#define HAVE_SYS_TYPES_H  1
#define HAVE_SYS_STAT_H   1
#endif

#endif /* VYTO_REGEX_CONFIG_H */
