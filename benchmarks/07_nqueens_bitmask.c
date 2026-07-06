// Benchmark: 15-Queens with the optimal bitmask algorithm (C counterpart).
#include <stdint.h>
#include <stdio.h>

static int64_t solve(int64_t mask, int64_t cols, int64_t d1, int64_t d2) {
    if (cols == mask) return 1;
    int64_t count = 0;
    int64_t avail = (~(cols | d1 | d2)) & mask;
    while (avail) {
        int64_t bit = avail & (-avail);
        avail ^= bit;
        count += solve(mask, cols | bit, (d1 | bit) << 1, (d2 | bit) >> 1);
    }
    return count;
}

int main(void) {
    int n = 15;
    int64_t result = solve((1LL << n) - 1, 0, 0, 0);
    printf("n=%d queens: %lld solutions\n", n, (long long)result);
    return 0;
}
