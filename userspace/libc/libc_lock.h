#pragma once
/*
 * Locks for libc's own process-wide state, which every pthread shares: the
 * heap (stdlib.c) and the stdio buffers (stdio.c).  A futex mutex as in
 * pthread.c ("Futexes Are Tricky", Drepper, mutex 2): 0 free, 1 held, 2 held
 * with waiters.  Not recursive.  Lock order: stdio before heap (fclose frees
 * under the stdio lock; malloc never touches stdio).  fork() takes both so
 * the child does not inherit a lock some other thread held mid-update.
 */
#include "../include/syscall.h"

extern volatile int __libc_heap_lock;
extern volatile int __libc_stdio_lock;

static inline void libc_lock(volatile int *l) {
    if (__sync_val_compare_and_swap(l, 0, 1) == 0) return;
    while (__sync_lock_test_and_set(l, 2) != 0)
        syscall4(240, (int)l, 0 /* FUTEX_WAIT */, 2, 0);
}

static inline void libc_unlock(volatile int *l) {
    /* xchg is a full barrier on x86: the protected writes are visible first. */
    if (__sync_lock_test_and_set(l, 0) == 2)
        syscall4(240, (int)l, 1 /* FUTEX_WAKE */, 1, 0);
}
