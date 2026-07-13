/* vyto/util/time native backing — monotonic + wall clocks and sleep.

   All values are plain i64 nanoseconds; no allocation, no shared state, so the
   Vyto side stays low-latency (a clock read is one syscall/vDSO call). */

#include <time.h>

long long vtime_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

long long vtime_real_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

void vtime_sleep_ns(long long ns) {
    if (ns <= 0) return;
    struct timespec req;
    req.tv_sec = (time_t)(ns / 1000000000LL);
    req.tv_nsec = (long)(ns % 1000000000LL);
    /* restart on EINTR so the full interval elapses */
    while (nanosleep(&req, &req) != 0) {
        if (req.tv_sec == 0 && req.tv_nsec == 0) break;
    }
}
