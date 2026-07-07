// Münchausen number finder — C counterpart.
// Same algorithm as 10_munchausen.vt for fair comparison.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#define LIMIT 440000000

static int64_t digit_pow(int d) {
    switch (d) {
        case 0: return 0;
        case 1: return 1;
        case 2: return 4;
        case 3: return 27;
        case 4: return 256;
        case 5: return 3125;
        case 6: return 46656;
        case 7: return 823543;
        case 8: return 16777216;
        default: return 387420489;
    }
}

static int64_t munchausen_sum(int64_t n) {
    int64_t s = 0;
    while (n > 0) {
        s += digit_pow(n % 10);
        n /= 10;
    }
    return s;
}

int main(void) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int64_t found[16];
    int count = 0;
    for (int64_t n = 0; n < LIMIT; n++) {
        if (munchausen_sum(n) == n) {
            found[count++] = n;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1000000.0;

    printf("Münchausen numbers found: %d\n", count);
    for (int i = 0; i < count; i++) {
        printf("  %ld\n", (long)found[i]);
    }
    printf("time_ms: %.1f\n", ms);
    return 0;
}
