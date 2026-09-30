/*
 * Interval timers that deliver signals: alarm(2), setitimer(2)/getitimer(2)
 * and the POSIX per-process timers timer_create(2) & co.
 *
 * Every timer lives in one fixed table keyed by thread-group id (the Linux
 * signal_struct owns both the itimers and the posix_timers list, so they are
 * per PROCESS, shared by its threads):
 *
 *   ITIMER_REAL     wall-clock; SIGALRM to the process.  alarm() is this timer
 *                   with no interval.
 *   ITIMER_VIRTUAL  counts down only while a thread of the process runs in user
 *                   mode (a tick that interrupts ring 3); SIGVTALRM.
 *   ITIMER_PROF     counts down while a thread of the process runs at all (the
 *                   same ticks the utime accounting charges); SIGPROF.
 *   POSIX timers    CLOCK_REALTIME / MONOTONIC / BOOTTIME, SIGEV_SIGNAL (to the
 *                   process), SIGEV_THREAD_ID (to one thread) or SIGEV_NONE.
 *
 * Lifetime follows Linux: fork() gives the child a new thread group and
 * therefore no timers (copy_signal starts it disarmed); execve() keeps the
 * itimers but deletes the POSIX timers (exit_itimers from begin_new_exec); the
 * process's end deletes all of them.
 *
 * Wall-clock deadlines are monotonic instants in nanoseconds.  The tick fires a
 * timer on the first tick at or after its deadline (clock_mono_to_tick), so it
 * is never early and at most one 10 ms tick late; getitimer/timer_gettime
 * report the exact remainder from the fine clock.
 *
 * Expiry runs from scheduler_tick (PIT on the BSP, LAPIC timer on the APs)
 * under the Big Kernel Lock with interrupts off.  It only marks the signal
 * pending and makes a sleeping target runnable (signal_send*), which allocates
 * nothing and takes no lock; the signal is delivered on that thread's next
 * return to user mode, and a blocking syscall it was sleeping in sees
 * signal_interrupt_pending() and returns -EINTR (restarted under SA_RESTART)
 * or -ERESTARTNOHAND.
 */
#include "ktimer.h"
#include "process.h"
#include "signal.h"
#include "syscall.h"
#include "../arch/i686/cpu/pit.h"
#include "../arch/i686/cpu/tsc.h"
#include "../arch/i686/cpu/percpu.h"
#include "../drivers/rtc.h"
#include <kernel/config.h>
#include <stdint.h>

#define KT_MAX        256         /* all timers, every process */
#define KT_POSIX_MAX  32          /* POSIX timers one process may hold */

#define ITIMER_REAL_K     0
#define ITIMER_VIRTUAL_K  1
#define ITIMER_PROF_K     2
#define KT_POSIX          3

#define SIGVTALRM_K  26
#define SIGPROF_K    27

#define SIGEV_SIGNAL_K     0
#define SIGEV_NONE_K       1
#define SIGEV_THREAD_K     2
#define SIGEV_THREAD_ID_K  4

#define TIMER_ABSTIME_K    1

#define CLK_REALTIME_K     0
#define CLK_MONOTONIC_K    1
#define CLK_BOOTTIME_K     7

#define NSEC_PER_SEC  1000000000ULL
#define EINVAL_K  22
#define EFAULT_K  14
#define EAGAIN_K  11

struct ktimer {
    uint8_t  used;
    uint8_t  kind;         /* ITIMER_* or KT_POSIX */
    uint8_t  armed;
    int      tgid;         /* owning process */
    int      id;           /* POSIX timer id (unique within the process) */
    int      clock;        /* POSIX: CLOCK_* it was created on */
    int      notify;       /* POSIX: SIGEV_* */
    int      signo;
    int      tid;          /* SIGEV_THREAD_ID: the thread it signals */
    uint64_t deadline;     /* REAL/POSIX: monotonic ns of the next expiry;
                            * VIRTUAL/PROF: ns of CPU time still to run.
                            * Saturates at KT_NS_MAX, which never expires. */
    uint64_t interval;     /* reload in ns; 0 = one-shot */
    uint8_t  queued;       /* POSIX: its signal was queued and may be pending */
    uint32_t overrun;      /* expiries while that signal was still pending */
    uint32_t overrun_last; /* timer_getoverrun(): overruns of the last delivered */
};

static struct ktimer ktimers[KT_MAX];
static int           ktimers_used;   /* fast path: nothing armed, nothing to scan */

/* ── helpers ──────────────────────────────────────────────────────────────── */

/* 64-bit unsigned divide (the kernel links no __udivdi3): shift-subtract,
 * used only when a timer is armed, read, or rearmed after a missed period. */
static uint64_t udivmod64(uint64_t n, uint64_t d, uint64_t *rem) {
    uint64_t q = 0, r = 0;
    if (!d) { if (rem) *rem = 0; return 0; }
    if (!(n >> 32) && !(d >> 32)) {
        if (rem) *rem = (uint32_t)n % (uint32_t)d;
        return (uint32_t)n / (uint32_t)d;
    }
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= 1ULL << i; }
    }
    if (rem) *rem = r;
    return q;
}

/* Every time value is kept in 64-bit nanoseconds, saturated at KT_NS_MAX
 * (Linux KTIME_MAX): a timer that far out never expires.  Deadlines are
 * compared in 64 bits too — a 32-bit tick difference wraps after ~248 days
 * and would make a far-future timer look already due. */
#define KT_NS_MAX  0x7FFFFFFFFFFFFFFFULL

static uint64_t ns_add(uint64_t a, uint64_t b) {
    return (a >= KT_NS_MAX || b >= KT_NS_MAX - a) ? KT_NS_MAX : a + b;
}

/* Monotonic time at the start of tick `now` (clock_mono() is tick * 10 ms plus
 * the TSC offset into the tick), so deadline <= this means the tick has
 * reached it: never early, at most one tick late. */
static uint64_t tick_ns(uint32_t now) { return (uint64_t)now * TICK_NS; }

static struct ktimer *kt_find(int tgid, int kind, int id) {
    for (int i = 0; i < KT_MAX; i++) {
        struct ktimer *t = &ktimers[i];
        if (t->used && t->tgid == tgid && t->kind == kind &&
            (kind != KT_POSIX || t->id == id))
            return t;
    }
    return (struct ktimer *)0;
}

static struct ktimer *kt_alloc(void) {
    for (int i = 0; i < KT_MAX; i++)
        if (!ktimers[i].used) {
            struct ktimer *t = &ktimers[i];
            __builtin_memset(t, 0, sizeof(*t));
            t->used = 1;
            ktimers_used++;
            return t;
        }
    return (struct ktimer *)0;
}

static void kt_free(struct ktimer *t) {
    if (!t->used) return;
    t->used = 0;
    t->armed = 0;
    ktimers_used--;
}

/* A live thread of thread group tgid (for delivering / liveness), or NULL. */
static struct proc *group_member(int tgid) {
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *q = &ptable[i];
        if (q->state == PROC_UNUSED || q->state == PROC_ZOMBIE || q->state == PROC_EMBRYO)
            continue;
        if (q->tgid == tgid) return q;
    }
    return (struct proc *)0;
}

/* Arm a wall-clock timer to expire at monotonic instant `dl`. */
static void kt_arm_at(struct ktimer *t, uint64_t dl) {
    t->deadline = dl;
    t->armed    = 1;
}

static int kt_signo(const struct ktimer *t) {
    switch (t->kind) {
    case ITIMER_REAL_K:    return SIGALRM;
    case ITIMER_VIRTUAL_K: return SIGVTALRM_K;
    case ITIMER_PROF_K:    return SIGPROF_K;
    default:               return t->signo;
    }
}

/* Is signal sig pending on thread tid of tgid (tid 0: the process)?  A
 * process-directed signal waits in the group's shared set (struct sigshared),
 * a thread-directed one in that thread's own pending_sigs. */
static int sig_pending_in(int tgid, int tid, int sig) {
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *q = &ptable[i];
        if (q->state == PROC_UNUSED || q->state == PROC_ZOMBIE || q->tgid != tgid) continue;
        if (tid && q->pid != tid) continue;
        uint32_t set = tid ? q->pending_sigs : signal_pending_set(q);
        if (set & (1u << sig)) return 1;
    }
    return 0;
}

/* Linux hands a POSIX timer's overrun count over at the delivery of its
 * signal (posixtimer_rearm from dequeue_signal).  Delivery here is the bit
 * leaving pending_sigs, so the hand-over happens at the first look after it:
 * the next expiry or a timer_getoverrun(). */
static void kt_sync_overrun(struct ktimer *t) {
    if (!t->queued || sig_pending_in(t->tgid, t->notify == SIGEV_THREAD_ID_K ? t->tid : 0,
                                     t->signo))
        return;
    t->queued = 0;
    t->overrun_last = t->overrun;
    t->overrun = 0;
}

/* One expiry: queue the signal.  Returns 0 if the process is gone (the caller
 * then frees the timer). */
static int kt_fire(struct ktimer *t) {
    if (t->kind == KT_POSIX && t->notify == SIGEV_NONE_K) return 1;
    int sig = kt_signo(t);
    struct proc *target = (struct proc *)0;
    if (t->kind == KT_POSIX && t->notify == SIGEV_THREAD_ID_K) {
        for (int i = 0; i < MAX_PROCS; i++) {
            struct proc *q = &ptable[i];
            if (q->state != PROC_UNUSED && q->state != PROC_ZOMBIE &&
                q->pid == t->tid && q->tgid == t->tgid) { target = q; break; }
        }
        if (!target) return group_member(t->tgid) != (struct proc *)0;
    } else {
        target = group_member(t->tgid);
        if (!target) return 0;
    }
    if (t->kind == KT_POSIX) {
        /* One signal per POSIX timer: an expiry while the last one is still
         * pending is an overrun, not a second signal. */
        kt_sync_overrun(t);
        if (t->queued) { t->overrun++; return 1; }
        t->queued = 1;
    }
    if (t->kind == KT_POSIX && t->notify == SIGEV_THREAD_ID_K)
        signal_send(target, sig);
    else
        signal_send_group(target, sig);
    return 1;
}

/* ── tick ─────────────────────────────────────────────────────────────────── */

void ktimer_tick(int user_mode) {
    if (!ktimers_used) return;
    uint32_t     now = pit_ticks();
    struct proc *cur = current_proc;
    int          bsp = (this_cpu_id() == 0);

    for (int i = 0; i < KT_MAX; i++) {
        struct ktimer *t = &ktimers[i];
        if (!t->used || !t->armed) continue;

        if (t->kind == ITIMER_REAL_K || t->kind == KT_POSIX) {
            /* Wall-clock timers run off the BSP's PIT tick only: that is the
             * tick that advances the clock, and one CPU doing it means one
             * expiry per deadline. */
            if (!bsp || t->deadline > tick_ns(now)) continue;
            if (!kt_fire(t)) { kt_free(t); continue; }
            if (!t->interval) { t->armed = 0; continue; }
            /* Periodic: the next expiry is a whole interval after the one
             * that just passed (hrtimer_forward), not after "now"; periods
             * missed entirely are overruns, not extra signals. */
            uint64_t dl = ns_add(t->deadline, t->interval);
            uint64_t mono = clock_mono_ns();
            if (dl <= mono) {             /* mono < KT_NS_MAX, so dl is finite */
                uint64_t missed = udivmod64(mono - dl, t->interval, (uint64_t *)0) + 1;
                dl += missed * t->interval;
                if (t->kind == KT_POSIX) t->overrun += (uint32_t)missed;
            }
            kt_arm_at(t, dl);
            continue;
        }

        /* CPU-time itimers: charge this CPU's tick to the running process. */
        if (!cur || cur->tgid != t->tgid) continue;
        if (t->kind == ITIMER_VIRTUAL_K && !user_mode) continue;
        if (t->deadline > TICK_NS) { t->deadline -= TICK_NS; continue; }
        if (!kt_fire(t)) { kt_free(t); continue; }
        if (t->interval) t->deadline = t->interval;
        else             t->armed = 0;
    }
}

/* ── lifetime hooks ───────────────────────────────────────────────────────── */

void ktimer_group_exit(int tgid) {
    for (int i = 0; i < KT_MAX; i++)
        if (ktimers[i].used && ktimers[i].tgid == tgid) kt_free(&ktimers[i]);
}

void ktimer_exec(int tgid) {
    for (int i = 0; i < KT_MAX; i++)
        if (ktimers[i].used && ktimers[i].tgid == tgid && ktimers[i].kind == KT_POSIX)
            kt_free(&ktimers[i]);
}

/* ── value conversion ─────────────────────────────────────────────────────── */

struct ktv32 { int32_t sec, usec; };
struct kitv32 { struct ktv32 interval, value; };
struct kts32 { int32_t sec, nsec; };
struct kits32 { struct kts32 interval, value; };
struct kts64 { int64_t sec, nsec; };
struct kits64 { struct kts64 interval, value; };

/* sec/nsec (already validated non-negative, nsec < 1e9) to saturated ns. */
static uint64_t ns_of(int64_t sec, int64_t nsec) {
    if ((uint64_t)sec >= KT_NS_MAX / NSEC_PER_SEC) return KT_NS_MAX;
    return ns_add((uint64_t)sec * NSEC_PER_SEC, (uint64_t)nsec);
}

/* 64-bit seconds into a 32-bit time field: clamp rather than wrap. */
static int32_t sec32(int64_t s) { return s > 0x7fffffffLL ? 0x7fffffff : (int32_t)s; }
static void split_ns(uint64_t ns, int64_t *sec, int64_t *nsec) {
    uint64_t r;
    *sec  = (int64_t)udivmod64(ns, NSEC_PER_SEC, &r);
    *nsec = (int64_t)r;
}

/* Remaining time and interval of t in ns (value 0 = disarmed). */
static void kt_read(const struct ktimer *t, uint64_t *value, uint64_t *interval) {
    *value = 0;
    *interval = t ? t->interval : 0;
    if (!t || !t->armed) return;
    if (t->kind == ITIMER_VIRTUAL_K || t->kind == ITIMER_PROF_K) {
        *value = t->deadline;
        return;
    }
    uint64_t mono = clock_mono_ns();
    /* Due but not yet run by the tick: still armed, so never report 0
     * (Linux itimer_get_remtime reports 1 us; the POSIX side 1 ns). */
    *value = t->deadline > mono ? t->deadline - mono : 1;
}

/* Arm or disarm the itimer `which` of the calling process to value/interval
 * (ns); the old setting goes to *oval and *oint. */
static int itimer_set(int which, uint64_t value, uint64_t interval,
                      uint64_t *oval, uint64_t *oint) {
    int tgid = current_proc->tgid;
    struct ktimer *t = kt_find(tgid, which, 0);
    kt_read(t, oval, oint);
    if (!value) {                       /* disarm; the slot is not needed */
        if (t) kt_free(t);
        return 0;
    }
    if (!t) {
        t = kt_alloc();
        if (!t) return -EAGAIN_K;
        t->kind = (uint8_t)which;
        t->tgid = tgid;
    }
    t->interval = interval;
    t->overrun = t->overrun_last = 0;
    if (which == ITIMER_REAL_K) {
        kt_arm_at(t, ns_add(clock_mono_ns(), value));
    } else {
        /* CPU time is charged a whole tick at a time; the timer fires on the
         * tick that uses up the budget. */
        t->deadline = value;
        t->armed = 1;
    }
    return 0;
}

/* ── alarm / setitimer / getitimer ────────────────────────────────────────── */

int sys_alarm(registers_t *regs) {
    uint32_t secs = regs->ebx;
    /* Linux alarm_setitimer on 32-bit: a timeval holds at most INT_MAX s. */
    if (secs > 0x7fffffffU) secs = 0x7fffffffU;
    uint64_t oval, oint;
    int r = itimer_set(ITIMER_REAL_K, (uint64_t)secs * NSEC_PER_SEC, 0, &oval, &oint);
    if (r < 0) return r;
    /* Linux alarm_setitimer: round the old remainder to the nearest second,
     * but never report a pending alarm as 0. */
    uint64_t orem;
    uint32_t osec = (uint32_t)udivmod64(oval, NSEC_PER_SEC, &orem);
    uint32_t ous  = (uint32_t)orem / 1000;
    if ((!osec && ous) || ous >= 500000) osec++;
    return (int)osec;
}

static int tv_to_ns(const struct ktv32 *tv, uint64_t *ns) {
    if (tv->sec < 0 || tv->usec < 0 || tv->usec >= 1000000) return -EINVAL_K;
    *ns = ns_of(tv->sec, (int64_t)tv->usec * 1000);
    return 0;
}

static void ns_to_tv(uint64_t ns, struct ktv32 *tv) {
    int64_t s, n;
    split_ns(ns, &s, &n);
    tv->sec  = sec32(s);
    tv->usec = (int32_t)((uint32_t)n / 1000);
    if (ns && !tv->sec && !tv->usec) tv->usec = 1;   /* armed: never 0 */
}

int sys_setitimer(registers_t *regs) {
    int which = (int)regs->ebx;
    void *unew = (void *)(uintptr_t)regs->ecx;
    void *uold = (void *)(uintptr_t)regs->edx;
    if (which < ITIMER_REAL_K || which > ITIMER_PROF_K) return -EINVAL_K;
    struct kitv32 nv;
    __builtin_memset(&nv, 0, sizeof(nv));
    /* A NULL new value disarms (Linux keeps this historical behaviour). */
    if (unew && copy_from_user(&nv, unew, sizeof(nv)) < 0) return -EFAULT_K;
    uint64_t val, iv, oval, oint;
    int r = tv_to_ns(&nv.value, &val);
    if (r < 0) return r;
    r = tv_to_ns(&nv.interval, &iv);
    if (r < 0) return r;
    r = itimer_set(which, val, iv, &oval, &oint);
    if (r < 0) return r;
    if (uold) {
        struct kitv32 ov;
        ns_to_tv(oval, &ov.value);
        ns_to_tv(oint, &ov.interval);
        if (copy_to_user(uold, &ov, sizeof(ov)) < 0) return -EFAULT_K;
    }
    return 0;
}

int sys_getitimer(registers_t *regs) {
    int which = (int)regs->ebx;
    void *ucur = (void *)(uintptr_t)regs->ecx;
    if (which < ITIMER_REAL_K || which > ITIMER_PROF_K) return -EINVAL_K;
    uint64_t val, iv;
    kt_read(kt_find(current_proc->tgid, which, 0), &val, &iv);
    struct kitv32 cv;
    ns_to_tv(val, &cv.value);
    ns_to_tv(iv, &cv.interval);
    return copy_to_user(ucur, &cv, sizeof(cv)) < 0 ? -EFAULT_K : 0;
}

/* ── POSIX timers ─────────────────────────────────────────────────────────── */

/* Linux i386 struct sigevent: sigval, signo, notify, then _tid for
 * SIGEV_THREAD_ID (the union is padded to 64 bytes; we read the head only). */
struct ksigevent { uint32_t value; int32_t signo; int32_t notify; int32_t tid; };

int sys_timer_create(registers_t *regs) {
    int clock = (int)regs->ebx;
    void *usev = (void *)(uintptr_t)regs->ecx;
    int *uid = (int *)(uintptr_t)regs->edx;
    if (clock != CLK_REALTIME_K && clock != CLK_MONOTONIC_K && clock != CLK_BOOTTIME_K)
        return -EINVAL_K;
    struct ksigevent ev = { 0, SIGALRM, SIGEV_SIGNAL_K, 0 };
    if (usev && copy_from_user(&ev, usev, sizeof(ev)) < 0) return -EFAULT_K;
    switch (ev.notify) {
    case SIGEV_NONE_K:
        break;
    case SIGEV_THREAD_ID_K: {
        struct proc *q = (struct proc *)0;
        for (int i = 0; i < MAX_PROCS; i++)
            if (ptable[i].state != PROC_UNUSED && ptable[i].state != PROC_ZOMBIE &&
                ptable[i].pid == ev.tid && ptable[i].tgid == current_proc->tgid)
                { q = &ptable[i]; break; }
        if (!q) return -EINVAL_K;
    }   /* the signal number is checked like SIGEV_SIGNAL's */
    /* fall through */
    case SIGEV_SIGNAL_K:
        if (ev.signo < 1 || ev.signo >= NSIGS) return -EINVAL_K;
        break;
    default:            /* SIGEV_THREAD is libc's, never the kernel's */
        return -EINVAL_K;
    }

    int tgid = current_proc->tgid, count = 0;
    uint32_t idmask = 0;
    for (int i = 0; i < KT_MAX; i++)
        if (ktimers[i].used && ktimers[i].tgid == tgid && ktimers[i].kind == KT_POSIX) {
            count++;
            if (ktimers[i].id < 32) idmask |= 1u << ktimers[i].id;
        }
    if (count >= KT_POSIX_MAX) return -EAGAIN_K;
    int id = 0;
    while (id < 32 && (idmask & (1u << id))) id++;

    if (uid && !access_ok(uid, sizeof(int))) return -EFAULT_K;
    struct ktimer *t = kt_alloc();
    if (!t) return -EAGAIN_K;
    t->kind   = KT_POSIX;
    t->tgid   = tgid;
    t->id     = id;
    t->clock  = clock;
    t->notify = ev.notify;
    t->signo  = ev.signo;
    t->tid    = ev.tid;
    if (copy_to_user(uid, &id, sizeof(id)) < 0) { kt_free(t); return -EFAULT_K; }
    return 0;
}

static struct ktimer *posix_timer(int id) {
    return kt_find(current_proc->tgid, KT_POSIX, id);
}

static int timer_settime_ns(int id, int flags, int64_t vs, int64_t vn,
                            int64_t is, int64_t in, uint64_t *oval, uint64_t *oint) {
    struct ktimer *t = posix_timer(id);
    if (!t) return -EINVAL_K;
    if (vs < 0 || vn < 0 || vn >= (int64_t)NSEC_PER_SEC ||
        is < 0 || in < 0 || in >= (int64_t)NSEC_PER_SEC)
        return -EINVAL_K;
    kt_read(t, oval, oint);
    t->overrun = t->overrun_last = 0;   /* Linux common_timer_set */
    t->queued = 0;
    uint64_t val = ns_of(vs, vn);
    if (!val) { t->armed = 0; t->interval = 0; return 0; }
    t->interval = ns_of(is, in);
    uint64_t mono = clock_mono_ns();
    uint64_t dl;
    if (flags & TIMER_ABSTIME_K) {
        /* REALTIME is the RTC boot epoch plus uptime (nothing steps it), so an
         * absolute wall-clock time maps onto a fixed monotonic instant. */
        if (t->clock == CLK_REALTIME_K) {
            uint64_t epoch = (uint64_t)rtc_boot_epoch() * NSEC_PER_SEC;
            dl = val >= KT_NS_MAX ? KT_NS_MAX : val > epoch ? val - epoch : 0;
        } else {
            dl = val;
        }
        if (dl < mono) dl = mono;      /* already past: expires on the next tick */
    } else {
        dl = ns_add(mono, val);
    }
    kt_arm_at(t, dl);
    return 0;
}

int sys_timer_settime(registers_t *regs) {
    void *unew = (void *)(uintptr_t)regs->edx, *uold = (void *)(uintptr_t)regs->esi;
    struct kits32 nv;
    if (!unew || copy_from_user(&nv, unew, sizeof(nv)) < 0) return -EFAULT_K;
    uint64_t oval, oint;
    int r = timer_settime_ns((int)regs->ebx, (int)regs->ecx, nv.value.sec, nv.value.nsec,
                             nv.interval.sec, nv.interval.nsec, &oval, &oint);
    if (r < 0 || !uold) return r;
    struct kits32 ov;
    int64_t s, n;
    split_ns(oval, &s, &n); ov.value.sec = sec32(s);    ov.value.nsec = (int32_t)n;
    split_ns(oint, &s, &n); ov.interval.sec = sec32(s); ov.interval.nsec = (int32_t)n;
    return copy_to_user(uold, &ov, sizeof(ov)) < 0 ? -EFAULT_K : 0;
}

int sys_timer_settime64(registers_t *regs) {
    void *unew = (void *)(uintptr_t)regs->edx, *uold = (void *)(uintptr_t)regs->esi;
    struct kits64 nv;
    if (!unew || copy_from_user(&nv, unew, sizeof(nv)) < 0) return -EFAULT_K;
    uint64_t oval, oint;
    int r = timer_settime_ns((int)regs->ebx, (int)regs->ecx, nv.value.sec, nv.value.nsec,
                             nv.interval.sec, nv.interval.nsec, &oval, &oint);
    if (r < 0 || !uold) return r;
    struct kits64 ov;
    split_ns(oval, &ov.value.sec, &ov.value.nsec);
    split_ns(oint, &ov.interval.sec, &ov.interval.nsec);
    return copy_to_user(uold, &ov, sizeof(ov)) < 0 ? -EFAULT_K : 0;
}

int sys_timer_gettime(registers_t *regs) {
    struct ktimer *t = posix_timer((int)regs->ebx);
    if (!t) return -EINVAL_K;
    uint64_t val, iv;
    kt_read(t, &val, &iv);
    struct kits32 cv;
    int64_t s, n;
    split_ns(val, &s, &n); cv.value.sec = sec32(s);    cv.value.nsec = (int32_t)n;
    split_ns(iv, &s, &n);  cv.interval.sec = sec32(s); cv.interval.nsec = (int32_t)n;
    return copy_to_user((void *)(uintptr_t)regs->ecx, &cv, sizeof(cv)) < 0 ? -EFAULT_K : 0;
}

int sys_timer_gettime64(registers_t *regs) {
    struct ktimer *t = posix_timer((int)regs->ebx);
    if (!t) return -EINVAL_K;
    uint64_t val, iv;
    kt_read(t, &val, &iv);
    struct kits64 cv;
    split_ns(val, &cv.value.sec, &cv.value.nsec);
    split_ns(iv, &cv.interval.sec, &cv.interval.nsec);
    return copy_to_user((void *)(uintptr_t)regs->ecx, &cv, sizeof(cv)) < 0 ? -EFAULT_K : 0;
}

int sys_timer_getoverrun(registers_t *regs) {
    struct ktimer *t = posix_timer((int)regs->ebx);
    if (!t) return -EINVAL_K;
    kt_sync_overrun(t);
    return t->overrun_last > 0x7fffffffU ? 0x7fffffff : (int)t->overrun_last;
}

int sys_timer_delete(registers_t *regs) {
    struct ktimer *t = posix_timer((int)regs->ebx);
    if (!t) return -EINVAL_K;
    kt_free(t);
    return 0;
}
