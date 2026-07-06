#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

int main() {
    int64_t size = 300;
    double *a = (double *)malloc(size * size * sizeof(double));
    double *b = (double *)malloc(size * size * sizeof(double));
    double *c = (double *)malloc(size * size * sizeof(double));

    for (int64_t i = 0; i < size; i++) {
        for (int64_t j = 0; j < size; j++) {
            a[i * size + j] = (double)(i + j);
            b[i * size + j] = (double)(i - j);
            c[i * size + j] = 0.0;
        }
    }

    for (int64_t i = 0; i < size; i++) {
        for (int64_t j = 0; j < size; j++) {
            double sum = 0.0;
            for (int64_t k = 0; k < size; k++) {
                sum += a[i * size + k] * b[k * size + j];
            }
            c[i * size + j] = sum;
        }
    }

    printf("matrix %ldx%ld multiplied\n", (long)size, (long)size);
    printf("c[0][0] = %g\n", c[0]);
    free(a); free(b); free(c);
    return 0;
}
