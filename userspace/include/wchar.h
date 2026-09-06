#pragma once
#include <stddef.h>

typedef int wchar_t;

int wcwidth(wchar_t wc);
int wcrtomb(char *s, wchar_t wc, void *ps);
