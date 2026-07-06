/* greeter — demo library shipped as a prebuilt .so.
 * The Volt binding (greeter.vt) is generated from this header by voltbind. */
#ifndef GREETER_H
#define GREETER_H

#define GREETER_MAX_LEN 64
#define GREETER_MAGIC 0x2A

typedef enum { GREET_CASUAL, GREET_FORMAL = 5, GREET_LOUD } GreetStyle;

typedef struct GreetStats {
    int calls;
    double avg_len;
} GreetStats;

const char *greeter_hello(const char *name, int style);
long greeter_len(const char *s);
double greeter_scale(double x, float factor);
GreetStats greeter_stats(void);
void greeter_reset(void);

/* not representable in Volt v0.1 — voltbind must skip these */
int greeter_printf(const char *fmt, ...);
void greeter_each(void (*cb)(const char *item));

#endif
