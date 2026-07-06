#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

int64_t fib(int64_t n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

int main() {
    int64_t n = 40;
    int64_t result = fib(n);
    printf("fib(%ld) = %ld\n", (long)n, (long)result);
    return 0;
}
