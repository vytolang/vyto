#include "cbwrap.h"

void cb_each(int from, int to, void (*fn)(void *, int), void *ud) {
    for (int i = from; i < to; i++) fn(ud, i);
}

long cb_fold(long init, int n, long (*fn)(long, int, void *), void *ud) {
    long acc = init;
    for (int i = 0; i < n; i++) acc = fn(acc, i, ud);
    return acc;
}
