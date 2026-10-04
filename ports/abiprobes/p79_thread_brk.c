/*
 * P79 the program break belongs to the process, not to a thread.
 *
 * Linux keeps brk in the mm: every thread sees and moves the same break.
 *
 * MaeroOS before: each thread had its own copy of the break, taken at
 * clone(), so a worker's brk(0) returned a stale break and growing it
 * mapped fresh zero pages over the heap the main thread had already used.
 *
 * The worker is created first; the main thread then moves the break up and
 * fills the new heap; the worker must see the moved break, move it further,
 * and the main thread must see that and find its data intact.
 */
#define PROBE_NAME "p79_thread_brk"
#include "probe.h"
#include <semaphore.h>

static sem_t g_go, g_done;
static uintptr_t g_seen, g_set;

static uintptr_t xbrk(uintptr_t a) { return (uintptr_t)syscall(SYS_brk, a); }

static void *worker(void *arg)
{
    (void)arg;
    sem_wait(&g_go);
    g_seen = xbrk(0);
    g_set = xbrk(g_seen + 8192);
    sem_post(&g_done);
    return NULL;
}

int main(void)
{
    probe_watchdog(60);
    sem_init(&g_go, 0, 0);
    sem_init(&g_done, 0, 0);
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) != 0) probe_fail("pthread_create");

    uintptr_t b0 = xbrk(0);
    uintptr_t b1 = xbrk(b0 + 8192);
    if (b1 != b0 + 8192) probe_fail("brk grow: %#lx -> %#lx", (unsigned long)b0, (unsigned long)b1);
    memset((void *)b0, 0xa5, 8192);

    sem_post(&g_go);
    sem_wait(&g_done);
    pthread_join(t, NULL);
    probe_info("main break %#lx, worker saw %#lx and set %#lx",
               (unsigned long)b1, (unsigned long)g_seen, (unsigned long)g_set);
    if (g_seen != b1)
        probe_fail("the worker's brk(0) is %#lx, the process's break is %#lx",
                   (unsigned long)g_seen, (unsigned long)b1);
    if (g_set != b1 + 8192) probe_fail("the worker could not grow the break");
    if (xbrk(0) != g_set)
        probe_fail("main's brk(0) %#lx does not see the worker's break %#lx",
                   (unsigned long)xbrk(0), (unsigned long)g_set);
    for (int i = 0; i < 8192; i++)
        if (((unsigned char *)b0)[i] != 0xa5)
            probe_fail("heap byte %d lost after the worker's brk", i);
    memset((void *)b1, 1, 8192);           /* the worker's part is mapped */
    probe_pass();
}
