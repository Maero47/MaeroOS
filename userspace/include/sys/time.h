#pragma once
#include <unistd.h>

int settimeofday(const struct timeval *tv, const void *tz);

#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

struct itimerval {
    struct timeval it_interval;   /* reload value; zero = one-shot */
    struct timeval it_value;      /* time to the next expiry; zero = disarmed */
};

int getitimer(int which, struct itimerval *curr_value);
int setitimer(int which, const struct itimerval *new_value,
              struct itimerval *old_value);
