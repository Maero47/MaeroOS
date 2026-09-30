#pragma once

#define LC_ALL 0
#define LC_CTYPE 1
#define MB_CUR_MAX 4
#define LC_CTYPE_MASK 1

typedef void *locale_t;

char *setlocale(int category, const char *locale);
locale_t newlocale(int mask, const char *locale, locale_t base);
locale_t uselocale(locale_t locale);
#define LC_NUMERIC  2
#define LC_TIME     3
#define LC_COLLATE  4
#define LC_MONETARY 5
#define LC_MESSAGES 6
