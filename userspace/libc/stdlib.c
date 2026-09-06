#include "../include/stdlib.h"
#include "../include/unistd.h"
#include "../include/string.h"

/* environ is defined in crt0.asm as a .bss word, exported as a global symbol */
extern char **environ;

/* sbrk: extend heap by increment bytes, returns old break */
void *sbrk(int increment) {
    void *cur = brk((void *)0);   /* query current break */
    if (increment == 0) return cur;
    void *new = (char *)cur + increment;
    void *ret = brk(new);
    /* If brk returns < new, it failed — return (void*)-1 */
    if ((char *)ret < (char *)new) return (void *)-1;
    return cur;
}

/* Simple bump allocator with free-list */
typedef struct blk {
    size_t       size;
    int          free;
    struct blk  *next;
} blk_t;

static blk_t *heap_head = (void *)0;

void *malloc(size_t size) {
    if (!size) return (void *)0;

    /* Align to 8 bytes */
    size = (size + 7) & ~(size_t)7;

    /* Search free list */
    blk_t *b = heap_head;
    while (b) {
        if (b->free && b->size >= size) {
            b->free = 0;
            return (char *)b + sizeof(blk_t);
        }
        b = b->next;
    }

    /* Expand heap */
    blk_t *nb = sbrk(sizeof(blk_t) + size);
    if (nb == (void *)-1) return (void *)0;
    nb->size = size;
    nb->free = 0;
    nb->next = (void *)0;

    /* Append to list */
    if (!heap_head) {
        heap_head = nb;
    } else {
        blk_t *tail = heap_head;
        while (tail->next) tail = tail->next;
        tail->next = nb;
    }

    return (char *)nb + sizeof(blk_t);
}

void free(void *ptr) {
    if (!ptr) return;
    blk_t *b = (blk_t *)((char *)ptr - sizeof(blk_t));
    b->free = 1;
}

char *getenv(const char *name) {
    if (!environ || !name) return (char *)0;
    int nlen = strlen(name);
    for (char **e = environ; *e; e++) {
        if (strncmp(*e, name, (size_t)nlen) == 0 && (*e)[nlen] == '=')
            return *e + nlen + 1;
    }
    return (char *)0;
}

int atoi(const char *s) {
    int n = 0, neg = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return neg ? -n : n;
}

long strtol(const char *s, char **endp, int base) {
    long n = 0; int neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        n = n * base + d;
        s++;
    }
    if (endp) *endp = (char *)s;
    return neg ? -n : n;
}

unsigned long strtoul(const char *s, char **endp, int base) {
    return (unsigned long)strtol(s, endp, base);
}

long atol(const char *s) { return strtol(s, (char **)0, 10); }

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (!size) { free(ptr); return (void *)0; }
    blk_t *b = (blk_t *)((char *)ptr - sizeof(blk_t));
    if (b->size >= size) return ptr;
    void *np = malloc(size);
    if (!np) return (void *)0;
    memcpy(np, ptr, b->size < size ? b->size : size);
    free(ptr);
    return np;
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }

/* Minimal system(): returns -1 (no /bin/sh contract on MaeroOS yet). */
int system(const char *command) {
    (void)command;
    return -1;
}

double atof(const char *s) {
    double v = 0.0, frac = 0.0, div = 1.0;
    int neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') v = v * 10.0 + (*s++ - '0');
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            frac = frac * 10.0 + (*s++ - '0');
            div *= 10.0;
        }
    }
    v += frac / div;
    return neg ? -v : v;
}

double fabs(double x) {
    union { double d; unsigned long long u; } v;
    v.d = x;
    v.u &= ~(1ULL << 63);
    return v.d;
}
