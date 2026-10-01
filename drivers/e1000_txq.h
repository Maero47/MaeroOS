#pragma once
/*
 * e1000 TX ring bookkeeping, kept free of hardware access so the host test
 * (tools/test_e1000_tx.py) can drive it with simulated completions.
 *
 * Slots clean .. next-1 are posted to the chip.  One slot always stays
 * empty: TDT == TDH means "ring empty" to the chip (8254x SDM 3.3.1), so a
 * ring filled to the last slot would look empty to it and never drain.
 *
 * Stall detection: stall_tick is the tick at which posted frames were first
 * seen making no progress, 0 when not stalled.  Any completion, whoever
 * reclaims it (a send or the watchdog), clears it.
 */
#include <stdint.h>

#define E1000_TXQ_STALL_TICKS 200   /* 2 s at the PIT's 100 Hz */

struct e1000_txq {
    uint32_t size;          /* descriptors in the ring */
    uint32_t next;          /* next slot to post */
    uint32_t clean;         /* oldest posted slot not yet reclaimed */
    uint32_t stall_tick;
};

/* done(idx, ctx): has the chip finished the frame in slot idx (DD set)? */
typedef int (*e1000_txq_done_fn)(uint32_t idx, void *ctx);

static inline void e1000_txq_init(struct e1000_txq *q, uint32_t size) {
    q->size = size;
    q->next = q->clean = q->stall_tick = 0;
}

static inline uint32_t e1000_txq_pending(const struct e1000_txq *q) {
    return (q->next + q->size - q->clean) % q->size;
}

static inline int e1000_txq_full(const struct e1000_txq *q) {
    return (q->next + 1) % q->size == q->clean;
}

/* Advance clean over finished slots; returns how many were freed. */
static inline uint32_t e1000_txq_reclaim(struct e1000_txq *q,
                                         e1000_txq_done_fn done, void *ctx) {
    uint32_t freed = 0;
    while (q->clean != q->next && done(q->clean, ctx)) {
        q->clean = (q->clean + 1) % q->size;
        freed++;
    }
    if (freed)
        q->stall_tick = 0;      /* progress, wherever it was noticed */
    return freed;
}

/* Claim the slot to post (caller checked !full); returns its index. */
static inline uint32_t e1000_txq_post(struct e1000_txq *q) {
    uint32_t idx = q->next;
    q->next = (idx + 1) % q->size;
    return idx;
}

/* 1 when frames have been posted with no completion for STALL_TICKS. */
static inline int e1000_txq_stalled(struct e1000_txq *q, uint32_t now,
                                    e1000_txq_done_fn done, void *ctx) {
    e1000_txq_reclaim(q, done, ctx);
    if (q->clean == q->next) {
        q->stall_tick = 0;
        return 0;
    }
    if (!q->stall_tick) {
        q->stall_tick = now ? now : 1;
        return 0;
    }
    return now - q->stall_tick >= E1000_TXQ_STALL_TICKS;
}
