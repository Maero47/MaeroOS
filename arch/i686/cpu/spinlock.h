#ifndef ARCH_I686_SPINLOCK_H
#define ARCH_I686_SPINLOCK_H

#include <stdint.h>

/* Minimal test-and-set spinlock for SMP.  On a single CPU it is always
 * uncontended (the holder is the only CPU that can run kernel code).  Use
 * `pause` in the spin to be friendly to the other hyperthread/core. */
typedef struct { volatile uint32_t locked; } spinlock_t;

static inline void spin_init(spinlock_t *l) { l->locked = 0; }

static inline void spin_lock(spinlock_t *l) {
    while (__sync_lock_test_and_set(&l->locked, 1))
        while (l->locked) __asm__ volatile("pause");
}

static inline int spin_trylock(spinlock_t *l) {
    return __sync_lock_test_and_set(&l->locked, 1) == 0;
}

static inline void spin_unlock(spinlock_t *l) {
    __sync_lock_release(&l->locked);
}

#endif /* ARCH_I686_SPINLOCK_H */
