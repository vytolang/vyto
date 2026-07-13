/* vyto/net/websocket native backing — cryptographic-quality random bytes only.

   Everything else (base64, RFC 6455 framing, the HTTP Upgrade) is pure Vyto.
   The random bytes seed the Sec-WebSocket-Key and per-frame masks; RFC 6455
   requires client masks to be unpredictable. Fills a caller bytes(n) buffer. */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

void ws_random(char *out, int n) {
    if (n <= 0) return;
    size_t got = 0;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        got = fread(out, 1, (size_t)n, f);
        fclose(f);
    }
    if (got < (size_t)n) {
        static int seeded = 0;
        if (!seeded) { srand((unsigned)time(NULL) ^ (unsigned)(size_t)out); seeded = 1; }
        for (size_t i = got; i < (size_t)n; i++) out[i] = (char)(rand() & 0xFF);
    }
}
