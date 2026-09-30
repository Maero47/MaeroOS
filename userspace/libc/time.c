/* Calendar time.  There is no zoneinfo database: local time is UTC shifted by
 * the offset of a POSIX TZ string such as "UTC0", "EST5" or "UTC-01:30"
 * (daylight saving rules are ignored). */
#include "../include/ctype.h"
#include "../include/stdio.h"
#include "../include/stdlib.h"
#include "../include/string.h"
#include "../include/strings.h"
#include "../include/time.h"

static const char *const wday_name[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};
static const char *const mon_name[] = {
    "January", "February", "March", "April", "May", "June", "July", "August",
    "September", "October", "November", "December",
};

char *tzname[2] = { "UTC", "UTC" };
long timezone;
int daylight;
static char tz_std[16] = "UTC";

/* TZ = std offset: "hh[:mm[:ss]]" hours WEST of UTC (so UTC-2 is ahead). */
void tzset(void) {
    const char *tz = getenv("TZ"), *p;
    long off = 0;
    int n = 0;

    timezone = 0;
    daylight = 0;
    strcpy(tz_std, "UTC");
    if (!tz || !*tz) goto out;
    if (*tz == ':') tz++;
    p = tz;
    if (*p == '<') {
        while (*p && *p != '>') p++;
        if (*p) p++;
        n = (int)(p - tz);
    } else {
        while (isalpha((unsigned char)*p)) p++;
        n = (int)(p - tz);
    }
    if (n >= 3 && n < (int)sizeof(tz_std)) {
        memcpy(tz_std, tz, n);
        tz_std[n] = 0;
    }
    if (*p == '+' || *p == '-' || isdigit((unsigned char)*p)) {
        int neg = *p == '-';
        long h = 0, m = 0, s = 0;
        if (*p == '+' || *p == '-') p++;
        while (isdigit((unsigned char)*p)) h = h * 10 + (*p++ - '0');
        if (*p == ':') { p++; while (isdigit((unsigned char)*p)) m = m * 10 + (*p++ - '0'); }
        if (*p == ':') { p++; while (isdigit((unsigned char)*p)) s = s * 10 + (*p++ - '0'); }
        off = h * 3600 + m * 60 + s;
        if (neg) off = -off;
    }
    timezone = off;
out:
    tzname[0] = tzname[1] = tz_std;
}

/* Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant). */
static long long days_from_civil(long long y, int m, int d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

static int is_leap(long long y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static struct tm *to_tm(long long t, struct tm *tm, long gmtoff) {
    long long days = t / 86400, rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    tm->tm_hour = (int)(rem / 3600);
    tm->tm_min = (int)(rem / 60 % 60);
    tm->tm_sec = (int)(rem % 60);
    tm->tm_wday = (int)(((days % 7) + 11) % 7);        /* 1970-01-01 was a Thursday */

    long long z = days + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = (long long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    tm->tm_year = (int)(y - 1900);
    tm->tm_mon = (int)m - 1;
    tm->tm_mday = (int)d;
    tm->tm_yday = (int)(days - days_from_civil(y, 1, 1));
    tm->tm_isdst = 0;
    tm->tm_gmtoff = gmtoff;
    tm->tm_zone = tz_std;
    return tm;
}

struct tm *gmtime_r(const time_t *timep, struct tm *result) {
    to_tm(*timep, result, 0);
    result->tm_zone = "UTC";
    return result;
}
struct tm *gmtime(const time_t *timep) {
    static struct tm tm;
    return gmtime_r(timep, &tm);
}
struct tm *localtime_r(const time_t *timep, struct tm *result) {
    tzset();
    return to_tm((long long)*timep - timezone, result, -timezone);
}
struct tm *localtime(const time_t *timep) {
    static struct tm tm;
    return localtime_r(timep, &tm);
}

/* Normalises the fields of *tm (as mktime must) and returns UTC seconds. */
static long long tm_to_secs(struct tm *tm) {
    long long mon = tm->tm_mon, year = (long long)tm->tm_year + 1900;
    year += mon / 12;
    mon %= 12;
    if (mon < 0) { mon += 12; year--; }
    long long days = days_from_civil(year, (int)mon + 1, 1) + tm->tm_mday - 1;
    return days * 86400 + (long long)tm->tm_hour * 3600 + (long long)tm->tm_min * 60
           + tm->tm_sec;
}

time_t timegm(struct tm *tm) {
    long long t = tm_to_secs(tm);
    gmtime_r(&(time_t){ (time_t)t }, tm);
    return (time_t)t;
}
time_t mktime(struct tm *tm) {
    tzset();
    long long t = tm_to_secs(tm) + timezone;
    time_t tt = (time_t)t;
    localtime_r(&tt, tm);
    return tt;
}

double difftime(time_t a, time_t b) { return (double)a - (double)b; }

char *asctime_r(const struct tm *tm, char *buf) {
    snprintf(buf, 26, "%.3s %.3s%3d %.2d:%.2d:%.2d %d\n",
             wday_name[(unsigned)tm->tm_wday % 7], mon_name[(unsigned)tm->tm_mon % 12],
             tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, 1900 + tm->tm_year);
    return buf;
}
char *asctime(const struct tm *tm) {
    static char buf[32];
    return asctime_r(tm, buf);
}
char *ctime_r(const time_t *timep, char *buf) {
    struct tm tm;
    return asctime_r(localtime_r(timep, &tm), buf);
}
char *ctime(const time_t *timep) {
    static char buf[32];
    return ctime_r(timep, buf);
}

/* ISO 8601 week-based year and week number. */
static int iso_week(const struct tm *tm, int *year) {
    int y = tm->tm_year + 1900, wday = (tm->tm_wday + 6) % 7;   /* Monday = 0 */
    int week = (tm->tm_yday - wday + 10) / 7;
    if (week < 1) {
        y--;
        int pyday = tm->tm_yday + 365 + is_leap(y);
        week = (pyday - wday + 10) / 7;
    } else if (week == 53) {
        int days = 365 + is_leap(y);
        if (tm->tm_yday - wday + 3 >= days) { week = 1; y++; }
    }
    *year = y;
    return week;
}

size_t strftime(char *s, size_t max, const char *format, const struct tm *tm) {
    size_t n = 0;
    char tmp[64];

    if (!max) return 0;
    for (const char *f = format; *f; f++) {
        const char *out = tmp;
        int pad = 0, width = -1;

        if (*f != '%') {
            if (n + 1 >= max) return 0;
            s[n++] = *f;
            continue;
        }
        f++;
        if (*f == '_' || *f == '-' || *f == '0' || *f == '^' || *f == '#') pad = *f++;
        if (isdigit((unsigned char)*f)) {
            width = 0;
            while (isdigit((unsigned char)*f)) width = width * 10 + (*f++ - '0');
        }
        if (*f == 'E' || *f == 'O') f++;
#define NUM(fmt, v) snprintf(tmp, sizeof(tmp), pad == '-' ? "%d" : pad == '_' ? fmt##_sp : fmt, (int)(v))
#define f2 "%02d"
#define f2_sp "%2d"
#define f3 "%03d"
#define f3_sp "%3d"
#define f1 "%d"
#define f1_sp "%d"
        int hour12 = tm->tm_hour % 12 ? tm->tm_hour % 12 : 12, isoy;
        switch (*f) {
        case 'a': snprintf(tmp, sizeof(tmp), "%.3s", wday_name[(unsigned)tm->tm_wday % 7]); break;
        case 'A': out = wday_name[(unsigned)tm->tm_wday % 7]; break;
        case 'b': case 'h': snprintf(tmp, sizeof(tmp), "%.3s", mon_name[(unsigned)tm->tm_mon % 12]); break;
        case 'B': out = mon_name[(unsigned)tm->tm_mon % 12]; break;
        case 'c': strftime(tmp, sizeof(tmp), "%a %b %e %H:%M:%S %Y", tm); break;
        case 'C': NUM(f2, (tm->tm_year + 1900) / 100); break;
        case 'd': NUM(f2, tm->tm_mday); break;
        case 'D': strftime(tmp, sizeof(tmp), "%m/%d/%y", tm); break;
        case 'e': snprintf(tmp, sizeof(tmp), pad == '-' ? "%d" : pad == '0' ? "%02d" : "%2d", tm->tm_mday); break;
        case 'F': strftime(tmp, sizeof(tmp), "%Y-%m-%d", tm); break;
        case 'g': iso_week(tm, &isoy); NUM(f2, isoy % 100); break;
        case 'G': iso_week(tm, &isoy); NUM(f1, isoy); break;
        case 'H': NUM(f2, tm->tm_hour); break;
        case 'I': NUM(f2, hour12); break;
        case 'j': NUM(f3, tm->tm_yday + 1); break;
        case 'k': snprintf(tmp, sizeof(tmp), "%2d", tm->tm_hour); break;
        case 'l': snprintf(tmp, sizeof(tmp), "%2d", hour12); break;
        case 'm': NUM(f2, tm->tm_mon + 1); break;
        case 'M': NUM(f2, tm->tm_min); break;
        case 'n': out = "\n"; break;
        case 'N': out = "000000000"; break;
        case 'p': out = tm->tm_hour < 12 ? "AM" : "PM"; break;
        case 'P': out = tm->tm_hour < 12 ? "am" : "pm"; break;
        case 'r': strftime(tmp, sizeof(tmp), "%I:%M:%S %p", tm); break;
        case 'R': strftime(tmp, sizeof(tmp), "%H:%M", tm); break;
        case 's': {
            struct tm t = *tm;
            snprintf(tmp, sizeof(tmp), "%lld", tm_to_secs(&t) - tm->tm_gmtoff);
            break;
        }
        case 'S': NUM(f2, tm->tm_sec); break;
        case 't': out = "\t"; break;
        case 'T': strftime(tmp, sizeof(tmp), "%H:%M:%S", tm); break;
        case 'u': NUM(f1, tm->tm_wday ? tm->tm_wday : 7); break;
        case 'U': NUM(f2, (tm->tm_yday + 7 - tm->tm_wday) / 7); break;
        case 'V': NUM(f2, iso_week(tm, &isoy)); break;
        case 'w': NUM(f1, tm->tm_wday); break;
        case 'W': NUM(f2, (tm->tm_yday + 7 - (tm->tm_wday + 6) % 7) / 7); break;
        case 'x': strftime(tmp, sizeof(tmp), "%m/%d/%y", tm); break;
        case 'X': strftime(tmp, sizeof(tmp), "%H:%M:%S", tm); break;
        case 'y': NUM(f2, (tm->tm_year + 1900) % 100); break;
        case 'Y': NUM(f1, tm->tm_year + 1900); break;
        case 'z': case ':': {
            int colon = *f == ':';
            if (colon) { if (f[1] != 'z') { out = "%:"; break; } f++; }
            long off = tm->tm_gmtoff, a = off < 0 ? -off : off;
            snprintf(tmp, sizeof(tmp), colon ? "%c%02ld:%02ld" : "%c%02ld%02ld",
                     off < 0 ? '-' : '+', a / 3600, a / 60 % 60);
            break;
        }
        case 'Z': out = tm->tm_zone ? tm->tm_zone : "UTC"; break;
        case '%': out = "%"; break;
        case 0: f--; out = "%"; break;
        default: tmp[0] = '%'; tmp[1] = *f; tmp[2] = 0; break;
        }
#undef NUM
        size_t len = strlen(out);
        if (width > (int)len) {
            size_t fill = (size_t)width - len;
            char c = (pad == '_' || (!pad && !isdigit((unsigned char)*out))) ? ' ' : '0';
            if (n + fill >= max) return 0;
            while (fill--) s[n++] = c;
        }
        if (n + len >= max) return 0;
        for (size_t i = 0; i < len; i++)
            s[n++] = pad == '^' ? (char)toupper((unsigned char)out[i]) : out[i];
    }
    s[n] = 0;
    return n;
}

static const char *get_num(const char *s, int *v, int maxdig, int lo, int hi) {
    int n = 0, neg = 0, d = 0;
    while (isspace((unsigned char)*s)) s++;
    if (*s == '-' || *s == '+') { neg = *s == '-'; s++; }
    for (; d < maxdig && isdigit((unsigned char)*s); d++) n = n * 10 + (*s++ - '0');
    if (!d) return 0;
    if (neg) n = -n;
    if (n < lo || n > hi) return 0;
    *v = n;
    return s;
}

static const char *get_name(const char *s, const char *const *names, int count, int *v) {
    for (int i = 0; i < count; i++) {
        size_t full = strlen(names[i]);
        if (!strncasecmp(s, names[i], full)) { *v = i; return s + full; }
        if (!strncasecmp(s, names[i], 3)) { *v = i; return s + 3; }
    }
    return 0;
}

char *strptime(const char *s, const char *format, struct tm *tm) {
    int century = -1, year2 = -1, pm = -1, v;

    for (const char *f = format; *f && s; f++) {
        if (isspace((unsigned char)*f)) {
            while (isspace((unsigned char)*s)) s++;
            continue;
        }
        if (*f != '%') {
            if (*s++ != *f) return 0;
            continue;
        }
        f++;
        if (*f == 'E' || *f == 'O') f++;
        switch (*f) {
        case 'a': case 'A': s = get_name(s, wday_name, 7, &tm->tm_wday); break;
        case 'b': case 'B': case 'h': s = get_name(s, mon_name, 12, &tm->tm_mon); break;
        case 'c': s = strptime(s, "%a %b %e %H:%M:%S %Y", tm); break;
        case 'C': s = get_num(s, &century, 2, 0, 99); break;
        case 'd': case 'e': s = get_num(s, &tm->tm_mday, 2, 1, 31); break;
        case 'D': s = strptime(s, "%m/%d/%y", tm); break;
        case 'F': s = strptime(s, "%Y-%m-%d", tm); break;
        case 'H': case 'k': s = get_num(s, &tm->tm_hour, 2, 0, 23); break;
        case 'I': case 'l': s = get_num(s, &tm->tm_hour, 2, 1, 12); if (s && tm->tm_hour == 12) tm->tm_hour = 0; break;
        case 'j': s = get_num(s, &v, 3, 1, 366); if (s) tm->tm_yday = v - 1; break;
        case 'm': s = get_num(s, &v, 2, 1, 12); if (s) tm->tm_mon = v - 1; break;
        case 'M': s = get_num(s, &tm->tm_min, 2, 0, 59); break;
        case 'n': case 't': while (isspace((unsigned char)*s)) s++; break;
        case 'p': case 'P':
            while (isspace((unsigned char)*s)) s++;
            if (!strncasecmp(s, "am", 2)) pm = 0;
            else if (!strncasecmp(s, "pm", 2)) pm = 1;
            else return 0;
            s += 2;
            break;
        case 'r': s = strptime(s, "%I:%M:%S %p", tm); break;
        case 'R': s = strptime(s, "%H:%M", tm); break;
        case 's': {
            char *e;
            long long t = strtoll(s, &e, 10);
            if (e == s) return 0;
            time_t tt = (time_t)t;
            localtime_r(&tt, tm);
            s = e;
            break;
        }
        case 'S': s = get_num(s, &tm->tm_sec, 2, 0, 61); break;
        case 'T': s = strptime(s, "%H:%M:%S", tm); break;
        case 'u': s = get_num(s, &v, 1, 1, 7); if (s) tm->tm_wday = v % 7; break;
        case 'w': s = get_num(s, &tm->tm_wday, 1, 0, 6); break;
        case 'y': s = get_num(s, &year2, 2, 0, 99); break;
        case 'Y': s = get_num(s, &v, 4, 0, 9999); if (s) tm->tm_year = v - 1900; break;
        case 'z': {
            while (isspace((unsigned char)*s)) s++;
            if (*s == 'Z') { s++; break; }
            if (*s != '+' && *s != '-') return 0;
            s++;
            for (int i = 0; i < 4; i++) {
                if (i == 2 && *s == ':') s++;
                if (!isdigit((unsigned char)*s)) return 0;
                s++;
            }
            break;
        }
        case 'Z':
            while (isalpha((unsigned char)*s)) s++;
            break;
        case '%': if (*s++ != '%') return 0; break;
        default: return 0;
        }
    }
    if (!s) return 0;
    if (year2 >= 0) {
        int c = century >= 0 ? century : (year2 < 69 ? 20 : 19);
        tm->tm_year = c * 100 + year2 - 1900;
    } else if (century >= 0) tm->tm_year = century * 100 - 1900;
    if (pm == 1 && tm->tm_hour < 12) tm->tm_hour += 12;
    return (char *)s;
}
