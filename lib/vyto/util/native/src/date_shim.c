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

#ifdef _WIN32
/* The MSVC runtime spells the reentrant converters differently (arguments
   reversed, errno_t return) and calls timegm _mkgmtime. Map them here so the
   bodies below stay single-sourced. */
#include <ctype.h>
#include <stdlib.h>
#define gmtime_r(t, tm)    (gmtime_s((tm), (t)) ? NULL : (tm))
#define localtime_r(t, tm) (localtime_s((tm), (t)) ? NULL : (tm))
#define timegm(tm)         _mkgmtime(tm)

/* Windows has no strptime in any form, so vyto/util/date's `parse` would
   otherwise have to disappear on this platform. This covers the conversions
   strftime can round-trip for a DateTime — everything the struct carries —
   in the C locale:
     %Y %y %C  %m %d %e  %H %I %M %S  %j  %p  %a %A %b %B %h
     %D (= %m/%d/%y)  %F (= %Y-%m-%d)  %T (= %H:%M:%S)  %R (= %H:%M)
     %n %t (any whitespace)  %% (literal)
   Anything else returns NULL, which surfaces as date_is_valid() == false
   rather than as a silently wrong date. */
static const char *const wday_ab[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
static const char *const wday_full[7] = {"Sunday","Monday","Tuesday","Wednesday",
                                         "Thursday","Friday","Saturday"};
static const char *const mon_ab[12] = {"Jan","Feb","Mar","Apr","May","Jun",
                                       "Jul","Aug","Sep","Oct","Nov","Dec"};
static const char *const mon_full[12] = {"January","February","March","April",
                                         "May","June","July","August","September",
                                         "October","November","December"};

/* Read 1..width digits into *out; NULL if there isn't at least one. */
static const char *sp_num(const char *s, int width, int *out) {
    int v = 0, n = 0;
    while (*s == ' ') s++; /* %e and friends pad with a space */
    while (n < width && *s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); n++; }
    if (n == 0) return NULL;
    *out = v;
    return s;
}

/* Case-insensitive match of the longest entry in tab[]; index into *out. */
static const char *sp_name(const char *s, const char *const *tab, int n, int *out) {
    int best = -1;
    size_t bestlen = 0;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(tab[i]);
        if (l > bestlen && _strnicmp(s, tab[i], l) == 0) { best = i; bestlen = l; }
    }
    if (best < 0) return NULL;
    *out = best;
    return s + bestlen;
}

static const char *sp_fmt(const char *s, const char *f, struct tm *tm, int *pm_flag);

static const char *sp_conv(const char *s, char c, struct tm *tm, int *pm_flag) {
    int v;
    switch (c) {
    case 'Y': if (!(s = sp_num(s, 4, &v))) return NULL; tm->tm_year = v - 1900; return s;
    case 'y': if (!(s = sp_num(s, 2, &v))) return NULL;
              tm->tm_year = v < 69 ? v + 100 : v; return s; /* POSIX 69/70 pivot */
    case 'C': if (!(s = sp_num(s, 2, &v))) return NULL;
              tm->tm_year = v * 100 - 1900 + (tm->tm_year + 1900) % 100; return s;
    case 'm': if (!(s = sp_num(s, 2, &v))) return NULL; tm->tm_mon = v - 1; return s;
    case 'd': case 'e': if (!(s = sp_num(s, 2, &v))) return NULL; tm->tm_mday = v; return s;
    case 'H': if (!(s = sp_num(s, 2, &v))) return NULL; tm->tm_hour = v; return s;
    case 'I': if (!(s = sp_num(s, 2, &v))) return NULL; tm->tm_hour = v % 12; return s;
    case 'M': if (!(s = sp_num(s, 2, &v))) return NULL; tm->tm_min = v; return s;
    case 'S': if (!(s = sp_num(s, 2, &v))) return NULL; tm->tm_sec = v; return s;
    case 'j': if (!(s = sp_num(s, 3, &v))) return NULL; tm->tm_yday = v - 1; return s;
    case 'p':
        if (_strnicmp(s, "AM", 2) == 0) { *pm_flag = 0; return s + 2; }
        if (_strnicmp(s, "PM", 2) == 0) { *pm_flag = 1; return s + 2; }
        return NULL;
    case 'a': case 'A':
        if ((s = sp_name(s, wday_full, 7, &v)) || (s = sp_name(s, wday_ab, 7, &v))) {
            tm->tm_wday = v;
            return s;
        }
        return NULL;
    case 'b': case 'B': case 'h': {
        const char *t;
        if ((t = sp_name(s, mon_full, 12, &v)) || (t = sp_name(s, mon_ab, 12, &v))) {
            tm->tm_mon = v;
            return t;
        }
        return NULL;
    }
    case 'D': return sp_fmt(s, "%m/%d/%y", tm, pm_flag);
    case 'F': return sp_fmt(s, "%Y-%m-%d", tm, pm_flag);
    case 'T': return sp_fmt(s, "%H:%M:%S", tm, pm_flag);
    case 'R': return sp_fmt(s, "%H:%M", tm, pm_flag);
    case 'n': case 't':
        while (isspace((unsigned char)*s)) s++;
        return s;
    case '%': return *s == '%' ? s + 1 : NULL;
    default: return NULL; /* unsupported conversion: fail loudly, don't guess */
    }
}

static const char *sp_fmt(const char *s, const char *f, struct tm *tm, int *pm_flag) {
    for (; *f; f++) {
        if (*f == '%') {
            f++;
            if (!*f) return NULL;
            if (!(s = sp_conv(s, *f, tm, pm_flag))) return NULL;
        } else if (isspace((unsigned char)*f)) {
            while (isspace((unsigned char)*s)) s++;
        } else {
            if (*s != *f) return NULL;
            s++;
        }
    }
    return s;
}

static char *strptime(const char *s, const char *fmt, struct tm *tm) {
    int pm_flag = -1;
    const char *end = sp_fmt(s, fmt, tm, &pm_flag);
    if (!end) return NULL;
    /* %p only means something once %I has set the 12-hour value */
    if (pm_flag == 1 && tm->tm_hour < 12) tm->tm_hour += 12;
    else if (pm_flag == 0 && tm->tm_hour == 12) tm->tm_hour = 0;
    return (char *)end;
}
#endif

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
