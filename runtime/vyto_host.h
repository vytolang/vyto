/* Vyto host-hook contract — the seam between the runtime and the platform.
 *
 * Every libc touch-point in vyto_rt.c (allocation, byte output, abort) routes
 * through these six functions instead of calling malloc/fwrite/exit directly.
 *
 *   Hosted builds (the default): vyto_rt.c defines the hooks as thin static
 *   wrappers over libc (see its top). This header stays libc-free so it never
 *   leaks stdio/stdlib types (e.g. div_t) into module headers, and existing
 *   linux/macos/windows targets are byte-for-byte unchanged.
 *
 *   Freestanding builds (-DVT_NO_LIBC): the hooks are `extern` — the embedder
 *   MUST supply strong definitions (a link error otherwise), backed by whatever
 *   the platform offers: a bump/pool allocator, a UART, a semihosting channel,
 *   a breakpoint. No libc is referenced anywhere.
 *
 * See docs/PORTABILITY.md for the embedder walkthrough. */
#ifndef VYTO_HOST_H
#define VYTO_HOST_H

#include <stddef.h>

#ifndef VT_NORETURN
#if defined(__GNUC__) || defined(__clang__)
#define VT_NORETURN __attribute__((noreturn))
#else
#define VT_NORETURN
#endif
#endif

#ifdef VT_NO_LIBC
/* Provided by the embedder. */
void *vt_host_alloc(size_t n);              /* zeroed, like calloc(1, n) */
void *vt_host_realloc(void *p, size_t n);
void  vt_host_free(void *p);
void  vt_host_write(const char *buf, size_t len);      /* stdout channel */
void  vt_host_write_err(const char *buf, size_t len);  /* stderr channel; may alias write */
VT_NORETURN void vt_host_abort(void);       /* replaces exit(101); must not return */
#endif

#endif /* VYTO_HOST_H */
