/* cbwrap — a callback-driven C API for demonstrating Vyto's cthunk. */
#ifndef CBWRAP_H
#define CBWRAP_H

/* userdata-first callback (GLib/libuv style) */
void cb_each(int from, int to, void (*fn)(void *ud, int value), void *ud);

/* userdata-last callback with a return value (qsort_r style) */
long cb_fold(long init, int n, long (*fn)(long acc, int value, void *ud), void *ud);

#endif
