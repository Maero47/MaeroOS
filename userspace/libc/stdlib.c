#include "../include/stdlib.h"
#include "../include/unistd.h"
#include "../include/string.h"
#include "../include/errno.h"
#include "../include/limits.h"
#include "../include/sys/mman.h"
#include "libc_lock.h"

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

/* The free list and the break are shared by every thread (browse, store and
 * linuxapps allocate from workers): each operation holds this lock. */
volatile int __libc_heap_lock;

/* Set by pthread_create.  The kernel's brk() keeps the break per thread, so
 * a worker's brk starts from a stale break and maps fresh pages over heap
 * another thread already uses.  Once a process has threads the heap grows
 * with anonymous mmap instead (whole chunks, carved under the heap lock). */
int __libc_threaded;
static char *arena_cur, *arena_end;

#define ARENA_CHUNK (256u * 1024u)

static void *heap_grow(size_t n) {
    if (!__libc_threaded) return sbrk((int)n);
    if ((size_t)(arena_end - arena_cur) < n) {
        size_t len = n > ARENA_CHUNK ? (n + 4095) & ~(size_t)4095 : ARENA_CHUNK;
        /* The kernel's mmap2 itself: libc's mmap() of anonymous memory is
         * malloc() (toybox_compat.c), which would take this lock again. */
        long r = syscall(192, 0L, (long)len, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1L, 0L);
        if (r < 0 && r > -4096) return (void *)-1;
        char *p = (char *)r;
        if (p != arena_end) arena_cur = p;     /* the old tail is left unused */
        arena_end = p + len;
    }
    void *r = arena_cur;
    arena_cur += n;
    return r;
}

static void *malloc_locked(size_t size);

void *malloc(size_t size) {
    libc_lock(&__libc_heap_lock);
    void *p = malloc_locked(size);
    libc_unlock(&__libc_heap_lock);
    return p;
}

static void *malloc_locked(size_t size) {
    if (!size) return (void *)0;

    /* sbrk() takes an int: anything that cannot be aligned and given a header
     * without passing INT_MAX would wrap to a tiny (or negative) request. */
    if (size > 0x7fffffffU - sizeof(blk_t) - 7) { errno = ENOMEM; return (void *)0; }

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
    blk_t *nb = heap_grow(sizeof(blk_t) + size);
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

/* Is p a block malloc() handed out?  munmap() (toybox_compat.c) frees the
 * anonymous "mappings" mmap() took from the heap, which in a threaded
 * process lie in the mmap range too. */
int __libc_heap_owns(const void *p) {
    int found = 0;
    libc_lock(&__libc_heap_lock);
    for (blk_t *b = heap_head; b; b = b->next)
        if ((const char *)b + sizeof(blk_t) == (const char *)p) { found = 1; break; }
    libc_unlock(&__libc_heap_lock);
    return found;
}

void free(void *ptr) {
    if (!ptr) return;
    blk_t *b = (blk_t *)((char *)ptr - sizeof(blk_t));
    libc_lock(&__libc_heap_lock);
    b->free = 1;
    libc_unlock(&__libc_heap_lock);
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

/* posix.c: the digits of s as a magnitude, saturated at limit (*over). */
unsigned long long __strto_u64(const char *s, char **endp, int base,
                               int *neg, unsigned long long limit, int *over);

/* C11 7.22.1.4: out of range gives LONG_MAX / LONG_MIN / ULONG_MAX and
 * errno ERANGE (callers such as inet_aton range-check the result, which a
 * wrapped value would pass). */
long strtol(const char *s, char **endp, int base) {
    int neg, over;
    unsigned long long v = __strto_u64(s, endp, base, &neg,
                                       (unsigned long long)LONG_MAX + 1, &over);
    if (over) return neg ? LONG_MIN : LONG_MAX;
    if (!neg && v > (unsigned long long)LONG_MAX) { errno = ERANGE; return LONG_MAX; }
    return neg ? (long)(0UL - (unsigned long)v) : (long)v;
}

unsigned long strtoul(const char *s, char **endp, int base) {
    int neg, over;
    unsigned long long v = __strto_u64(s, endp, base, &neg, ULONG_MAX, &over);
    if (over) return ULONG_MAX;
    return neg ? 0UL - (unsigned long)v : (unsigned long)v;
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
    if (size && nmemb > (size_t)-1 / size) { errno = ENOMEM; return (void *)0; }
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }


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

