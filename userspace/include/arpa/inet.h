#pragma once

#include <netinet/in.h>

#define INET_ADDRSTRLEN  16
#define INET6_ADDRSTRLEN 46

uint32_t inet_addr(const char *s);
int inet_aton(const char *s, struct in_addr *out);
char *inet_ntoa(struct in_addr in);
int inet_pton(int af, const char *src, void *dst);
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
