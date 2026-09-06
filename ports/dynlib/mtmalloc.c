/* mtmalloc — multithreaded malloc stress: each thread churns malloc/free of
 * varied sizes.  Isolates whether our threads+TLS+shared-heap are sound under
 * concurrent allocation (what GLib/Pango do). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#define NT 4
#define ITERS 20000
static void *worker(void *arg){
    unsigned seed = (unsigned)(long)arg * 2654435761u;
    for (int i=0;i<ITERS;i++){
        seed = seed*1103515245u + 12345u;
        size_t sz = 8 + (seed % 4096);
        char *p = malloc(sz);
        if (!p) return (void*)1;
        memset(p, (int)(seed&0xff), sz);
        if ((seed & 7) == 0) { free(p); p = malloc(sz*2); if(!p) return (void*)2; }
        free(p);
    }
    return 0;
}
int main(void){
    pthread_t t[NT]; long bad=0;
    for (long i=0;i<NT;i++) if (pthread_create(&t[i],0,worker,(void*)i)) { printf("MTMALLOC_FAIL create\n"); return 1; }
    for (int i=0;i<NT;i++){ void*r; pthread_join(t[i],&r); bad += (long)r; }
    if (bad){ printf("MTMALLOC_FAIL bad=%ld\n", bad); return 1; }
    printf("MTMALLOC_OK threads=%d iters=%d\n", NT, ITERS);
    return 0;
}
