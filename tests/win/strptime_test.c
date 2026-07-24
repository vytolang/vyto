/* Exercises date_shim.c's _WIN32 branch — chiefly the hand-written strptime,
   which Windows has no libc equivalent of — by compiling that branch on THIS
   machine with the MSVC-spelled names it expects supplied as shims.
 *
 * This is the only part of the Windows port whose behaviour can be checked
 * without a Windows box, so it is checked here rather than deferred to the
 * staged run. Built and run by tests/run_tests_win.sh. */
#define _GNU_SOURCE
#include <time.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

typedef int errno_t;
static int _strnicmp(const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }
static errno_t gmtime_s(struct tm *tm, const time_t *t) { return gmtime_r(t, tm) ? 0 : 1; }
static errno_t localtime_s(struct tm *tm, const time_t *t) { return localtime_r(t, tm) ? 0 : 1; }
static time_t _mkgmtime(struct tm *tm) { return timegm(tm); }

#define _WIN32 1
/* glibc's <time.h> already declares strptime non-static; rename the shim's own
   definition so the harness compiles. Everything else is unchanged. */
#define strptime vyto_strptime
#include "date_shim.c"

static int fails = 0;

static void ok(const char *what, int cond) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) fails++;
}

/* parse then round-trip through the shim's own to_unix */
static long long parse_unix(const char *s, const char *fmt, int *good) {
    VDate v;
    memset(&v, 0, sizeof v);
    *good = vdate_parse(s, fmt, &v);
    if (!*good) return -1;
    return vdate_to_unix(&v, 1);
}

int main(void) {
    int g;
    /* the exact call examples/44_date.vt makes */
    ok("ISO datetime", parse_unix("2021-07-04 12:30:00", "%Y-%m-%d %H:%M:%S", &g) == 1625401800LL && g);
    ok("garbage rejected", (parse_unix("not a date", "%Y-%m-%d", &g), !g));

    ok("epoch", parse_unix("1970-01-01 00:00:00", "%Y-%m-%d %H:%M:%S", &g) == 0 && g);
    ok("%F %T", parse_unix("2023-11-14 22:13:20", "%F %T", &g) == 1700000000LL && g);
    ok("%D", parse_unix("11/14/23", "%D", &g) == 1699920000LL && g);
    ok("month name %B", parse_unix("14 November 2023", "%d %B %Y", &g) == 1699920000LL && g);
    ok("month abbrev %b", parse_unix("14 Nov 2023", "%d %b %Y", &g) == 1699920000LL && g);
    ok("weekday name skipped", parse_unix("Tuesday 2023-11-14", "%A %Y-%m-%d", &g) == 1699920000LL && g);
    ok("%I%p pm", parse_unix("2023-11-14 10:13:20 PM", "%Y-%m-%d %I:%M:%S %p", &g) == 1700000000LL && g);
    ok("%I%p am midnight", parse_unix("2023-11-14 12:00:00 AM", "%Y-%m-%d %I:%M:%S %p", &g) == 1699920000LL && g);
    /* POSIX %y: 69..99 -> 1969..1999, 00..68 -> 2000..2068 */
    ok("%y pivot 69->1969", parse_unix("69-01-01", "%y-%m-%d", &g) == -31536000LL && g);
    ok("%y pivot 70->1970", parse_unix("70-01-01", "%y-%m-%d", &g) == 0 && g);
    ok("%y pivot 00->2000", parse_unix("00-01-01", "%y-%m-%d", &g) == 946684800LL && g);
    ok("%e space pad", parse_unix("2023-11- 4", "%Y-%m-%e", &g) == 1699056000LL && g);
    ok("literal %% ", parse_unix("2023%11%14", "%Y%%%m%%%d", &g) == 1699920000LL && g);
    ok("whitespace flexible", parse_unix("2023-11-14   22:13:20", "%Y-%m-%d %T", &g) == 1700000000LL && g);

    /* failure modes must fail, not silently produce a date */
    (void)parse_unix("2023-13", "%Y-%m-%d", &g); ok("truncated input rejected", !g);
    (void)parse_unix("2023-11-14", "%Y/%m/%d", &g); ok("wrong separator rejected", !g);
    (void)parse_unix("14 Xxx 2023", "%d %b %Y", &g); ok("bad month name rejected", !g);
    (void)parse_unix("2023-11-14", "%Y-%m-%d%Z", &g); ok("unsupported conversion rejected", !g);
    (void)parse_unix("abcd-11-14", "%Y-%m-%d", &g); ok("non-numeric year rejected", !g);

    /* format still works through the remapped gmtime_r/timegm */
    VDate d;
    vdate_from_unix(1700000000LL, 1, &d);
    char buf[64];
    vdate_format(&d, "%Y-%m-%d %H:%M:%S", buf, sizeof buf);
    ok("format round-trip", strcmp(buf, "2023-11-14 22:13:20") == 0);
    ok("wday derived", d.wday == 2); /* 2023-11-14 was a Tuesday */

    printf("%s\n", fails ? "FAILURES" : "all ok");
    return fails != 0;
}
