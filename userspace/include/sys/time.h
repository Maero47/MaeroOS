#pragma once
#include <unistd.h>

int settimeofday(const struct timeval *tv, const void *tz);
