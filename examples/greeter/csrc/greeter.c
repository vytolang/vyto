#include "greeter.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static GreetStats stats;

const char *greeter_hello(const char *name, int style) {
    static char buf[GREETER_MAX_LEN];
    const char *fmt = style == GREET_FORMAL ? "Good day, %s."
                    : style == GREET_LOUD   ? "HEY %s!!!"
                                            : "hi %s";
    snprintf(buf, sizeof buf, fmt, name);
    stats.calls++;
    stats.avg_len += ((double)strlen(buf) - stats.avg_len) / stats.calls;
    return buf;
}

long greeter_len(const char *s) { return (long)strlen(s); }

double greeter_scale(double x, float factor) { return x * (double)factor; }

GreetStats greeter_stats(void) { return stats; }

void greeter_reset(void) { stats.calls = 0; stats.avg_len = 0; }

int greeter_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

void greeter_each(void (*cb)(const char *item)) { cb("one"); cb("two"); }
