#include "../include/pthread.h"
#include "../include/stdint.h"
#include "../include/stdlib.h"
#include "../include/syscall.h"
#include "../include/sys/wait.h"
#include "../include/unistd.h"

/*
 * pthread-lite: threads are clone(CLONE_VM | SIGCHLD) kernel tasks sharing
 * this address space; join is waitpid on the tid.  See pthread.h for limits.
 */

#define CLONE_VM      0x100
#define CLONE_FILES   0x400     /* share the fd table — POSIX threads share fds */
#define CLONE_SIGCHLD 17        /* SIGCHLD in the low exit_signal byte */
#define THREAD_STACK  (256 * 1024)

/* libc/clone.asm: pre-pushes fn+arg on the child stack, child calls fn */
extern int __clone_thread(int flags, void *stack_top,
                          void (*fn)(void *), void *arg);

typedef struct {
    void *(*fn)(void *);
    void *arg;
    void *stack;            /* freed lazily: see note in pthread_join */
} thread_start_t;

static void thread_trampoline(void *p) {
    thread_start_t *st = (thread_start_t *)p;
    st->fn(st->arg);
    /* Return value travels through exit status only as 0; full void*
     * returns would need a shared result slot (add when needed). */
}

int pthread_create(pthread_t *thread, const void *attr,
                   void *(*fn)(void *), void *arg) {
    (void)attr;
    char *stack = (char *)malloc(THREAD_STACK + sizeof(thread_start_t) + 16);
    thread_start_t *st;
    int tid;

    if (!stack) return -1;
    /* Park the start record at the base; stack grows down from the top. */
    st = (thread_start_t *)stack;
    st->fn = fn;
    st->arg = arg;
    st->stack = stack;
    {
        char *top = stack + THREAD_STACK;
        top -= (unsigned)(uintptr_t)top & 15;     /* 16-byte align */
        tid = __clone_thread(CLONE_VM | CLONE_FILES | CLONE_SIGCHLD, top,
                             thread_trampoline, st);
    }
    if (tid < 0) {
        free(stack);
        return -1;
    }
    if (thread) *thread = tid;
    return 0;
}

int pthread_join(pthread_t thread, void **retval) {
    int status = 0;

    if (waitpid(thread, &status, 0) != thread)
        return -1;
    if (retval) *retval = 0;
    /*
     * The thread stack is intentionally NOT freed here: free() walks the
     * shared heap and the stack block is safest to recycle from the joiner
     * only after the kernel has reaped the thread — which waitpid above
     * guarantees, so reclaim it now via a small registry-free trick: the
     * start record stores the base pointer at a known spot... but we no
     * longer know it post-join.  Leaking ~256K per joined thread is the
     * v1 trade-off; long-running apps should pool threads.
     */
    return 0;
}

pthread_t pthread_self(void) {
    return syscall0(224);   /* gettid */
}

/* ── futex-backed mutex ─────────────────────────────────────────────────── */

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

static int futex(volatile int *uaddr, int op, int val) {
    return syscall4(240, (int)(uintptr_t)uaddr, op, val, 0);
}

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
