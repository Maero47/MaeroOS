#pragma once

#include <netinet/in.h>

uint32_t inet_addr(const char *s);
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
