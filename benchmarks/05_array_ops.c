#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

int main() {
    int64_t size = 10000000;
    int64_t *arr = (int64_t *)malloc(size * sizeof(int64_t));

    for (int64_t i = 0; i < size; i++) {
        arr[i] = (i * 7) % 1000;
    }

    int64_t sum = 0;
    for (int64_t i = 0; i < size; i++) {
        sum += arr[i];
    }

    int64_t min = arr[0], max = arr[0];
    for (int64_t i = 1; i < size; i++) {
        int64_t v = arr[i];
        if (v < min) min = v;
        if (v > max) max = v;
    }

    printf("array size %ld\n", (long)size);
    printf("sum = %ld\n", (long)sum);
    printf("min = %ld\n", (long)min);
    printf("max = %ld\n", (long)max);
    free(arr);
    return 0;
}
