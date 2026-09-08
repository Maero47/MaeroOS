#pragma once

/*
 * pthread-lite for MaeroOS.
 *
 * Threads are kernel tasks in the caller's thread group (clone with
 * CLONE_VM | CLONE_THREAD | CLONE_SIGHAND ...); a pthread_t is the kernel tid
 * and pthread_join waits on the thread's tid word (CLONE_CHILD_CLEARTID
 * futex), like NPTL.  At most 64 live threads per process.  Mutexes are
 * futex-backed.  Known limit: errno is process-global.
 */

typedef int pthread_t;

typedef struct {
    volatile int state;   /* 0 unlocked, 1 locked, 2 locked w/ waiters */
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER { 0 }

int pthread_create(pthread_t *thread, const void *attr,
                   void *(*fn)(void *), void *arg);
int pthread_join(pthread_t thread, void **retval);
pthread_t pthread_self(void);

int pthread_mutex_init(pthread_mutex_t *m, const void *attr);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_mutex_destroy(pthread_mutex_t *m);
