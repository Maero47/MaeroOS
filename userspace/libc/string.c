#include "../include/string.h"
#include "../include/stdlib.h"

/*
 * The block primitives are string instructions, not byte loops.  The
 * compositor (desktop, maeroX) moves whole scanlines of 32-bit pixels through
 * these, and a byte loop ran at well under a byte per cycle: the desktop's
 * per-frame shadow compare and copy of a 4 MiB frame alone cost several ms.
 * `rep movsl` / `rep stosl` move a dword per step (and on any CPU of the last
 * decade the microcode streams whole cache lines), with the 0-3 byte tail
 * done by `rep movsb`.  Userspace is built -mno-sse, and these stay that way:
 * no FPU/SSE state is touched.
 */
void *memcpy(void *dst, const void *src, size_t n) {
    void *d = dst;
    const void *s = src;
    size_t words = n >> 2, tail = n & 3;
    __asm__ volatile("rep movsl\n\t"
                     "movl %3, %%ecx\n\t"
                     "rep movsb"
                     : "+D"(d), "+S"(s), "+c"(words)
                     : "r"(tail)
                     : "memory");
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d == s || n == 0) return dst;
    if (d < s || d >= s + n) return memcpy(dst, src, n);
    /* Overlapping with dst above src: copy backwards, a dword at a time.
     * Not `std; rep movs`: sigreturn here does not restore the direction
     * flag, so a signal taken mid-copy would resume it running forwards. */
    d += n;
    s += n;
    while (n & 3) { *--d = *--s; n--; }
    {
        unsigned *dw = (unsigned *)(void *)d;
        const unsigned *sw = (const unsigned *)(const void *)s;
        for (n >>= 2; n; n--) *--dw = *--sw;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    void *d = dst;
    unsigned v = (unsigned char)c;
    size_t words = n >> 2, tail = n & 3;
    v |= v << 8;
    v |= v << 16;
    __asm__ volatile("rep stosl\n\t"
                     "movl %3, %%ecx\n\t"
                     "rep stosb"
                     : "+D"(d), "+c"(words)
                     : "a"(v), "r"(tail)
                     : "memory");
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
    /* Skip the equal prefix a dword at a time (the common case for the
     * compositor's scanline compares is "all equal"), then settle the order
     * on the first differing bytes. */
    while (n >= 16) {
        const unsigned *wa = (const unsigned *)(const void *)ua;
        const unsigned *wb = (const unsigned *)(const void *)ub;
        if ((wa[0] ^ wb[0]) | (wa[1] ^ wb[1]) | (wa[2] ^ wb[2]) | (wa[3] ^ wb[3]))
            break;
        ua += 16; ub += 16; n -= 16;
    }
    while (n--) {
        if (*ua != *ub) return (int)*ua - (int)*ub;
        ua++; ub++;
    }
    return 0;
}
