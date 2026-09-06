#pragma once
#include <stddef.h>

#define GRND_NONBLOCK 0x0001
#define GRND_RANDOM   0x0002

int getrandom(void *buf, unsigned int buflen, unsigned int flags);
int getentropy(void *buf, size_t buflen);
