#include "pipe.h"
#include "signal.h"
#include "process.h"
#include "scheduler.h"
#include "../mm/heap.h"
#include "../kernel/printk.h"
#include <stddef.h>

pipe_buf_t *pipe_alloc(void) {
    pipe_buf_t *p = kmalloc(sizeof(pipe_buf_t));
    if (!p) return NULL;
    p->head     = 0;
    p->count    = 0;
    p->nreaders = 1;
    p->nwriters = 1;
    return p;
}

/* True when the current process has a deliverable signal (not ignored). */
static int pipe_signal_pending(void) {
    return signal_interrupt_pending(current_proc);
}

int pipe_read(pipe_buf_t *p, char *buf, int len, int nonblock) {
    if (len <= 0) return 0;
    /* Wait for data; POSIX: return as soon as any is available (a read may
     * return fewer than len bytes). */
    /* NB: do NOT bound/timeout these blocking reads for Firefox.  A ~500ms
     * timeout-to-EAGAIN was tried (2026-06-26) to recover the IO thread from the
     * empty-self-heal-pipe hang, and it REGRESSED HARD (5/5 runs stalled, 4 with
     * no window at all) — Firefox's launch-phase blocking pipe reads are
     * load-bearing (legitimate sync barriers that must wait indefinitely), and
     * any timeout breaks them.  Classic indefinite blocking is required. */
    while (p->count == 0) {
        if (p->nwriters == 0) return 0;       /* EOF */
        if (nonblock) return -11;             /* -EAGAIN */
        if (pipe_signal_pending()) return -4; /* -EINTR */
        sleep_on(p);
    }
    int n = 0;
    int take = len;
    if ((uint32_t)take > p->count) take = (int)p->count;
    for (int i = 0; i < take; i++) {
        buf[n++] = p->data[p->head];
        p->head  = (p->head + 1) % PIPE_BUF_SIZE;
    }
    p->count -= (uint32_t)take;
    wake_up(p);   /* wake any blocked writers */
    io_wake();
    return n;
}

int pipe_write(pipe_buf_t *p, const char *buf, int len, int nonblock) {
    if (len <= 0) return 0;
    if (p->nreaders == 0) {
        signal_send(current_proc, SIGPIPE);
        return -32;  /* -EPIPE */
    }
    int n = 0;
    while (n < len) {
        while (p->count == PIPE_BUF_SIZE) {
            if (p->nreaders == 0) return n ? n : -32;
            if (nonblock) return n ? n : -11;             /* -EAGAIN */
            if (pipe_signal_pending()) return n ? n : -4; /* -EINTR */
            sleep_on(p);
        }
        int space = (int)(PIPE_BUF_SIZE - p->count);
        int put   = len - n;
        if (put > space) put = space;
        for (int i = 0; i < put; i++) {
            uint32_t tail   = (p->head + p->count) % PIPE_BUF_SIZE;
            p->data[tail]   = buf[n++];
            p->count++;
        }
        wake_up(p);   /* wake any blocked readers */
        io_wake();
    }
    return n;
}

static void pipe_try_free(pipe_buf_t *p) {
    if (p->nreaders <= 0 && p->nwriters <= 0) {
        wake_up(p);  /* wake anyone still sleeping on it */
        kfree(p);
    }
}

void pipe_close_read(pipe_buf_t *p) {
    p->nreaders--;
    wake_up(p);       /* wake writers — they'll get EPIPE */
    io_wake();        /* wake pollers/select waiting on POLLERR (nreaders==0) */
    pipe_try_free(p);
}

void pipe_close_write(pipe_buf_t *p) {
    p->nwriters--;
    wake_up(p);       /* wake readers — they'll get EOF */
    io_wake();        /* wake pollers/select: nwriters==0 → POLLHUP/EOF readable.
                       * Pollers sleep on the global io_activity channel, not on
                       * this pipe, so wake_up(p) alone would not rouse them until
                       * their poll-timeout re-check. */
    pipe_try_free(p);
}
