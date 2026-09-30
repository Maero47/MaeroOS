#pragma once
#include <sys/types.h>
#include <unistd.h>

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define UTIME_NOW 1073741823L
#define UTIME_OMIT 1073741822L

typedef int clockid_t;

time_t time(time_t *tloc);
struct tm *localtime(const time_t *timep);
struct tm *localtime_r(const time_t *timep, struct tm *result);
struct tm *gmtime(const time_t *timep);
char *ctime(const time_t *timep);
size_t strftime(char *s, size_t max, const char *format, const struct tm *tm);
struct tm *gmtime_r(const time_t *timep, struct tm *result);
time_t timegm(struct tm *tm);
char *asctime(const struct tm *tm);
char *asctime_r(const struct tm *tm, char *buf);
char *ctime_r(const time_t *timep, char *buf);
double difftime(time_t a, time_t b);
extern char *tzname[2];
extern long timezone;
extern int daylight;
char *strptime(const char *buf, const char *format, struct tm *tm);
time_t mktime(struct tm *tm);
void tzset(void);
unsigned int sleep(unsigned int seconds);
int clock_gettime(clockid_t clk_id, struct timespec *tp);
int utimensat(int dirfd, const char *path, const struct timespec times[2],
              int flags);
int futimens(int fd, const struct timespec times[2]);
