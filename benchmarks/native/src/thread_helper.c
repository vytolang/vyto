#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

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

// Parallel sum: splits array into num_threads chunks
int64_t volt_parallel_sum(int64_t *data, int64_t size, int64_t num_threads) {
    pthread_t *threads = malloc(num_threads * sizeof(pthread_t));
    ThreadArg *args = malloc(num_threads * sizeof(ThreadArg));
    int64_t chunk = size / num_threads;

    for (int64_t t = 0; t < num_threads; t++) {
        args[t].data = data;
        args[t].start = t * chunk;
        args[t].count = (t == num_threads - 1) ? (size - t * chunk) : chunk;
        args[t].result = 0;
        pthread_create(&threads[t], NULL, thread_sum, &args[t]);
    }

    int64_t total = 0;
    for (int64_t t = 0; t < num_threads; t++) {
        pthread_join(threads[t], NULL);
        total += args[t].result;
    }

    free(threads);
    free(args);
    return total;
}

// Single-threaded sum for comparison
int64_t volt_single_sum(int64_t *data, int64_t size) {
    int64_t sum = 0;
    for (int64_t i = 0; i < size; i++) {
        sum += data[i];
    }
    return sum;
}

// Fill array: data[i] = (i * 7) % 1000
void volt_fill_array(int64_t *data, int64_t size) {
    for (int64_t i = 0; i < size; i++) {
        data[i] = (i * 7) % 1000;
    }
}
