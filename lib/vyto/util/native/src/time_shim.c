/* vyto/util/time native backing — monotonic + wall clocks and sleep.

   All values are plain i64 nanoseconds; no allocation, no shared state, so the
   Vyto side stays low-latency (a clock read is one syscall/vDSO call). */

#ifdef _WIN32
/* mingw declares clock_gettime/nanosleep, but they live in libwinpthread, which
   the -win32 threads toolchain does not link. Use kernel32 directly so this
   needs no #link entry and no particular gcc threads model. */
#include <windows.h>

long long vtime_mono_ns(void) {
    static LARGE_INTEGER freq; /* fixed for the life of the process */
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    /* Split into whole seconds + remainder before scaling: t.QuadPart * 1e9
       overflows i64 after ~9 seconds of uptime on a 1GHz-tick counter. */
    long long q = t.QuadPart / freq.QuadPart, r = t.QuadPart % freq.QuadPart;
    return q * 1000000000LL + r * 1000000000LL / freq.QuadPart;
}

long long vtime_real_ns(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft); /* 100ns units since 1601-01-01 UTC */
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (long long)(u.QuadPart - 116444736000000000ULL) * 100LL;
}

void vtime_sleep_ns(long long ns) {
    if (ns <= 0) return;
    /* Sleep()'s resolution is milliseconds; round up so a sub-ms request still
       yields rather than returning immediately. */
    Sleep((DWORD)((ns + 999999LL) / 1000000LL));
}
#else
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
#endif
