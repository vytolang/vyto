#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

int main() {
    int64_t limit = 10000000;
    bool *sieve = (bool *)malloc(limit * sizeof(bool));
    for (int64_t i = 0; i < limit; i++) sieve[i] = true;
    sieve[0] = false;
    sieve[1] = false;

    int64_t p = 2;
    while (p * p < limit) {
        if (sieve[p]) {
            int64_t i = p * p;
            while (i < limit) {
                sieve[i] = false;
                i += p;
            }
        }
        p += 1;
    }

    int64_t count = 0;
    for (int64_t i = 0; i < limit; i++) {
        if (sieve[i]) count += 1;
    }
    printf("primes up to %ld: %ld\n", (long)limit, (long)count);
    free(sieve);
    return 0;
}
