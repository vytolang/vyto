/* vyto/util/date native backing — calendar math via <time.h>.

   The Vyto side and C agree on a flat POD `VDate` (nine ints, exact layout);
   struct tm never crosses the FFI boundary. The Vyto `DateTime` is filled by
   out-parameter (&local -> VDate*), matching the sqlite3_open(&db) idiom. */

#define _GNU_SOURCE      /* timegm, strptime on glibc */
#include <time.h>
#include <string.h>

typedef struct {
    int year, month, day, hour, min, sec, wday, yday, isdst;
} VDate;

static void tm_to_vdate(const struct tm *tm, VDate *o) {
    o->year = tm->tm_year + 1900;
    o->month = tm->tm_mon + 1;
    o->day = tm->tm_mday;
    o->hour = tm->tm_hour;
    o->min = tm->tm_min;
    o->sec = tm->tm_sec;
    o->wday = tm->tm_wday;
    o->yday = tm->tm_yday;
    o->isdst = tm->tm_isdst;
}

static void vdate_to_tm(const VDate *d, struct tm *tm) {
    memset(tm, 0, sizeof *tm);
    tm->tm_year = d->year - 1900;
    tm->tm_mon = d->month - 1;
    tm->tm_mday = d->day;
    tm->tm_hour = d->hour;
    tm->tm_min = d->min;
    tm->tm_sec = d->sec;
    tm->tm_isdst = -1;
}

void vdate_from_unix(long long secs, int utc, VDate *out) {
    time_t t = (time_t)secs;
    struct tm tm;
    if (utc) gmtime_r(&t, &tm); else localtime_r(&t, &tm);
    tm_to_vdate(&tm, out);
}

long long vdate_to_unix(const VDate *d, int utc) {
    struct tm tm;
    vdate_to_tm(d, &tm);
    return (long long)(utc ? timegm(&tm) : mktime(&tm));
}

int vdate_format(const VDate *d, const char *fmt, char *out, int cap) {
    /* Normalise through timegm+gmtime so wday/yday are always correct even if
       the caller built the DateTime by hand and left them at 0. */
    struct tm tm;
    vdate_to_tm(d, &tm);
    time_t t = timegm(&tm);
    struct tm norm;
    gmtime_r(&t, &norm);
    return (int)strftime(out, (size_t)cap, fmt, &norm);
}

int vdate_parse(const char *s, const char *fmt, VDate *out) {
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    if (!strptime(s, fmt, &tm)) return 0;
    tm_to_vdate(&tm, out);
    return 1;
}
