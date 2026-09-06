#include "../include/string.h"
#include "../include/stdlib.h"

void *memcpy(void *dst, const void *src, size_t n) {
    char *d = dst;
    const char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    char *d = dst;
    const char *s = src;
    if (d < s || d >= s + n) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    char *d = dst;
    while (n--) *d++ = (char)c;
    return dst;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (*s++) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    char *d = dst;
    /* POSIX: copy at most n bytes, zero-fill the remainder — never write
     * more than n total (the old version wrote n+1 for short sources). */
    while (n) {
        n--;
        if (!(*d++ = *src++))
            break;
    }
    while (n--) *d++ = '\0';
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst;
    while (*d) d++;
    while (n-- && (*d++ = *src++));
    *d = '\0';
    return dst;
}

char *strcat(char *dst, const char *src) {
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++));
    return dst;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : (char *)0;
}

char *strrchr(const char *s, int c) {
    const char *last = (char *)0;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    return (c == '\0') ? (char *)s : (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*n && *h == *n) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return (char *)0;
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

char *strndup(const char *s, size_t n) {
    size_t len = strlen(s);
    if (len > n) len = n;
    char *p = malloc(len + 1);
    if (p) { memcpy(p, s, len); p[len] = '\0'; }
    return p;
}

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    while (*s) {
        const char *a = accept;
        while (*a && *a != *s) a++;
        if (!*a) break;
        n++; s++;
    }
    return n;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (*s) {
        const char *r = reject;
        while (*r && *r != *s) r++;
        if (*r) break;
        n++; s++;
    }
    return n;
}

char *strpbrk(const char *s, const char *accept) {
    s += strcspn(s, accept);
    return *s ? (char *)s : (char *)0;
}

static char *_strtok_p = (char *)0;

char *strtok(char *s, const char *delim) {
    if (s) _strtok_p = s;
    if (!_strtok_p) return (char *)0;
    _strtok_p += strspn(_strtok_p, delim);
    if (!*_strtok_p) { _strtok_p = (char *)0; return (char *)0; }
    char *tok = _strtok_p;
    _strtok_p += strcspn(_strtok_p, delim);
    if (*_strtok_p) *_strtok_p++ = '\0';
    else             _strtok_p = (char *)0;
    return tok;
}

char *strtok_r(char *s, const char *delim, char **saveptr) {
    if (s) *saveptr = s;
    if (!*saveptr) return (char *)0;
    *saveptr += strspn(*saveptr, delim);
    if (!**saveptr) { *saveptr = (char *)0; return (char *)0; }
    char *tok = *saveptr;
    *saveptr += strcspn(*saveptr, delim);
    if (**saveptr) *(*saveptr)++ = '\0';
    else            *saveptr = (char *)0;
    return tok;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *ua = a, *ub = b;
    while (n--) {
        if (*ua != *ub) return (int)*ua - (int)*ub;
        ua++; ub++;
    }
    return 0;
}
