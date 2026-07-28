/* vyto/util/uuid native backing — cryptographic-quality random bytes only.

   UUID formatting, the v7 timestamp layout and the parser are pure Vyto; the
   one thing a language with no OS entropy API cannot do for itself is produce
   unpredictable bytes, so that is all this file does. Fills a caller bytes(n)
   buffer.

   Source order: getrandom(2) (Linux; never fails once the entropy pool is
   initialized), BCryptGenRandom (Windows), then /dev/urandom, then — only if
   all of those are unavailable, e.g. a stripped-down container — a time-seeded
   PRNG as the last resort.

   Same ladder as lib/vyto/net/websocket/native/src/ws_shim.c, deliberately
   duplicated rather than shared: a native/src directory is compiled for every
   module in its package dir, so hoisting this into lib/vyto/util/native/src
   would compile it — and force the Windows bcrypt link — into every program
   that imports vyto/util/fmt. Keeping it here confines the cost to callers who
   actually asked for randomness. */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#if defined(__linux__)
#include <sys/random.h>
#elif defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

/* A v4 UUID is only as good as its entropy, and callers use these bytes for
   session identifiers and idempotency keys, so this must reach a real CSPRNG
   on every platform. The rand() tail is a last resort that should never be
   hit: seeded from the clock, it would make identifiers guessable. */
void vt_rand_bytes(char *out, int n) {
    if (n <= 0) return;
    size_t got = 0;
#if defined(__linux__)
    while (got < (size_t)n) {
        ssize_t r = getrandom(out + got, (size_t)n - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
    }
#elif defined(_WIN32)
    /* Windows has neither getrandom nor /dev/urandom, so without this the
       fallback chain below lands straight on rand(). */
    if (BCRYPT_SUCCESS(BCryptGenRandom(NULL, (PUCHAR)out, (ULONG)n,
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        got = (size_t)n;
#endif
    if (got < (size_t)n) {
        FILE *f = fopen("/dev/urandom", "rb");
        if (f) {
            got += fread(out + got, 1, (size_t)n - got, f);
            fclose(f);
        }
    }
    if (got < (size_t)n) {
        static int seeded = 0;
        if (!seeded) { srand((unsigned)time(NULL) ^ (unsigned)(size_t)out); seeded = 1; }
        for (size_t i = got; i < (size_t)n; i++) out[i] = (char)(rand() & 0xFF);
    }
}
