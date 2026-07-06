#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

typedef struct {
    int64_t *data;
    int64_t start;
    int64_t count;
    int64_t result;
} ThreadArg;

static void* thread_sum(void *arg) {
    ThreadArg *ta = (ThreadArg*)arg;
    int64_t sum = 0;
    for (int64_t i = ta->start; i < ta->start + ta->count; i++) {
        sum += ta->data[i];
    }
    ta->result = sum;
    return NULL;
}

int64_t parallel_sum(int64_t *data, int64_t size, int num_threads) {
    pthread_t *threads = malloc(num_threads * sizeof(pthread_t));
    ThreadArg *args = malloc(num_threads * sizeof(ThreadArg));
    int64_t chunk = size / num_threads;

    for (int t = 0; t < num_threads; t++) {
        args[t].data = data;
        args[t].start = t * chunk;
        args[t].count = (t == num_threads - 1) ? (size - t * chunk) : chunk;
        args[t].result = 0;
        pthread_create(&threads[t], NULL, thread_sum, &args[t]);
    }

    int64_t total = 0;
    for (int t = 0; t < num_threads; t++) {
        pthread_join(threads[t], NULL);
        total += args[t].result;
    }

    free(threads);
    free(args);
    return total;
}

int64_t single_sum(int64_t *data, int64_t size) {
    int64_t sum = 0;
    for (int64_t i = 0; i < size; i++) {
        sum += data[i];
    }
    return sum;
}

void fill_array(int64_t *data, int64_t size) {
    for (int64_t i = 0; i < size; i++) {
        data[i] = (i * 7) % 1000;
    }
}

int main() {
    int num_threads = 4;
    int64_t size = 100000000;

    int64_t *data = malloc(size * sizeof(int64_t));
    fill_array(data, size);

    // Single-threaded
    int64_t sum1 = single_sum(data, size);
    printf("single-threaded sum = %ld\n", (long)sum1);

    // Multi-threaded
    int64_t sum2 = parallel_sum(data, size, num_threads);
    printf("multi-threaded sum (%d threads) = %ld\n", num_threads, (long)sum2);

    free(data);
    return 0;
}
