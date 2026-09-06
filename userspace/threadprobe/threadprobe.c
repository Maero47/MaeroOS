#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <unistd.h>

/*
 * threadprobe — exercises CLONE_VM threads end to end:
 *   1. two threads increment a shared counter under a futex mutex
 *   2. shared memory is REALLY shared (no COW divergence)
 *   3. getpid() is the group id in every thread; gettid() differs
 *   4. set_thread_area TLS: each thread sees its own %gs-based slot
 */

#define LOOPS 50000

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int counter;
static volatile int t1_pid, t2_pid, t1_tid, t2_tid;

struct user_desc_min { int entry_number; unsigned base_addr; };

static void tls_setup(unsigned *slot) {
    struct user_desc_min ud;
    unsigned sel;

    ud.entry_number = -1;
    ud.base_addr = (unsigned)(uintptr_t)slot;
    if (syscall1(243, (int)(uintptr_t)&ud) < 0) return;
    sel = (unsigned)(ud.entry_number * 8 + 3);
    __asm__ volatile("mov %0, %%gs" :: "r"(sel));
}

static unsigned tls_read(void) {
    unsigned v;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(v));
    return v;
}

/* Cross-thread fd visibility (Linux CLONE_FILES): a thread created BEFORE main
 * opens an fd must still see that fd.  With a per-thread COPIED fd table the
 * sibling never sees it (write → EBADF) — that was the bug that mmap'd the
 * compositor's render buffer EBADF and blocked all painting. */
static volatile int fd_go, fd_pub = -1, fd_result = -99;

static void *fdworker(void *arg) {
    (void)arg;
    while (!fd_go) { }                 /* user-mode spin: timer preempts us */
    int r = (int)write(fd_pub, "x", 1);
    fd_result = (r == 1) ? 0 : -1;
    return 0;
}

static void *worker(void *arg) {
    int idx = (int)(uintptr_t)arg;
    unsigned tls_slot[1];

    tls_slot[0] = 0x1000u + (unsigned)idx;
    tls_setup(tls_slot);

    if (idx == 1) { t1_pid = getpid(); t1_tid = syscall0(224); }
    else          { t2_pid = getpid(); t2_tid = syscall0(224); }

    for (int i = 0; i < LOOPS; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }

    if (tls_read() != 0x1000u + (unsigned)idx) {
        printf("threadprobe: TLS clobbered in thread %d\n", idx);
        exit(1);
    }
    return 0;
}

int main(void) {
    pthread_t a, b;
    int mypid = getpid();

    /* Cross-thread fd visibility test (CLONE_FILES). */
    pthread_t fw;
    if (pthread_create(&fw, 0, fdworker, 0) != 0) {
        printf("threadprobe: create failed\n");
        return 1;
    }
    int pfd[2];
    if (pipe(pfd) != 0) { printf("threadprobe: pipe failed\n"); return 1; }
    fd_pub = pfd[1];          /* opened AFTER fdworker was created */
    fd_go  = 1;
    pthread_join(fw, 0);
    if (fd_result != 0) {
        printf("threadprobe: sibling can't see fd %d opened after it started "
               "(CLONE_FILES broken)\n", pfd[1]);
        return 1;
    }
    char c = 0;
    if (read(pfd[0], &c, 1) != 1 || c != 'x') {
        printf("threadprobe: cross-thread pipe data wrong\n");
        return 1;
    }

    if (pthread_create(&a, 0, worker, (void *)1) != 0) {
        printf("threadprobe: create failed\n");
        return 1;
    }
    if (pthread_create(&b, 0, worker, (void *)2) != 0) {
        printf("threadprobe: create failed\n");
        return 1;
    }
    pthread_join(a, 0);
    pthread_join(b, 0);

    if (counter != 2 * LOOPS) {
        printf("threadprobe: counter %d != %d (lost updates)\n",
               counter, 2 * LOOPS);
        return 1;
    }
    if (t1_pid != mypid || t2_pid != mypid) {
        printf("threadprobe: getpid mismatch (%d/%d vs %d)\n",
               t1_pid, t2_pid, mypid);
        return 1;
    }
    if (t1_tid == t2_tid || t1_tid == mypid) {
        printf("threadprobe: tids not distinct (%d/%d)\n", t1_tid, t2_tid);
        return 1;
    }
    printf("threadprobe ok\n");
    return 0;
}
