#include "../include/pthread.h"
#include "../include/stdint.h"
#include "../include/stdlib.h"
#include "../include/syscall.h"
#include "../include/unistd.h"

/*
 * pthread-lite: threads are kernel tasks created with the same clone flags a
 * real libc uses (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
 * CLONE_THREAD | CLONE_SYSVSEM | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID),
 * so they are members of the calling thread group: getpid() is shared, signal
 * dispositions are shared, and a thread exit is not a child exit (no SIGCHLD,
 * not waitable).  Join works like NPTL's: the kernel zeroes the thread's tid
 * word and futex-wakes it when the thread exits (CLONE_CHILD_CLEARTID), and
 * pthread_join sleeps on that word.  A pthread_t is still the kernel tid; a
 * small table maps it to the thread's descriptor.  See pthread.h for limits.
 */

#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SYSVSEM        0x00040000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define THREAD_STACK  (256 * 1024)
#define MAX_THREADS   64

/* libc/clone.asm: pre-pushes fn+arg on the child stack, child calls fn */
extern int __clone_thread(int flags, void *stack_top,
                          void (*fn)(void *), void *arg, int *tidptr);

typedef struct {
    /* CLONE_CHILD_CLEARTID word: the kernel writes the tid here at create and
     * ZEROES it (plus a FUTEX_WAKE) when the thread exits.  It is the join
     * futex, never an identity — see `id`. */
    volatile int tid;
    /* The tid as a stable join key.  pthread_join must still find the
     * descriptor of a thread that has already exited (the common case), and by
     * then the kernel has cleared `tid`; keying the lookup on that word made
     * join return -1 without freeing the stack or the table slot, so both
     * leaked and pthread_create failed for good after MAX_THREADS cycles.
     * The kernel never touches this field. */
    int          id;
    void *(*fn)(void *);
    void *arg;
    void *ret;
    char *stack;            /* the malloc'd block holding this record + stack */
} thread_desc_t;

static thread_desc_t *threads[MAX_THREADS];
static pthread_mutex_t threads_lock = PTHREAD_MUTEX_INITIALIZER;

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

static int futex(volatile int *uaddr, int op, int val) {
    return syscall4(240, (int)(uintptr_t)uaddr, op, val, 0);
}

static void thread_trampoline(void *p) {
    thread_desc_t *st = (thread_desc_t *)p;
    st->ret = st->fn(st->arg);
}

int pthread_create(pthread_t *thread, const void *attr,
                   void *(*fn)(void *), void *arg) {
    (void)attr;
    char *block = (char *)malloc(THREAD_STACK + sizeof(thread_desc_t) + 16);
    thread_desc_t *st;
    int tid, slot = -1;

    if (!block) return -1;
    /* Park the descriptor at the base; the stack grows down from the top. */
    st = (thread_desc_t *)block;
    st->fn = fn;
    st->arg = arg;
    st->ret = 0;
    st->tid = 0;
    st->id = 0;             /* set once the tid is known; 0 matches no thread */
    st->stack = block;

    pthread_mutex_lock(&threads_lock);
    for (int i = 0; i < MAX_THREADS; i++)
        if (!threads[i]) { threads[i] = st; slot = i; break; }
    pthread_mutex_unlock(&threads_lock);
    if (slot < 0) { free(block); return -1; }

    {
        char *top = block + sizeof(thread_desc_t) + THREAD_STACK;
        top -= (unsigned)(uintptr_t)top & 15;     /* 16-byte align */
        tid = __clone_thread(CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                             CLONE_THREAD | CLONE_SYSVSEM |
                             CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID,
                             top, thread_trampoline, st, (int *)&st->tid);
    }
    if (tid < 0) {
        pthread_mutex_lock(&threads_lock);
        threads[slot] = 0;
        pthread_mutex_unlock(&threads_lock);
        free(block);
        return -1;
    }
    st->id = tid;           /* only the creator writes this; join reads it */
    if (thread) *thread = tid;
    return 0;
}

int pthread_join(pthread_t thread, void **retval) {
    thread_desc_t *st = 0;
    int slot = -1;

    pthread_mutex_lock(&threads_lock);
    for (int i = 0; i < MAX_THREADS; i++)
        if (threads[i] && threads[i]->id == thread) { st = threads[i]; slot = i; break; }
    pthread_mutex_unlock(&threads_lock);
    if (!st) return -1;

    /* CLONE_CHILD_CLEARTID: the kernel stores 0 in st->tid and wakes this
     * futex when the thread has exited (NPTL pthread_join does exactly this). */
    for (;;) {
        int cur = st->tid;
        if (cur == 0) break;
        futex(&st->tid, FUTEX_WAIT, cur);
    }

    if (retval) *retval = st->ret;
    pthread_mutex_lock(&threads_lock);
    threads[slot] = 0;
    pthread_mutex_unlock(&threads_lock);
    /* The thread is gone (the kernel cleared tid after it stopped running on
     * this stack), so the stack block can be recycled. */
    free(st->stack);
    return 0;
}

pthread_t pthread_self(void) {
    return syscall0(224);   /* gettid */
}

/* ── futex-backed mutex ─────────────────────────────────────────────────── */

int pthread_mutex_init(pthread_mutex_t *m, const void *attr) {
    (void)attr;
    m->state = 0;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m) {
    /* Fast path: 0 → 1.  Contended: mark 2 and futex-wait while locked. */
    if (__sync_val_compare_and_swap(&m->state, 0, 1) == 0)
        return 0;
    while (__sync_lock_test_and_set(&m->state, 2) != 0)
        futex(&m->state, FUTEX_WAIT, 2);
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
    int prev = __sync_lock_test_and_set(&m->state, 0);
    if (prev == 2)
        futex(&m->state, FUTEX_WAKE, 1);
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m) {
    m->state = 0;
    return 0;
}
