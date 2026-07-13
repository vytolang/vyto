/* host_hooks.c — a libc-backed implementation of the six Vyto host hooks.
 *
 * This is the *host stand-in* used to exercise the freestanding runtime on a
 * normal desktop, with no cross toolchain. The Vyto runtime itself is compiled
 * `--freestanding` (-DVT_NO_LIBC) and reaches the platform ONLY through these
 * functions; here we route them to libc and provide an `int main` that calls
 * the exported `vt_main()`. On a real MCU you would instead back these with a
 * bump/pool allocator + UART + breakpoint, and call vt_main() from the reset
 * handler (see docs/PORTABILITY.md and build.sh in this directory). */
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>

void *vt_host_alloc(size_t n) { return calloc(1, n ? n : 1); }
void *vt_host_realloc(void *p, size_t n) { return realloc(p, n); }
void  vt_host_free(void *p) { free(p); }
void  vt_host_write(const char *b, size_t n) { (void)!write(1, b, n); }
void  vt_host_write_err(const char *b, size_t n) { (void)!write(2, b, n); }
void  vt_host_abort(void) { _exit(101); }

void vt_main(void); /* exported by the freestanding build in place of main() */
int main(void) { vt_main(); return 0; }
