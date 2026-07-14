/* vyto/net/websocket native backing — cryptographic-quality random bytes only.

   Everything else (base64, SHA-1, RFC 6455 framing, the HTTP Upgrade) is pure
   Vyto. The random bytes seed the Sec-WebSocket-Key and per-frame masks;
   RFC 6455 requires client masks to be unpredictable. Fills a caller
   bytes(n) buffer.

   Source order: getrandom(2) (Linux; never fails once the entropy pool is
   initialized), then /dev/urandom, then — only if both are unavailable, e.g.
   a stripped-down container — a time-seeded PRNG as the last resort. */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#if defined(__linux__)
#include <sys/random.h>
#endif

void ws_random(char *out, int n) {
    if (n <= 0) return;
    size_t got = 0;
#if defined(__linux__)
    while (got < (size_t)n) {
        ssize_t r = getrandom(out + got, (size_t)n - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
    }
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
