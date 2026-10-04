#include "alsa.h"
#include "ac97.h"
#include "alsa_resample.h"
#include "hda.h"
#include "../arch/i686/cpu/pit.h"
#include "../arch/i686/cpu/spinlock.h"
#include "../arch/i686/cpu/tsc.h"
#include "../kernel/printk.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../proc/process.h"
#include "../proc/scheduler.h"
#include "../proc/signal.h"
#include "../proc/syscall.h"
#include <stddef.h>
#include <stdint.h>

/*
 * The ALSA kernel ABI, as much of it as alsa-lib's "hw" plugin needs for
 * playback: one card (/dev/snd/controlC0) with one PCM playback device
 * (/dev/snd/pcmC0D0p) in RW_INTERLEAVED access, on the HDA or AC'97 driver.
 *
 * Structure layouts are the i386 ones of the ALSA UAPI (sound/asound.h,
 * PCM protocol 2.0.15, control 2.0.9), written out here from the published
 * ABI: the ioctl numbers carry each structure's size, and alsa-lib 1.2.14
 * (Alpine, musl, 64-bit time_t) uses the time64 STATUS/SYNC_PTR layouts
 * while a 32-bit time_t userland (Debian i386 glibc) uses the older ones,
 * so both are accepted, told apart by that size.
 *
 * Data path.  The hardware runs one fixed stream, 48 kHz S16LE stereo (the
 * stream /dev/dsp carries).  WRITEI frames in any of the advertised formats
 * (S8, U8, S16_LE, S24_LE, S32_LE, FLOAT_LE; 1 or 2 channels; 8-192 kHz) are
 * converted here: decoded to 16-bit stereo (float through its bit pattern:
 * no FPU in the kernel) and resampled to 48 kHz by linear interpolation.
 * Before the stream starts, converted PCM collects in a staging buffer;
 * from the start on it goes straight to the driver's ring.
 *
 * Pointers.  appl_ptr counts frames the application wrote.  hw_ptr is
 * derived from what the driver still has queued (ring + DMA ahead of the
 * engine), converted back to application frames, so appl_ptr - hw_ptr is
 * the real delay and avail = buffer_size - delay.  The driver's ring holds
 * 1.36 s, so the buffer is capped at 1 s and a write that fits in avail
 * never blocks in the driver.  An underrun (avail >= stop_threshold while
 * running) is an XRUN as on Linux.  The pointers are frame counts since
 * PREPARE reported modulo the boundary (2^32 frames is 24 h at 48 kHz).
 *
 * Not provided: mmap of the status/control pages (mmap fails, and alsa-lib
 * falls back to SYNC_PTR, as it does on any kernel without it) or of the
 * data buffer (no MMAP access), capture, pause, linked streams, mixer
 * controls.  A second open of the PCM fails (ENOENT here, EBUSY on Linux).
 */

#define EINTR_    4
#define ENXIO_    6
#define EAGAIN_  11
#define EFAULT_  14
#define ENOENT_   2
#define EPERM_    1
#define ENOMEM_  12
#define EINVAL_  22
#define ENOTTY_  25
#define EPIPE_   32
#define ENOSYS_  38
#define EBADFD_  77

#define PROTO(a, b, c)  (((a) << 16) | ((b) << 8) | (c))
#define PCM_VERSION     PROTO(2, 0, 15)
#define CTL_VERSION     PROTO(2, 0, 9)

#define ALSA_TRACE      0   /* 1: log every ioctl */
#define OUT_RATE        48000U
#define OUT_FRAME_BYTES 4U

/* PCM states */
enum { ST_OPEN, ST_SETUP, ST_PREPARED, ST_RUNNING, ST_XRUN, ST_DRAINING,
       ST_PAUSED, ST_SUSPENDED, ST_DISCONNECTED };

/* hw_params parameter indices: three masks, then intervals from 8 */
enum { P_ACCESS = 0, P_FORMAT = 1, P_SUBFORMAT = 2,
       P_SAMPLE_BITS = 8, P_FRAME_BITS, P_CHANNELS, P_RATE, P_PERIOD_TIME,
       P_PERIOD_SIZE, P_PERIOD_BYTES, P_PERIODS, P_BUFFER_TIME,
       P_BUFFER_SIZE, P_BUFFER_BYTES, P_TICK_TIME };
#define P_FIRST_IV  P_SAMPLE_BITS
#define P_LAST_IV   P_TICK_TIME

#define ACCESS_RW_INTERLEAVED 3

#define INFO_INTERLEAVED     0x00000100U
#define INFO_BLOCK_TRANSFER  0x00010000U
#define INFO_BATCH           0x00000010U
#define HW_FLAG_NORESAMPLE   0x1U

#define SYNC_HWSYNC     0x1U
#define SYNC_APPL       0x2U
#define SYNC_AVAIL_MIN  0x4U

struct snd_mask { uint32_t bits[8]; };
struct snd_interval {
    uint32_t min, max;
    uint32_t openmin:1, openmax:1, integer:1, empty:1;
};
struct hw_params {
    uint32_t flags;
    struct snd_mask masks[3];
    struct snd_mask mres[5];
    struct snd_interval intervals[12];
    struct snd_interval ires[9];
    uint32_t rmask, cmask, info, msbits, rate_num, rate_den;
    uint32_t fifo_size;
    uint8_t reserved[64];
};
_Static_assert(sizeof(struct hw_params) == 604, "i386 snd_pcm_hw_params");

struct sw_params {
    int32_t tstamp_mode;
    uint32_t period_step, sleep_min;
    uint32_t avail_min, xfer_align, start_threshold, stop_threshold;
    uint32_t silence_threshold, silence_size, boundary;
    uint32_t proto, tstamp_type;
    uint8_t reserved[56];
};
_Static_assert(sizeof(struct sw_params) == 104, "i386 snd_pcm_sw_params");

struct xferi { int32_t result; uint32_t buf; uint32_t frames; };

/* Sample formats offered: ALSA format number, physical and significant bits */
static const struct { uint8_t fmt, phys, sig; } formats[] = {
    {  0,  8,  8 },     /* S8 */
    {  1,  8,  8 },     /* U8 */
    {  2, 16, 16 },     /* S16_LE */
    {  6, 32, 24 },     /* S24_LE (low 3 bytes of 4) */
    { 10, 32, 32 },     /* S32_LE */
    { 14, 32, 32 },     /* FLOAT_LE */
};
#define NFORMATS (sizeof(formats) / sizeof(formats[0]))

#define RATE_MIN     8000U
#define RATE_MAX     192000U
#define BUF_TIME_MAX 1000000U      /* us; see "Pointers" above */

/* ── state ─────────────────────────────────────────────────────────────── */

static vfs_node_t dir_node, ctl_node, pcm_node;
static int card_present;
static const char *card_hw;      /* "HDA" or "AC97" */

static spinlock_t lk;            /* pcm fields below; short sections only */
#define LOCK()   do { preempt_disable(); spin_lock(&lk); } while (0)
#define UNLOCK() do { spin_unlock(&lk); preempt_enable(); } while (0)

static int pcm_busy;             /* sleeping mutex for stream-changing ioctls */
static int ctl_subscribed;

static struct {
    int opens;
    int state;
    uint32_t format, channels, rate, period_size, periods, buffer_size;
    uint32_t frame_bytes, fmt_phys, fmt_sig;
    uint32_t avail_min, start_threshold, stop_threshold, boundary;
    uint32_t appl, hw;            /* frames since PREPARE */
    uint32_t trig_sec, trig_nsec;
    int pushing;                  /* PCM on its way to the driver: hw frozen */
    uint8_t *stage;               /* converted PCM before the start */
    uint32_t stage_len, stage_cap;
    struct alsa_rs rs;            /* to 48 kHz (drivers/alsa_resample.h) */
} pcm;

/* ── output device ─────────────────────────────────────────────────────── */

static int out_write(const uint8_t *p, uint32_t len) {
    return ac97_present() ? ac97_write(p, len) : hda_write(p, len);
}

static uint32_t out_queued(void) {
    return ac97_present() ? ac97_queued() : hda_queued();
}

static void out_drop(void) {
    if (ac97_present()) ac97_drop(); else hda_drop();
}

/* ── small helpers ─────────────────────────────────────────────────────── */

static void put32(uint8_t *b, uint32_t off, uint32_t v) {
    memcpy(b + off, &v, 4);
}

static uint32_t get32(const uint8_t *b, uint32_t off) {
    uint32_t v;
    memcpy(&v, b + off, 4);
    return v;
}

static void put_ts64(uint8_t *b, uint32_t off, uint32_t sec, uint32_t nsec) {
    put32(b, off, sec);      put32(b, off + 4, 0);
    put32(b, off + 8, nsec); put32(b, off + 12, 0);
}

static void put_ts32(uint8_t *b, uint32_t off, uint32_t sec, uint32_t nsec) {
    put32(b, off, sec);
    put32(b, off + 4, nsec);
}

static void put_str(uint8_t *b, uint32_t off, uint32_t len, const char *s) {
    uint32_t n = strlen(s);
    if (n >= len) n = len - 1;
    memcpy(b + off, s, n);
}

static int sleep_ticks(uint32_t ticks) {
    if (signal_interrupt_pending(current_proc)) return -EINTR_;
    current_proc->wake_tick = pit_ticks() + ticks;
    sleep_on(&io_activity);
    return 0;
}

/* intr: a pending signal gives up with -EINTR.  The last close waits
 * regardless (intr 0): the holder may be a sibling thread blocked in START,
 * WRITEI or DRAIN on pcm.stage, which pcm_release frees. */
static int pcm_enter_how(int intr) {
    for (;;) {
        int got;
        LOCK();
        got = !pcm_busy;
        if (got) pcm_busy = 1;
        UNLOCK();
        if (got) return 0;
        if (intr && signal_interrupt_pending(current_proc)) return -EINTR_;
        current_proc->wake_tick = pit_ticks() + 1;
        sleep_on(&pcm_busy);
    }
}

static int pcm_enter(void) {
    return pcm_enter_how(1);
}

static void pcm_leave(void) {
    pcm_busy = 0;
    wake_up(&pcm_busy);
}

/* ── interval arithmetic (hw_params refinement) ───────────────────────── */

typedef struct snd_interval iv_t;

static int iv_set_empty(iv_t *i) {
    i->empty = 1;
    return -EINVAL_;
}

static int iv_in(const iv_t *i, uint32_t v) {
    if (i->empty) return 0;
    if (v < i->min || (v == i->min && i->openmin)) return 0;
    if (v > i->max || (v == i->max && i->openmax)) return 0;
    return 1;
}

static int iv_single(const iv_t *i) {
    return i->min == i->max ||
           (i->min + 1 == i->max && (i->openmin || i->openmax));
}

static uint32_t iv_value(const iv_t *i) {
    return (i->openmin && !i->openmax) ? i->max : i->min;
}

/* Intersect i with v: 1 if i changed, 0 if not, -EINVAL if i is empty. */
static int iv_refine(iv_t *i, const iv_t *v) {
    int changed = 0;

    if (i->empty || v->empty) return iv_set_empty(i);
    if (i->min < v->min) {
        i->min = v->min; i->openmin = v->openmin; changed = 1;
    } else if (i->min == v->min && !i->openmin && v->openmin) {
        i->openmin = 1; changed = 1;
    }
    if (i->max > v->max) {
        i->max = v->max; i->openmax = v->openmax; changed = 1;
    } else if (i->max == v->max && !i->openmax && v->openmax) {
        i->openmax = 1; changed = 1;
    }
    if (!i->integer && v->integer) {
        i->integer = 1; changed = 1;
    }
    if (i->integer) {
        if (i->openmin) {
            if (i->min == 0xFFFFFFFFU) return iv_set_empty(i);
            i->min++; i->openmin = 0;
        }
        if (i->openmax) {
            if (i->max == 0) return iv_set_empty(i);
            i->max--; i->openmax = 0;
        }
    } else if (!i->openmin && !i->openmax && i->min == i->max) {
        i->integer = 1;
    }
    if (i->min > i->max || (i->min == i->max && (i->openmin || i->openmax)))
        return iv_set_empty(i);
    return changed;
}

static uint32_t mul_sat(uint32_t a, uint32_t b) {
    uint64_t r = (uint64_t)a * b;
    return r > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)r;
}

/* a * b / c, saturating; *rem = 0 when saturated or c == 0 */
static uint32_t muldiv(uint32_t a, uint32_t b, uint32_t c, uint32_t *rem) {
    uint64_t n = (uint64_t)a * b, q;

    *rem = 0;
    if (c == 0) return 0xFFFFFFFFU;
    q = n / c;
    if (q > 0xFFFFFFFFULL) return 0xFFFFFFFFU;
    *rem = (uint32_t)(n - q * c);
    return (uint32_t)q;
}

static void max_round_up(iv_t *c, uint32_t rem, int open) {
    if (rem) {
        if (c->max != 0xFFFFFFFFU) c->max++;
        c->openmax = 1;
    } else {
        c->openmax = open;
    }
}

static void iv_mul(const iv_t *a, const iv_t *b, iv_t *c) {
    c->min = mul_sat(a->min, b->min);
    c->openmin = a->openmin || b->openmin;
    c->max = mul_sat(a->max, b->max);
    c->openmax = a->openmax || b->openmax;
    c->integer = a->integer && b->integer;
}

static void iv_div(const iv_t *a, const iv_t *b, iv_t *c) {
    uint32_t r;
    c->min = muldiv(a->min, 1, b->max, &r);
    c->openmin = r || a->openmin || b->openmax;
    if (b->min > 0) {
        c->max = muldiv(a->max, 1, b->min, &r);
        max_round_up(c, r, a->openmax || b->openmin);
    } else {
        c->max = 0xFFFFFFFFU; c->openmax = 0;
    }
    c->integer = 0;
}

/* c = a * k / b */
static void iv_mulkdiv(const iv_t *a, uint32_t k, const iv_t *b, iv_t *c) {
    uint32_t r;
    c->min = muldiv(a->min, k, b->max, &r);
    c->openmin = r || a->openmin || b->openmax;
    if (b->min > 0) {
        c->max = muldiv(a->max, k, b->min, &r);
        max_round_up(c, r, a->openmax || b->openmin);
    } else {
        c->max = 0xFFFFFFFFU; c->openmax = 0;
    }
    c->integer = 0;
}

/* c = a * b / k */
static void iv_muldivk(const iv_t *a, const iv_t *b, uint32_t k, iv_t *c) {
    uint32_t r;
    c->min = muldiv(a->min, b->min, k, &r);
    c->openmin = r || a->openmin || b->openmin;
    c->max = muldiv(a->max, b->max, k, &r);
    max_round_up(c, r, a->openmax || b->openmax);
    c->integer = 0;
}

/* The relations between the parameters (bits = bytes * 8, time in us). */
enum { R_MUL, R_DIV, R_MULKDIV, R_MULDIVK };
static const struct { uint8_t var, op, a, b; uint32_t k; } rules[] = {
    { P_SAMPLE_BITS, R_DIV,     P_FRAME_BITS,   P_CHANNELS,    0 },
    { P_FRAME_BITS,  R_MUL,     P_SAMPLE_BITS,  P_CHANNELS,    0 },
    { P_FRAME_BITS,  R_MULKDIV, P_PERIOD_BYTES, P_PERIOD_SIZE, 8 },
    { P_FRAME_BITS,  R_MULKDIV, P_BUFFER_BYTES, P_BUFFER_SIZE, 8 },
    { P_CHANNELS,    R_DIV,     P_FRAME_BITS,   P_SAMPLE_BITS, 0 },
    { P_RATE,        R_MULKDIV, P_PERIOD_SIZE,  P_PERIOD_TIME, 1000000 },
    { P_RATE,        R_MULKDIV, P_BUFFER_SIZE,  P_BUFFER_TIME, 1000000 },
    { P_PERIODS,     R_DIV,     P_BUFFER_SIZE,  P_PERIOD_SIZE, 0 },
    { P_PERIOD_SIZE, R_DIV,     P_BUFFER_SIZE,  P_PERIODS,     0 },
    { P_PERIOD_SIZE, R_MULKDIV, P_PERIOD_BYTES, P_FRAME_BITS,  8 },
    { P_PERIOD_SIZE, R_MULDIVK, P_PERIOD_TIME,  P_RATE,        1000000 },
    { P_BUFFER_SIZE, R_MUL,     P_PERIOD_SIZE,  P_PERIODS,     0 },
    { P_BUFFER_SIZE, R_MULKDIV, P_BUFFER_BYTES, P_FRAME_BITS,  8 },
    { P_BUFFER_SIZE, R_MULDIVK, P_BUFFER_TIME,  P_RATE,        1000000 },
    { P_PERIOD_BYTES, R_MULDIVK, P_PERIOD_SIZE, P_FRAME_BITS,  8 },
    { P_BUFFER_BYTES, R_MULDIVK, P_BUFFER_SIZE, P_FRAME_BITS,  8 },
    { P_PERIOD_TIME, R_MULKDIV, P_PERIOD_SIZE,  P_RATE,        1000000 },
    { P_BUFFER_TIME, R_MULKDIV, P_BUFFER_SIZE,  P_RATE,        1000000 },
};

#define IV(p, x) (&(p)->intervals[(x) - P_FIRST_IV])

static int mask_test(const struct snd_mask *m, uint32_t bit) {
    return (m->bits[bit >> 5] >> (bit & 31)) & 1;
}

static int mask_empty(const struct snd_mask *m) {
    for (int i = 0; i < 8; i++)
        if (m->bits[i]) return 0;
    return 1;
}

/* m &= allowed; 1 if changed, -EINVAL if empty */
static int mask_refine(struct snd_mask *m, const struct snd_mask *allowed) {
    int changed = 0;
    for (int i = 0; i < 8; i++) {
        uint32_t v = m->bits[i] & allowed->bits[i];
        if (v != m->bits[i]) changed = 1;
        m->bits[i] = v;
    }
    return mask_empty(m) ? -EINVAL_ : changed;
}

static int mask_first(const struct snd_mask *m) {
    for (int i = 0; i < 256; i++)
        if (mask_test(m, (uint32_t)i)) return i;
    return -1;
}

static void mask_only(struct snd_mask *m, uint32_t bit) {
    memset(m, 0, sizeof(*m));
    m->bits[bit >> 5] = 1U << (bit & 31);
}

static int fmt_index(uint32_t fmt) {
    for (uint32_t i = 0; i < NFORMATS; i++)
        if (formats[i].fmt == fmt) return (int)i;
    return -1;
}

/* FORMAT <-> SAMPLE_BITS (the physical width) */
static int refine_format(struct hw_params *p, uint32_t *cmask) {
    struct snd_mask *fm = &p->masks[P_FORMAT];
    iv_t sb = { 0xFFFFFFFFU, 0, 0, 0, 1, 0 };
    int r, changed = 0;

    for (uint32_t i = 0; i < NFORMATS; i++) {
        if (!mask_test(fm, formats[i].fmt)) continue;
        if (!iv_in(IV(p, P_SAMPLE_BITS), formats[i].phys)) {
            fm->bits[formats[i].fmt >> 5] &= ~(1U << (formats[i].fmt & 31));
            *cmask |= 1U << P_FORMAT;
            changed = 1;
            continue;
        }
        if (formats[i].phys < sb.min) sb.min = formats[i].phys;
        if (formats[i].phys > sb.max) sb.max = formats[i].phys;
    }
    if (mask_empty(fm)) return -EINVAL_;
    r = iv_refine(IV(p, P_SAMPLE_BITS), &sb);
    if (r < 0) return r;
    if (r) { *cmask |= 1U << P_SAMPLE_BITS; changed = 1; }
    return changed;
}

/* Apply the device's limits and the relations until nothing changes. */
static int hw_refine(struct hw_params *p) {
    struct snd_mask allow;
    uint32_t cmask = 0;
    int r;

    /* The answers below are recomputed on every refine (alsa-lib's "any"
     * leaves info at ~0, which would claim pause, resume and mmap). */
    p->info = 0;
    p->fifo_size = 0;
    if (p->rmask & (1U << P_SAMPLE_BITS))
        p->msbits = 0;
    if (p->rmask & (1U << P_RATE))
        p->rate_num = p->rate_den = 0;

    memset(&allow, 0, sizeof(allow));
    allow.bits[0] = 1U << ACCESS_RW_INTERLEAVED;
    r = mask_refine(&p->masks[P_ACCESS], &allow);
    if (r < 0) return r;
    if (r) cmask |= 1U << P_ACCESS;

    memset(&allow, 0, sizeof(allow));
    for (uint32_t i = 0; i < NFORMATS; i++)
        allow.bits[formats[i].fmt >> 5] |= 1U << (formats[i].fmt & 31);
    r = mask_refine(&p->masks[P_FORMAT], &allow);
    if (r < 0) return r;
    if (r) cmask |= 1U << P_FORMAT;

    memset(&allow, 0, sizeof(allow));
    allow.bits[0] = 1;                                  /* SUBFORMAT_STD */
    r = mask_refine(&p->masks[P_SUBFORMAT], &allow);
    if (r < 0) return r;
    if (r) cmask |= 1U << P_SUBFORMAT;

    {
        static const struct { uint8_t var; uint32_t min, max; uint8_t integer; } lim[] = {
            { P_SAMPLE_BITS, 8, 32, 1 },
            { P_FRAME_BITS,  8, 64, 1 },
            { P_CHANNELS,    1, 2, 1 },
            { P_RATE,        RATE_MIN, RATE_MAX, 1 },
            { P_PERIOD_SIZE, 16, 1U << 20, 1 },
            { P_PERIOD_BYTES, 64, 1U << 23, 1 },
            { P_PERIODS,     2, 1024, 1 },
            { P_BUFFER_TIME, 0, BUF_TIME_MAX, 0 },
            { P_BUFFER_SIZE, 32, 1U << 20, 1 },
            { P_BUFFER_BYTES, 128, 1U << 23, 1 },
        };
        for (uint32_t i = 0; i < sizeof(lim) / sizeof(lim[0]); i++) {
            iv_t v = { lim[i].min, lim[i].max, 0, 0, lim[i].integer, 0 };
            if (lim[i].var == P_RATE && (p->flags & HW_FLAG_NORESAMPLE))
                v.min = v.max = OUT_RATE;
            r = iv_refine(IV(p, lim[i].var), &v);
            if (r < 0) return r;
            if (r) cmask |= 1U << lim[i].var;
        }
    }

    for (int pass = 0; pass < 64; pass++) {
        int changed;

        r = refine_format(p, &cmask);
        if (r < 0) return r;
        changed = r;
        for (uint32_t i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
            iv_t c = { 0, 0, 0, 0, 0, 0 };
            const iv_t *a = IV(p, rules[i].a), *b = IV(p, rules[i].b);
            if (a->empty || b->empty) return -EINVAL_;
            switch (rules[i].op) {
            case R_MUL:     iv_mul(a, b, &c); break;
            case R_DIV:     iv_div(a, b, &c); break;
            case R_MULKDIV: iv_mulkdiv(a, rules[i].k, b, &c); break;
            default:        iv_muldivk(a, b, rules[i].k, &c); break;
            }
            r = iv_refine(IV(p, rules[i].var), &c);
            if (r < 0) return r;
            if (r) { cmask |= 1U << rules[i].var; changed = 1; }
        }
        if (!changed) break;
    }

    p->cmask = cmask;
    p->rmask = 0;
    p->info = INFO_INTERLEAVED | INFO_BLOCK_TRANSFER | INFO_BATCH;
    {
        int f = mask_first(&p->masks[P_FORMAT]);
        uint32_t only = 1;
        for (uint32_t i = 0; i < NFORMATS; i++)
            if ((int)formats[i].fmt != f && mask_test(&p->masks[P_FORMAT], formats[i].fmt))
                only = 0;
        if (!p->msbits && only && f >= 0)
            p->msbits = formats[fmt_index((uint32_t)f)].sig;
    }
    if (!p->rate_den && iv_single(IV(p, P_RATE))) {
        p->rate_num = iv_value(IV(p, P_RATE));
        p->rate_den = 1;
    }
    return 0;
}

/* Fix every parameter still open (alsa-lib normally already has). */
static int hw_choose(struct hw_params *p) {
    static const struct { uint8_t var, last; } order[] = {
        { P_CHANNELS, 0 }, { P_RATE, 0 }, { P_PERIOD_TIME, 0 },
        { P_PERIOD_SIZE, 0 }, { P_PERIOD_BYTES, 0 }, { P_PERIODS, 0 },
        { P_BUFFER_TIME, 1 }, { P_BUFFER_SIZE, 1 }, { P_BUFFER_BYTES, 1 },
        { P_SAMPLE_BITS, 0 }, { P_FRAME_BITS, 0 },
    };
    int r;

    for (int m = 0; m < 3; m++) {
        int f = mask_first(&p->masks[m]);
        if (f < 0) return -EINVAL_;
        mask_only(&p->masks[m], (uint32_t)f);
    }
    r = hw_refine(p);
    if (r < 0) return r;
    for (uint32_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        iv_t *v = IV(p, order[i].var);
        if (iv_single(v)) continue;
        if (order[i].last) {
            v->min = v->max - (v->openmax ? 1 : 0);
            v->openmin = 0;
        } else {
            v->max = v->min + (v->openmin ? 1 : 0);
            v->openmax = 0;
        }
        r = hw_refine(p);
        if (r < 0) return r;
    }
    return 0;
}

/* ── conversion ────────────────────────────────────────────────────────── */

static int32_t f32_to_s16(uint32_t b) {
    uint32_t e = (b >> 23) & 0xFF;
    uint32_t m = (b & 0x7FFFFF) | 0x800000;
    int32_t v;

    if (e >= 127) v = 32767;                  /* |x| >= 1, inf, NaN */
    else if (e < 104) v = 0;
    else {
        v = (int32_t)(m >> (135 - e));        /* m * 2^(e-150) * 2^15 */
        if (v > 32767) v = 32767;
    }
    return (b >> 31) ? -v : v;
}

static int32_t sample_at(const uint8_t *s) {
    switch (pcm.format) {
    case 0:  return (int32_t)(int8_t)s[0] << 8;
    case 1:  return ((int32_t)s[0] - 128) << 8;
    case 2:  return (int16_t)(s[0] | (s[1] << 8));
    case 6: {
        int32_t v = (int32_t)(s[0] | (s[1] << 8) | (s[2] << 16));
        if (v & 0x800000) v -= 0x1000000;
        return v >> 8;
    }
    case 10: return (int32_t)get32(s, 0) >> 16;
    default: return f32_to_s16(get32(s, 0));
    }
}

#define IN_BYTES   4096U
#define OUT_FRAMES 4096U
static uint8_t in_buf[IN_BYTES];
static int16_t out_buf[OUT_FRAMES * 2];

/* `frames` frames from in_buf (NULL: silence) into out_buf at 48 kHz stereo;
 * returns the output frame count.  The caller keeps `frames` small enough
 * for OUT_FRAMES (see chunk_frames). */
static uint32_t convert(const uint8_t *in, uint32_t frames) {
    uint32_t n = 0, sb = pcm.fmt_phys / 8;

    for (uint32_t f = 0; f < frames; f++) {
        int32_t l = 0, r = 0;
        if (in) {
            const uint8_t *s = in + f * pcm.frame_bytes;
            l = sample_at(s);
            r = pcm.channels > 1 ? sample_at(s + sb) : l;
        }
        if (pcm.rate == OUT_RATE) {
            out_buf[2 * n] = (int16_t)l;
            out_buf[2 * n + 1] = (int16_t)r;
            n++;
            continue;
        }
        n += alsa_rs_frame(&pcm.rs, l, r, out_buf + 2 * n, OUT_FRAMES - n);
    }
    return n;
}

/* Input frames per chunk: fits in_buf, and its output fits out_buf. */
static uint32_t chunk_frames(void) {
    uint32_t n = IN_BYTES / pcm.frame_bytes;
    uint32_t lim = (uint32_t)(((uint64_t)(OUT_FRAMES - 2) * pcm.rate) / OUT_RATE);
    if (lim < n) n = lim;
    return n ? n : 1;
}

static void resampler_reset(void) {
    pcm.rs.have_prev = 0;
    pcm.rs.pos = 0;
    pcm.rs.step = (uint32_t)(((uint64_t)pcm.rate << 16) / OUT_RATE);
}

/* ── pointers ──────────────────────────────────────────────────────────── */

static uint32_t avail_now(void) {
    uint32_t used = pcm.appl - pcm.hw;
    return used >= pcm.buffer_size ? 0 : pcm.buffer_size - used;
}

static void now(uint32_t *sec, uint32_t *nsec) {
    clock_mono(sec, nsec);
}

/* Move hw_ptr up to what the device has played.  Called with lk held. */
static void update_hw(void) {
    uint32_t q, pend, used;

    if (pcm.state != ST_RUNNING && pcm.state != ST_DRAINING) return;
    if (pcm.pushing) return;
    q = out_queued() / OUT_FRAME_BYTES;
    pend = (uint32_t)(((uint64_t)q * pcm.rate + OUT_RATE - 1) / OUT_RATE);
    used = pcm.appl - pcm.hw;
    if (pend < used) pcm.hw = pcm.appl - pend;
    if (pcm.state == ST_DRAINING) {
        if (q == 0) {
            pcm.hw = pcm.appl;
            pcm.state = ST_SETUP;
            now(&pcm.trig_sec, &pcm.trig_nsec);
        }
    } else if (pcm.stop_threshold < pcm.boundary &&
               avail_now() >= pcm.stop_threshold) {
        pcm.state = ST_XRUN;
        now(&pcm.trig_sec, &pcm.trig_nsec);
    }
}

/* PREPARED -> RUNNING: hand the staged PCM to the device.  pcm_busy held. */
static void pcm_start(void) {
    LOCK();
    pcm.state = ST_RUNNING;
    pcm.pushing++;
    now(&pcm.trig_sec, &pcm.trig_nsec);
    UNLOCK();
    if (pcm.stage_len)
        out_write(pcm.stage, pcm.stage_len);
    LOCK();
    pcm.stage_len = 0;
    pcm.pushing--;
    UNLOCK();
}

static void pcm_reset_ptrs(void) {
    pcm.appl = pcm.hw = 0;
    pcm.stage_len = 0;
    resampler_reset();
}

static void pcm_release(void) {
    if (pcm.state == ST_RUNNING || pcm.state == ST_DRAINING ||
        pcm.state == ST_PREPARED || pcm.state == ST_XRUN)
        out_drop();
    if (pcm.stage) kfree(pcm.stage);
    pcm.stage = NULL;
    pcm.stage_cap = pcm.stage_len = 0;
    pcm.state = ST_OPEN;
}

/* ── PCM ioctls ────────────────────────────────────────────────────────── */

static void fill_pcm_info(uint8_t *b, int stream) {
    memset(b, 0, 288);
    put32(b, 0, 0);                       /* device */
    put32(b, 4, 0);                       /* subdevice */
    put32(b, 8, (uint32_t)stream);
    put32(b, 12, 0);                      /* card */
    put_str(b, 16, 64, "MaeroOS PCM");
    put_str(b, 80, 80, ac97_present() ? "MaeroOS AC97 PCM" : "MaeroOS HDA PCM");
    put_str(b, 160, 32, "subdevice #0");
    put32(b, 192, 0);                     /* SNDRV_PCM_CLASS_GENERIC */
    put32(b, 196, 0);
    put32(b, 200, 1);                     /* subdevices_count */
    put32(b, 204, pcm.opens ? 0 : 1);     /* subdevices_avail */
}

static int do_hw_params(struct hw_params *p) {
    int r, fi;
    uint32_t cap;

    if (pcm.state != ST_OPEN && pcm.state != ST_SETUP && pcm.state != ST_PREPARED)
        return -EBADFD_;
    r = hw_choose(p);
    if (r < 0) return r;
    fi = fmt_index((uint32_t)mask_first(&p->masks[P_FORMAT]));
    if (fi < 0 || mask_first(&p->masks[P_ACCESS]) != ACCESS_RW_INTERLEAVED)
        return -EINVAL_;

    if (pcm.state == ST_PREPARED) out_drop();
    pcm.format      = formats[fi].fmt;
    pcm.fmt_phys    = formats[fi].phys;
    pcm.fmt_sig     = formats[fi].sig;
    pcm.channels    = iv_value(IV(p, P_CHANNELS));
    pcm.rate        = iv_value(IV(p, P_RATE));
    pcm.period_size = iv_value(IV(p, P_PERIOD_SIZE));
    pcm.periods     = iv_value(IV(p, P_PERIODS));
    pcm.buffer_size = iv_value(IV(p, P_BUFFER_SIZE));
    pcm.frame_bytes = pcm.fmt_phys / 8 * pcm.channels;
    if (pcm.channels < 1 || pcm.channels > 2 || pcm.rate < RATE_MIN ||
        pcm.rate > RATE_MAX || !pcm.period_size || pcm.buffer_size < pcm.period_size)
        return -EINVAL_;

    cap = (uint32_t)(((uint64_t)pcm.buffer_size * OUT_RATE) / pcm.rate + 8) * OUT_FRAME_BYTES;
    if (cap > pcm.stage_cap) {
        if (pcm.stage) kfree(pcm.stage);
        pcm.stage = kmalloc(cap);
        pcm.stage_cap = pcm.stage ? cap : 0;
        if (!pcm.stage) { pcm.state = ST_OPEN; return -ENOMEM_; }
    }
    pcm.avail_min = pcm.period_size;
    pcm.start_threshold = 1;
    pcm.stop_threshold = pcm.buffer_size;
    pcm.boundary = pcm.buffer_size;
    while (pcm.boundary * 2 <= 0x7FFFFFFFU - pcm.buffer_size)
        pcm.boundary *= 2;
    pcm_reset_ptrs();
    pcm.state = ST_SETUP;
    printk("[ALSA] hw_params: format %u, %u ch, %u Hz, period %u, buffer %u\n",
           (unsigned)pcm.format, (unsigned)pcm.channels, (unsigned)pcm.rate,
           (unsigned)pcm.period_size, (unsigned)pcm.buffer_size);
    return 0;
}

static int do_sw_params(struct sw_params *s) {
    if (pcm.state == ST_OPEN) return -EBADFD_;
    if (s->avail_min == 0) return -EINVAL_;
    LOCK();
    if (s->boundary && s->boundary >= pcm.buffer_size)
        pcm.boundary = s->boundary;
    pcm.avail_min = s->avail_min;
    pcm.start_threshold = s->start_threshold;
    pcm.stop_threshold = s->stop_threshold;
    UNLOCK();
    return 0;
}

static uint32_t to_ring(uint32_t v) {
    return pcm.boundary ? v % pcm.boundary : v;
}

/* STATUS / STATUS_EXT: `size` picks the time64 (128) or time32 (108) layout. */
static int do_status(uint8_t *b, uint32_t size) {
    uint32_t sec, nsec, st, appl, hw, av, delay;

    LOCK();
    update_hw();
    st = (uint32_t)pcm.state;
    appl = pcm.appl; hw = pcm.hw;
    av = avail_now();
    delay = (st == ST_RUNNING || st == ST_DRAINING || st == ST_PREPARED) ? appl - hw : 0;
    UNLOCK();
    now(&sec, &nsec);
    if (size == 128) {
        uint32_t tsdata = get32(b, 68);
        memset(b, 0, 128);
        put32(b, 0, st);
        put_ts64(b, 8, pcm.trig_sec, pcm.trig_nsec);
        put_ts64(b, 24, sec, nsec);
        put32(b, 40, to_ring(appl)); put32(b, 44, to_ring(hw));
        put32(b, 48, delay); put32(b, 52, av); put32(b, 56, av);
        put32(b, 68, tsdata);
        put_ts64(b, 88, sec, nsec);              /* driver_tstamp */
    } else if (size == 108) {
        uint32_t tsdata = get32(b, 48);
        memset(b, 0, 108);
        put32(b, 0, st);
        put_ts32(b, 4, pcm.trig_sec, pcm.trig_nsec);
        put_ts32(b, 12, sec, nsec);
        put32(b, 20, to_ring(appl)); put32(b, 24, to_ring(hw));
        put32(b, 28, delay); put32(b, 32, av); put32(b, 36, av);
        put32(b, 48, tsdata);
        put_ts32(b, 60, sec, nsec);
    } else {
        return -ENOTTY_;
    }
    return 0;
}

/* SYNC_PTR: time64 (136 bytes) or time32 (132) layout. */
static int do_sync_ptr(uint8_t *b, uint32_t size) {
    uint32_t o_st, o_hw, o_ts, o_sus, o_ats, o_appl, o_amin, flags;
    uint32_t sec, nsec;
    int rc = 0;

    if (size == 136) {
        o_st = 8; o_hw = 16; o_ts = 24; o_sus = 40; o_ats = 48; o_appl = 72; o_amin = 76;
    } else if (size == 132) {
        o_st = 4; o_hw = 12; o_ts = 16; o_sus = 24; o_ats = 28; o_appl = 68; o_amin = 72;
    } else {
        return -ENOTTY_;
    }
    flags = get32(b, 0);
    LOCK();
    if (flags & SYNC_HWSYNC) {
        update_hw();
        if (pcm.state == ST_XRUN) rc = -EPIPE_;
    }
    if (rc == 0) {
        /* A new appl_ptr from the application: only RW access exists, where
         * the kernel moves it, so a move forward or back is not honoured. */
        if (!(flags & SYNC_AVAIL_MIN) && get32(b, o_amin))
            pcm.avail_min = get32(b, o_amin);
        put32(b, o_appl, to_ring(pcm.appl));
        put32(b, o_amin, pcm.avail_min);
        put32(b, o_st, (uint32_t)pcm.state);
        put32(b, o_hw, to_ring(pcm.hw));
        put32(b, o_sus, 0);
    }
    UNLOCK();
    if (rc) return rc;
    now(&sec, &nsec);
    if (size == 136) {
        put_ts64(b, o_ts, sec, nsec);
        put_ts64(b, o_ats, 0, 0);
    } else {
        put_ts32(b, o_ts, sec, nsec);
        put_ts32(b, o_ats, 0, 0);
    }
    return 0;
}

/* WRITEI_FRAMES: `frames` frames from user `ubuf`; returns frames or -errno. */
static int do_writei(uint32_t ubuf, uint32_t frames, int nonblock) {
    uint32_t done = 0;

    switch (pcm.state) {
    case ST_PREPARED: case ST_RUNNING: break;
    case ST_XRUN:      return -EPIPE_;
    default:           return -EBADFD_;
    }
    while (done < frames) {
        uint32_t av, n, outn;
        int st;

        LOCK();
        update_hw();
        st = pcm.state;
        av = avail_now();
        UNLOCK();
        if (st == ST_XRUN) return done ? (int)done : -EPIPE_;
        if (st != ST_RUNNING && st != ST_PREPARED) return done ? (int)done : -EBADFD_;
        if (av == 0) {
            if (nonblock) return done ? (int)done : -EAGAIN_;
            if (sleep_ticks(2) < 0) return done ? (int)done : -EINTR_;
            continue;
        }
        n = frames - done;
        if (n > av) n = av;
        if (n > chunk_frames()) n = chunk_frames();
        if (copy_from_user(in_buf, (const void *)(uintptr_t)(ubuf + done * pcm.frame_bytes),
                           n * pcm.frame_bytes) < 0)
            return done ? (int)done : -EFAULT_;
        outn = convert(in_buf, n);

        if (st == ST_PREPARED) {
            uint32_t bytes = outn * OUT_FRAME_BYTES;
            if (pcm.stage_len + bytes > pcm.stage_cap)
                bytes = pcm.stage_cap - pcm.stage_len;
            memcpy(pcm.stage + pcm.stage_len, out_buf, bytes);
            LOCK();
            pcm.stage_len += bytes;
            pcm.appl += n;
            UNLOCK();
            if (pcm.appl - pcm.hw >= pcm.start_threshold)
                pcm_start();
        } else {
            LOCK();
            pcm.pushing++;
            UNLOCK();
            out_write((const uint8_t *)out_buf, outn * OUT_FRAME_BYTES);
            LOCK();
            pcm.appl += n;
            pcm.pushing--;
            UNLOCK();
        }
        done += n;
    }
    return (int)done;
}

static int do_drain(int nonblock) {
    int st;

    switch (pcm.state) {
    case ST_OPEN: return -EBADFD_;
    case ST_SETUP: return 0;
    case ST_XRUN: pcm.state = ST_SETUP; return 0;
    case ST_PREPARED:
        if (pcm.appl == pcm.hw) { pcm.state = ST_SETUP; return 0; }
        pcm_start();
        break;
    default: break;
    }
    LOCK();
    if (pcm.state == ST_RUNNING) pcm.state = ST_DRAINING;
    UNLOCK();
    for (;;) {
        LOCK();
        update_hw();
        st = pcm.state;
        UNLOCK();
        if (st != ST_DRAINING) break;
        if (nonblock) return -EAGAIN_;
        if (sleep_ticks(2) < 0) return -EINTR_;
    }
    if (st == ST_XRUN) pcm.state = ST_SETUP;
    return 0;
}

static int do_drop(void) {
    if (pcm.state == ST_OPEN) return -EBADFD_;
    if (pcm.state == ST_SETUP) return 0;
    out_drop();
    LOCK();
    pcm.state = ST_SETUP;
    pcm.hw = pcm.appl;
    pcm.stage_len = 0;
    now(&pcm.trig_sec, &pcm.trig_nsec);
    UNLOCK();
    return 0;
}

static int do_prepare(void) {
    if (pcm.state == ST_OPEN || pcm.state == ST_DISCONNECTED) return -EBADFD_;
    if (pcm.state == ST_RUNNING || pcm.state == ST_DRAINING || pcm.state == ST_XRUN)
        out_drop();
    LOCK();
    pcm_reset_ptrs();
    pcm.state = ST_PREPARED;
    UNLOCK();
    return 0;
}

static int pcm_ioctl(uint32_t req, uint8_t *k, uint32_t size, void *uarg, int nonblock) {
    uint32_t nr = req & 0xFF;

    switch (nr) {
    case 0x00: put32(k, 0, PCM_VERSION); return 0;                  /* PVERSION */
    case 0x01: fill_pcm_info(k, 0); return 0;                         /* INFO */
    case 0x02: case 0x03: case 0x04: return 0;          /* TSTAMP, TTSTAMP, USER_PVERSION */
    case 0x10:                                                         /* HW_REFINE */
        if (size != sizeof(struct hw_params)) return -ENOTTY_;
        return hw_refine((struct hw_params *)k);
    case 0x20: case 0x24: return do_status(k, size);             /* STATUS, STATUS_EXT */
    case 0x21: {                                                       /* DELAY */
        int st;
        LOCK();
        update_hw();
        st = pcm.state;
        put32(k, 0, pcm.appl - pcm.hw);
        UNLOCK();
        if (st == ST_XRUN) return -EPIPE_;
        return (st == ST_RUNNING || st == ST_DRAINING || st == ST_PREPARED) ? 0 : -EBADFD_;
    }
    case 0x22: {                                                       /* HWSYNC */
        int st;
        LOCK();
        update_hw();
        st = pcm.state;
        UNLOCK();
        if (st == ST_XRUN) return -EPIPE_;
        return (st == ST_RUNNING || st == ST_DRAINING || st == ST_PREPARED) ? 0 : -EBADFD_;
    }
    case 0x23: return do_sync_ptr(k, size);                            /* SYNC_PTR */
    case 0x32: return -ENXIO_;                    /* CHANNEL_INFO: no mmap access */
    case 0x45: case 0x47: return -ENOSYS_;                   /* PAUSE, RESUME */
    case 0x46: case 0x49: put32(k, 0, 0); return 0;        /* REWIND, FORWARD: 0 frames */
    case 0x51: case 0x52: case 0x53: return -EINVAL_;      /* READI, WRITEN, READN */
    case 0x60: return -ENOSYS_;                                        /* LINK */
    case 0x61: return -EINVAL_;                                        /* UNLINK */
    }

    /* the rest change the stream: one at a time */
    {
        int rc = pcm_enter();
        if (rc < 0) return rc;
        switch (nr) {
        case 0x11:                                                     /* HW_PARAMS */
            rc = size == sizeof(struct hw_params) ? do_hw_params((struct hw_params *)k)
                                                  : -ENOTTY_;
            break;
        case 0x12:                                                     /* HW_FREE */
            if (pcm.state == ST_RUNNING || pcm.state == ST_DRAINING) rc = -EBADFD_;
            else pcm_release();
            break;
        case 0x13:                                                     /* SW_PARAMS */
            rc = size == sizeof(struct sw_params) ? do_sw_params((struct sw_params *)k)
                                                  : -ENOTTY_;
            break;
        case 0x40: rc = do_prepare(); break;                           /* PREPARE */
        case 0x41:                                                     /* RESET */
            if (pcm.state == ST_RUNNING || pcm.state == ST_PREPARED) {
                out_drop();
                LOCK();
                pcm.hw = pcm.appl;
                pcm.stage_len = 0;
                UNLOCK();
            } else {
                rc = -EBADFD_;
            }
            break;
        case 0x42:                                                     /* START */
            if (pcm.state != ST_PREPARED) rc = -EBADFD_;
            else if (pcm.appl == pcm.hw && pcm.stop_threshold < pcm.boundary) rc = -EPIPE_;
            else pcm_start();
            break;
        case 0x43: rc = do_drop(); break;                              /* DROP */
        case 0x44: rc = do_drain(nonblock); break;                     /* DRAIN */
        case 0x48:                                                     /* XRUN */
            if (pcm.state == ST_RUNNING || pcm.state == ST_PREPARED) {
                out_drop();
                pcm.state = ST_XRUN;
            } else {
                rc = -EBADFD_;
            }
            break;
        case 0x50: {                                                   /* WRITEI_FRAMES */
            struct xferi x;
            memcpy(&x, k, sizeof(x));
            rc = do_writei(x.buf, x.frames, nonblock);
            if (rc >= 0) {
                x.result = rc;
                rc = copy_to_user(uarg, &x, sizeof(x)) < 0 ? -EFAULT_ : 0;
            }
            break;
        }
        default:
            rc = -ENOTTY_;
        }
        pcm_leave();
        return rc;
    }
}

/* ── control ioctls ────────────────────────────────────────────────────── */

static int ctl_ioctl(uint32_t req, uint8_t *k) {
    switch (req & 0xFF) {
    case 0x00: put32(k, 0, CTL_VERSION); return 0;                    /* PVERSION */
    case 0x01:                                                         /* CARD_INFO */
        memset(k, 0, 376);
        put_str(k, 8, 16, "MaeroOS");
        put_str(k, 24, 16, "MaeroOS");
        put_str(k, 40, 32, ac97_present() ? "MaeroOS AC97" : "MaeroOS HDA");
        put_str(k, 72, 80, ac97_present() ? "MaeroOS AC97 (48 kHz S16 stereo out)"
                                          : "MaeroOS HDA (48 kHz S16 stereo out)");
        put_str(k, 168, 80, "MaeroOS Mixer");
        return 0;
    case 0x10:                                                         /* ELEM_LIST */
        put32(k, 8, 0);                                                /* used */
        put32(k, 12, 0);                                               /* count */
        return 0;
    case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
    case 0x1A: case 0x1B: case 0x1C:
        return -ENOENT_;                                 /* no elements, no TLVs */
    case 0x16: {                                                       /* SUBSCRIBE_EVENTS */
        int32_t v = (int32_t)get32(k, 0);
        if (v < 0) put32(k, 0, (uint32_t)ctl_subscribed);
        else ctl_subscribed = v != 0;
        return 0;
    }
    case 0x17: case 0x18: case 0x19: return -EPERM_;     /* ELEM_ADD/REPLACE/REMOVE */
    case 0x20: case 0x40: case 0x43:          /* HWDEP, RAWMIDI, UMP _NEXT_DEVICE */
        put32(k, 0, 0xFFFFFFFFU);
        return 0;
    case 0x30: {                                                       /* PCM_NEXT_DEVICE */
        int32_t d = (int32_t)get32(k, 0);
        put32(k, 0, d < 0 ? 0 : 0xFFFFFFFFU);
        return 0;
    }
    case 0x31: {                                                       /* PCM_INFO */
        uint32_t dev = get32(k, 0), sub = get32(k, 4), stream = get32(k, 8);
        if (dev != 0 || stream != 0 || (sub != 0 && sub != 0xFFFFFFFFU)) return -ENOENT_;
        fill_pcm_info(k, 0);
        return 0;
    }
    case 0x32: case 0x42: return 0;                      /* PREFER_SUBDEVICE */
    case 0xD0: return 0;                                                /* POWER */
    case 0xD1: put32(k, 0, 0); return 0;                                /* POWER_STATE: D0 */
    }
    return -ENOTTY_;
}

/* ── VFS glue ──────────────────────────────────────────────────────────── */

int alsa_node(const vfs_node_t *n) {
    return n == &ctl_node || n == &pcm_node;
}

int alsa_ioctl(vfs_node_t *n, uint32_t req, void *uarg, int nonblock) {
    uint32_t type = (req >> 8) & 0xFF, dir = req >> 30, size = (req >> 16) & 0x3FFF;
    uint8_t *k = NULL;
    int rc;

    if ((n == &ctl_node && type != 'U') || (n == &pcm_node && type != 'A'))
        return -ENOTTY_;
    if (n == &pcm_node && ((req & 0xFF) == 0x45 || (req & 0xFF) == 0x60))
        return -ENOSYS_;                  /* PAUSE, LINK: arguments are values */
    if (size > 1024) return -ENOTTY_;
    if (dir && size && !uarg) return -EFAULT_;
    /* A whole 1 KiB however small `size` is: the handlers write the full
     * structure their request number names, and only `size` goes back. */
    k = kmalloc(1024);
    if (!k) return -ENOMEM_;
    memset(k, 0, 1024);
    /* In for every direction: PCM_NEXT_DEVICE is declared _IOR, yet the
     * device number it starts from is read from the same int. */
    if (dir && size && copy_from_user(k, uarg, size) < 0) {
        kfree(k);
        return -EFAULT_;
    }
    if (n == &ctl_node)
        rc = ctl_ioctl(req, k);
    else
        rc = pcm_ioctl(req, k, size, uarg, nonblock);
    if (rc >= 0 && (dir & 2) && size && copy_to_user(uarg, k, size) < 0)
        rc = -EFAULT_;
    kfree(k);
    if (ALSA_TRACE) printk("[ALSA] ioctl %s %08x -> %d\n", n == &ctl_node ? "ctl" : "pcm", (unsigned)req, rc);
    return rc;
}

static uint32_t nodata_read(vfs_node_t *n, uint64_t off, uint32_t len, uint8_t *buf) {
    (void)n; (void)off; (void)len; (void)buf;
    return 0;
}

static uint32_t nodata_write(vfs_node_t *n, uint64_t off, uint32_t len,
                             const uint8_t *buf) {
    (void)n; (void)off; (void)len; (void)buf;
    return 0;
}

static int ctl_ready(vfs_node_t *n) {
    (void)n;
    return 0;                             /* no control events */
}

static int ctl_wready(vfs_node_t *n) {
    (void)n;
    return 1;
}

/* POLLOUT: avail_min frames free, or a state where writing fails at once. */
static int pcm_wready(vfs_node_t *n) {
    int r;
    (void)n;
    LOCK();
    update_hw();
    switch (pcm.state) {
    case ST_RUNNING: case ST_PREPARED:
        r = avail_now() >= pcm.avail_min;
        break;
    case ST_DRAINING:
        r = 0;
        break;
    default:
        r = 1;
    }
    UNLOCK();
    return r;
}

static vfs_node_t *pcm_open(vfs_node_t *n) {
    return pcm.opens ? NULL : n;           /* one substream */
}

static void pcm_retain(vfs_node_t *n) {
    (void)n;
    LOCK();
    pcm.opens++;
    UNLOCK();
}

static void pcm_close(vfs_node_t *n) {
    int last;
    (void)n;
    /* The last reference keeps opens at 1 until the release is done, so
     * no new open can start a stream that the release would then free. */
    LOCK();
    last = pcm.opens == 1;
    if (!last) pcm.opens--;
    UNLOCK();
    if (last) {
        /* Linux drains nothing on close either: queued PCM is dropped.
         * Linux also defers the release until no ioctl is running; here
         * close_fn runs as soon as the fd goes, so wait out the holder (a
         * sibling thread in START/WRITEI/DRAIN, which finishes as the device
         * plays) instead of freeing pcm.stage under it. */
        pcm_enter_how(0);
        pcm_release();
        LOCK();
        pcm.opens--;
        UNLOCK();
        pcm_leave();
    }
}

static vfs_node_t *snd_finddir(vfs_node_t *d, const char *name) {
    (void)d;
    if (!card_present) return NULL;
    if (strcmp(name, "controlC0") == 0) return &ctl_node;
    if (strcmp(name, "pcmC0D0p") == 0) return &pcm_node;
    return NULL;
}

static int snd_readdir(vfs_node_t *d, uint32_t idx, vfs_dirent_t *out) {
    vfs_node_t *n;
    (void)d;
    if (!card_present || idx > 1) return -1;
    n = idx == 0 ? &ctl_node : &pcm_node;
    out->ino = n->inode;
    out->type = VFS_FLAG_CHARDEV;
    strncpy(out->name, n->name, 255);
    out->name[255] = '\0';
    return 0;
}

static void chardev(vfs_node_t *n, const char *name, uint32_t ino, uint32_t minor) {
    memset(n, 0, sizeof(*n));
    strncpy(n->name, name, 255);
    n->flags = VFS_FLAG_CHARDEV;
    n->inode = ino;
    n->mask = 0666;                          /* like /dev/dsp: no audio group */
    n->rdev = (116U << 8) | minor;          /* ALSA's major */
    n->read_fn = nodata_read;
    n->write_fn = nodata_write;
}

void alsa_init(void) {
    spin_init(&lk);
    card_present = ac97_present() || hda_present();
    card_hw = ac97_present() ? "AC97" : "HDA";

    memset(&dir_node, 0, sizeof(dir_node));
    strncpy(dir_node.name, "snd", 255);
    dir_node.flags = VFS_FLAG_DIR;
    dir_node.inode = 40;
    dir_node.mask = 0755;
    dir_node.finddir_fn = snd_finddir;
    dir_node.readdir_fn = snd_readdir;

    chardev(&ctl_node, "controlC0", 41, 0);
    ctl_node.read_ready_fn = ctl_ready;
    ctl_node.write_ready_fn = ctl_wready;

    chardev(&pcm_node, "pcmC0D0p", 42, 16);
    pcm_node.read_ready_fn = ctl_ready;
    pcm_node.write_ready_fn = pcm_wready;
    pcm_node.open_fn = pcm_open;
    pcm_node.retain_fn = pcm_retain;
    pcm_node.close_fn = pcm_close;

    if (card_present)
        printk("[ALSA] card 0: %s, /dev/snd/controlC0 /dev/snd/pcmC0D0p\n", card_hw);
}

vfs_node_t *alsa_dev_dir(void) {
    return card_present ? &dir_node : NULL;
}
