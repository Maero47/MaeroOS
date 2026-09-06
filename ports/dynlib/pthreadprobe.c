/* pthreadprobe — proves real multithreading: spawns N threads that each bump a
 * mutex-guarded shared counter many times, joins them, and checks the total.
 * If it prints THREADS_OK the kernel's clone(CLONE_VM|SETTLS|CHILD_CLEARTID),
 * per-thread TLS, futex mutexes, and pthread_join all work together. */
#include <stdio.h>
#include <pthread.h>

#define NTHREADS 4
#define NITER    50000

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long counter = 0;
static __thread int tls_check;   /* per-thread storage — must not collide */

static void *worker(void *arg) {
    int id = (int)(long)arg;
    tls_check = id * 7 + 1;       /* write our own TLS slot */
    for (int i = 0; i < NITER; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }
    /* If TLS were shared, another thread would have clobbered this. */
    if (tls_check != id * 7 + 1) return (void *)1;
    return (void *)0;
}

int main(void) {
    pthread_t t[NTHREADS];
    long bad = 0;
    for (long i = 0; i < NTHREADS; i++)
        if (pthread_create(&t[i], 0, worker, (void *)i) != 0) {
            printf("THREADS_FAIL create\n"); return 1;
        }
    for (int i = 0; i < NTHREADS; i++) {
        void *r;
        pthread_join(t[i], &r);
        bad += (long)r;
    }
    long expect = (long)NTHREADS * NITER;
    if (bad) { printf("THREADS_FAIL tls\n"); return 1; }
    if (counter != expect) {
        printf("THREADS_FAIL count=%ld want=%ld\n", counter, expect); return 1;
    }
    printf("THREADS_OK count=%ld threads=%d\n", counter, NTHREADS);
    fflush(stdout);
    return 0;
}
