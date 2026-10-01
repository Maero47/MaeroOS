/* Host test for drivers/e1000_txq.h; run by tools/test_e1000_tx.py. */
#include <stdio.h>
#include <string.h>
#include "e1000_txq.h"

#define N 32

static int dd[N];                 /* simulated DD bits */
static int fails;

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static int done(uint32_t idx, void *ctx) { (void)ctx; return dd[idx]; }

/* The chip finishes the oldest unfinished posted frame. */
static void complete_one(struct e1000_txq *q) {
    for (uint32_t i = q->clean; i != q->next; i = (i + 1) % q->size)
        if (!dd[i]) { dd[i] = 1; return; }
}

static uint32_t send(struct e1000_txq *q) {
    e1000_txq_reclaim(q, done, 0);
    if (e1000_txq_full(q)) return (uint32_t)-1;
    uint32_t idx = e1000_txq_post(q);
    dd[idx] = 0;
    return idx;
}

int main(void) {
    struct e1000_txq q;

    /* 1. A ring the chip never drains takes N-1 frames, not N. */
    memset(dd, 0, sizeof(dd));
    e1000_txq_init(&q, N);
    int posted = 0;
    while (send(&q) != (uint32_t)-1) posted++;
    CHECK(posted == N - 1, "posted %d into a %d-slot ring", posted, N);
    CHECK(q.next != q.clean, "full ring has TDT == TDH");

    /* 2. Real stall: no completions.  Fires 200 ticks after it is first
     * seen, not before. */
    uint32_t t0 = 5000;
    CHECK(!e1000_txq_stalled(&q, t0, done, 0), "stalled at first sight");
    CHECK(!e1000_txq_stalled(&q, t0 + 199, done, 0), "stalled after 199 ticks");
    CHECK(e1000_txq_stalled(&q, t0 + 200, done, 0), "no stall after 200 ticks");

    /* 3. The review's case: a bulk sender whose send reclaims each
     * completion just before the watchdog looks (so the watchdog itself
     * never frees anything), the ring never empty, for 10 s.  Progress
     * found by the send must count: no stall. */
    memset(dd, 0, sizeof(dd));
    e1000_txq_init(&q, N);
    for (int i = 0; i < 8; i++) send(&q);
    int bogus = 0;
    for (uint32_t t = 1; t <= 1000; t++) {
        complete_one(&q);                 /* chip finishes one frame */
        send(&q);                         /* send reclaims it, posts one */
        if (e1000_txq_stalled(&q, t, done, 0)) bogus++;
        CHECK(e1000_txq_pending(&q) > 0, "ring went empty at tick %u", t);
    }
    CHECK(bogus == 0, "%d bogus stalls during a steady upload", bogus);

    /* 4. An idle ring never stalls, and the timer is cleared by draining. */
    for (uint32_t i = 0; i < N; i++) dd[i] = 1;
    CHECK(!e1000_txq_stalled(&q, 999999, done, 0), "drained ring stalled");
    CHECK(q.stall_tick == 0, "drained ring kept its stall timer");

    /* 5. pit tick 0 (and wraparound) still arms the timer. */
    e1000_txq_init(&q, N);
    memset(dd, 0, sizeof(dd));
    send(&q);
    CHECK(!e1000_txq_stalled(&q, 0, done, 0), "stalled at tick 0");
    CHECK(q.stall_tick != 0, "timer not armed at tick 0");
    e1000_txq_init(&q, N);
    send(&q);
    CHECK(!e1000_txq_stalled(&q, 0xFFFFFF80U, done, 0), "stalled before wrap");
    CHECK(e1000_txq_stalled(&q, 0x48U, done, 0), "no stall across tick wrap");

    if (fails) return 1;
    printf("test_e1000_tx: ok\n");
    return 0;
}
