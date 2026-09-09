#include "tsc.h"
#include "pit.h"
#include "../../../kernel/printk.h"
#include <io.h>

/*
 * TSC-interpolated tick clock.
 *
 * The PIT fires every 10 ms and pit_handler() calls tsc_tick_sample() just
 * before it increments the tick count.  We remember the TSC value at that
 * instant; a reader computes
 *
 *     now = ticks * 10 ms + (rdtsc() - tsc_at_tick) * ns_per_cycle
 *
 * with the second term clamped below one tick, so the clock is monotonic
 * across tick boundaries and can never run ahead of the tick clock the
 * scheduler uses for timeouts.
 *
 * Calibration is passive: the TSC delta between consecutive ticks is exactly
 * one tick period of cycles.  A late-delivered interrupt only makes a sample
 * LARGER, so the minimum over the first TSC_CAL_SAMPLES ticks is a robust
 * estimate of the true rate under both KVM and TCG (where the TSC follows
 * the host clock).  Boot reaches user space in well under those few ticks, so
 * the first clock reader waits (bounded) for the calibration to complete
 * instead of handing out a coarse value once and a fine one later.
 *
 * All arithmetic is 32x32->64 multiply plus shifts: the kernel links no
 * libgcc 64-bit division helpers.
 */

#define TSC_CAL_SAMPLES 4
#define TSC_MULT_SHIFT  24

static inline uint64_t rdtsc64(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Published by the tick: seqcount protects the (tsc_at_tick, tick) pair so a
 * reader on any CPU sees a consistent snapshot (Linux seqcount_t pattern).
 * The tick counter itself lives in pit.c; we snapshot it here too so the pair
 * is read atomically with respect to the interrupt that updates both. */
static volatile uint32_t g_seq;
static volatile uint32_t g_tick_snap;       /* tick count AFTER this sample */
static volatile uint64_t g_tsc_at_tick;

/* Calibration state. */
static uint64_t g_prev_tsc;
static uint32_t g_samples;
static uint32_t g_cycles_per_tick;          /* min observed cycles / tick    */
static uint32_t g_ns_mult;                  /* ns = cycles * mult >> SHIFT  */
static volatile int g_calibrated;
static uint32_t udiv64_32(uint64_t n, uint32_t d);
static uint64_t g_cal_tsc0;                 /* TSC at the first accepted sample */
static uint32_t g_cal_ticks;                /* ticks elapsed since g_cal_tsc0   */

/* Non-zero once the PIT channel-2 calibration below has produced a rate; the
 * passive tick-interval path is then not used at all. */
static int g_hw_calibrated;

static void tsc_set_rate(uint32_t cycles_per_tick, const char *how) {
    /* mult = TICK_NS * 2^SHIFT / cycles_per_tick.  For any TSC rate between
     * ~10 MHz and ~40 GHz this stays inside 32 bits. */
    uint64_t num = (uint64_t)TICK_NS << TSC_MULT_SHIFT;
    g_cycles_per_tick = cycles_per_tick;
    g_ns_mult = udiv64_32(num, cycles_per_tick);
    printk("[TSC] %u cycles per 10 ms tick (%u MHz, %s)\n",
           (unsigned)cycles_per_tick, (unsigned)(cycles_per_tick / 10000U), how);
}

/*
 * Adopt a refined rate, but only if it is credible.
 *
 * The passive path measures cycles-per-tick as (TSC span) / (ticks delivered),
 * which is only the rate if the ticks delivered account for the wall time the
 * span covers.  Early in boot they do not: the guest runs long stretches with
 * interrupts disabled (a PIO disk transfer holds IF=0 for the whole transfer),
 * the PIC collapses every tick missed during one such stretch into a single
 * pending interrupt, and QEMU repays the backlog afterwards at faster than
 * 100 Hz.  Measured on this host, the first 100 ticks covered 2.17 SECONDS of
 * wall time -- one interval alone exceeded 2^32 cycles, a single interrupt
 * standing in for about 113 ticks -- so the one-second refinement reported
 * 8.3 GHz on a 3.8 GHz machine, and the ten-second one was right only because
 * the catch-up had repaid the deficit by then.
 *
 * A real TSC rate does not change: it is invariant on any CPU this kernel will
 * meet, and under a hypervisor the guest's TSC follows the host's.  So the only
 * legitimate movement is the bias in the estimate being replaced, and a
 * refinement beyond that is evidence the window did not measure the wall time
 * it claims -- not that the machine changed speed.
 *
 * The band is +-50 %, chosen from both requirements rather than picked round.
 * It must REJECT the coalescing error, measured at 2.26x.  It must ADMIT the
 * real correction, which is bounded by how wrong the initial estimate can be:
 * that estimate is a MINIMUM over consecutive tick intervals, so it can only be
 * biased low, by a short catch-up interval, never high, and the worst bias
 * observed over these boots is 1.24x (3075 MHz against a true 3818).  Any bound
 * between 1.24x and 2.26x works; 1.5x leaves 21 % of headroom above the largest
 * correction that has to get through and rejects the error it has to stop by a
 * margin of 51 %.  +-25 % was tried first and left less than one per cent of
 * headroom on the admit side -- it would have worked on these boots by luck.
 */
static void tsc_refine(uint32_t avg, const char *how) {
    if (avg < 1000) return;
    uint32_t cur = g_cycles_per_tick;
    if (avg > cur + cur / 2U || avg < cur / 2U) {
        printk("[TSC] %s rejected: %u MHz against %u MHz (outside +-50%%)\n",
               how, (unsigned)(avg / 10000U), (unsigned)(cur / 10000U));
        return;
    }
    tsc_set_rate(avg, how);
}

/* 64 / 32 -> 32 unsigned division by shift-subtract (calibration only). */
static uint32_t udiv64_32(uint64_t n, uint32_t d) {
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        q <<= 1;
        if (r >= d) { r -= d; q |= 1; }
    }
    return (uint32_t)q;
}

void tsc_tick_sample(void) {
    uint64_t now = rdtsc64();

    if (g_hw_calibrated) {
        /* The rate came from a busy-wait against PIT channel 2 and is not
         * subject to anything the tick interrupt does.  Nothing to measure. */
    } else if (!g_calibrated) {
        if (g_prev_tsc) {
            uint64_t d = now - g_prev_tsc;
            /* Reject absurd samples (first tick after a long IF=0 stretch). */
            if (d && d < 0xFFFFFFFFULL) {
                uint32_t d32 = (uint32_t)d;
                if (!g_cycles_per_tick || d32 < g_cycles_per_tick)
                    g_cycles_per_tick = d32;
                if (!g_samples) g_cal_tsc0 = g_prev_tsc;
                g_samples++;
            }
        }
        g_prev_tsc = now;
        if (g_samples >= TSC_CAL_SAMPLES && g_cycles_per_tick >= 1000) {
            tsc_set_rate(g_cycles_per_tick, "initial");
            g_cal_ticks = g_samples;
            g_calibrated = 1;
        }
    } else if (g_cal_ticks < 1000) {
        /* Refine with a long-window average.  QEMU delivers late PIT ticks in
         * catch-up bursts, so single intervals scatter both ways; over 1 s and
         * 10 s the tick count is faithful to wall time and the average is the
         * true rate.  Applied only here, at a tick boundary under the seqcount,
         * so a reader never sees the sub-tick term shrink within one tick. */
        g_cal_ticks++;
        if (g_cal_ticks == 100 || g_cal_ticks == 1000) {
            uint64_t span = now - g_cal_tsc0;
            uint32_t avg = udiv64_32(span, g_cal_ticks);
            tsc_refine(avg, g_cal_ticks == 100 ? "1 s average" : "10 s average");
        }
    }

    /* Publish the new (tsc, tick) pair.  Writer side of the seqcount: odd
     * while updating.  pit_handler increments the tick right after we return,
     * so the snapshot is tick+1. */
    g_seq++;
    __asm__ volatile("" ::: "memory");
    g_tsc_at_tick = now;
    g_tick_snap   = pit_ticks() + 1;
    __asm__ volatile("" ::: "memory");
    g_seq++;
}

/*
 * Calibrate the TSC against PIT channel 2, the way a PC has always done it
 * (Linux: pit_calibrate_tsc).
 *
 * Channel 2 is the one channel whose gate is software-controlled and whose
 * output is readable, both through port 0x61, so the whole measurement is a
 * busy-wait with no interrupt anywhere in it.  That is the entire point: the
 * tick interrupt can be coalesced and its count is therefore not a measure of
 * wall time, while a counter the CPU polls directly cannot lose anything.
 *
 * Returns cycles per TICK_NS, or 0 if the channel does not behave (no hang: the
 * poll is bounded, and the caller falls back to the passive path).
 * Must be called with interrupts disabled, before the tick starts.
 */
#define PIT_FREQ_HZ 1193182U

static uint32_t tsc_calibrate_ch2(void) {
    /* One tick's worth of PIT counts: 11932 / 1193182 Hz = 10.0001 ms, 15 ppm
     * long, which is far below the accuracy anything here needs. */
    const uint32_t latch = PIT_FREQ_HZ / TICK_HZ;
    uint8_t saved = inb(0x61);

    /* Gate channel 2 on (bit 0), speaker data off (bit 1) so nothing sounds. */
    outb(0x61, (uint8_t)((saved & ~0x02U) | 0x01U));
    outb(0x43, 0xB0);                       /* ch2, lo/hi, mode 0, binary */
    outb(0x42, (uint8_t)(latch & 0xFF));
    outb(0x42, (uint8_t)(latch >> 8));

    uint64_t t0 = rdtsc64();
    /* Mode 0 drives OUT high when the count reaches zero; 0x61 bit 5 is it. */
    uint32_t guard = 0;
    while (!(inb(0x61) & 0x20)) {
        if (++guard > 20000000U) {          /* channel dead: give up, no hang */
            outb(0x61, saved);
            return 0;
        }
    }
    uint64_t d = rdtsc64() - t0;
    outb(0x61, saved);

    /* Anything outside 10 MHz .. 100 GHz is not a TSC rate we can use. */
    if (d < (uint64_t)TICK_HZ * 100000ULL || d > 1000000000ULL) return 0;
    return (uint32_t)d;
}

void tsc_init(void) {
    uint32_t cpt = tsc_calibrate_ch2();
    if (!cpt) {
        printk("[TSC] PIT channel 2 unusable; falling back to tick intervals\n");
        return;
    }
    tsc_set_rate(cpt, "PIT ch2");
    g_hw_calibrated = 1;
    g_calibrated = 1;
}

int clock_tsc_calibrated(void) { return g_calibrated; }

/* Calibrated TSC rate, for kprof's cycles->ms conversion.  0 until calibrated. */
uint32_t tsc_cycles_per_tick(void) { return g_calibrated ? g_cycles_per_tick : 0; }

/* Snapshot (tick, tsc_at_tick, sub-tick ns) consistently. */
static void clock_snapshot(uint32_t *tick, uint32_t *sub_ns) {
    uint32_t s, t;
    uint64_t base;
    for (;;) {
        s = g_seq;
        if (s & 1) { __asm__ volatile("pause"); continue; }
        __asm__ volatile("" ::: "memory");
        t    = g_tick_snap;
        base = g_tsc_at_tick;
        __asm__ volatile("" ::: "memory");
        if (s == g_seq) break;
    }
    /* Before the first sample the tick counter is the only clock. */
    if (!base) { *tick = pit_ticks(); *sub_ns = 0; return; }
    /* If ticks were published since the snapshot (should not happen: the
     * sample precedes the increment), fall back to the tick counter. */
    uint32_t cur = pit_ticks();
    if ((int32_t)(cur - t) > 0) { *tick = cur; *sub_ns = 0; return; }

    /* First readers can arrive before TSC_CAL_SAMPLES ticks have elapsed
     * (~40 ms after the first tick).  Wait for the calibration, but only
     * while interrupts can deliver ticks, and never longer than ~0.5e9 TSC
     * cycles (0.1-0.5 s at any plausible rate) in case the PIT is dead. */
    if (!g_calibrated) {
        uint32_t fl;
        __asm__ volatile("pushf; pop %0" : "=r"(fl));
        if (fl & 0x200) {
            uint64_t t0 = rdtsc64();
            while (!g_calibrated && rdtsc64() - t0 < 500000000ULL)
                __asm__ volatile("pause");
            if (g_calibrated) {          /* re-snapshot: ticks moved meanwhile */
                for (;;) {
                    s = g_seq;
                    if (s & 1) { __asm__ volatile("pause"); continue; }
                    __asm__ volatile("" ::: "memory");
                    t    = g_tick_snap;
                    base = g_tsc_at_tick;
                    __asm__ volatile("" ::: "memory");
                    if (s == g_seq) break;
                }
                cur = pit_ticks();
                if ((int32_t)(cur - t) > 0) { *tick = cur; *sub_ns = 0; return; }
            }
        }
    }

    uint32_t ns = 0;
    if (g_calibrated) {
        uint64_t d = rdtsc64() - base;
        /* Clamp the cycle delta so the multiply cannot overflow, then clamp
         * the result below one tick: the interpolation may never claim the
         * next tick has already happened. */
        if (d > (uint64_t)g_cycles_per_tick * 2U) d = (uint64_t)g_cycles_per_tick * 2U;
        ns = (uint32_t)(((uint64_t)(uint32_t)d * g_ns_mult) >> TSC_MULT_SHIFT);
        if (ns >= TICK_NS) ns = TICK_NS - 1;
    }
    *tick = t;
    *sub_ns = ns;
}

void clock_mono(uint32_t *sec, uint32_t *nsec) {
    uint32_t tick, sub;
    clock_snapshot(&tick, &sub);
    *sec  = tick / TICK_HZ;
    *nsec = (tick % TICK_HZ) * TICK_NS + sub;   /* < 1e9 since sub < TICK_NS */
}

uint64_t clock_mono_ns(void) {
    uint32_t s, ns;
    clock_mono(&s, &ns);
    return (uint64_t)s * 1000000000ULL + ns;
}

uint32_t clock_mono_to_tick(uint32_t sec, uint32_t nsec) {
    /* ceil((sec*1e9 + nsec) / TICK_NS) without 64-bit division. */
    uint64_t t = (uint64_t)sec * TICK_HZ + (nsec + TICK_NS - 1) / TICK_NS;
    if (t > 0xFFFFFFFFULL) return 0xFFFFFFFFU;
    return (uint32_t)t;
}
