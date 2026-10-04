#include "pipe.h"
#include "signal.h"
#include "process.h"
#include "scheduler.h"
#include "../mm/heap.h"
#include "../kernel/printk.h"
#include "syscall.h"       /* copy_to_user / copy_from_user */
#include <stddef.h>

pipe_buf_t *pipe_alloc(void) {
    pipe_buf_t *p = kmalloc(sizeof(pipe_buf_t));
    if (!p) return NULL;
    p->head     = 0;
    p->count    = 0;
    p->nreaders = 1;
    p->nwriters = 1;
    p->fifo     = 0;
    p->pins     = 0;
    kspin_init(&p->lock, "pipe");
    return p;
}

static void pipe_try_free(pipe_buf_t *p);

/* A blocking read or write holds a pin for its whole duration (as usocket does
 * with a reference): the descriptor that led here can be closed by another
 * thread sharing the fd table while this one sleeps, and with both ends closed
 * the buffer would otherwise be freed under it.  The last unpin frees a pipe
 * whose ends are all gone; the woken caller sees EOF or EPIPE first. */
static void pipe_pin(pipe_buf_t *p) {
    uint32_t fl = kspin_lock_irqsave(&p->lock);
    p->pins++;
    kspin_unlock_irqrestore(&p->lock, fl);
}
static int pipe_unpin(pipe_buf_t *p, int ret) {
    uint32_t fl = kspin_lock_irqsave(&p->lock);
    int last = --p->pins == 0;
    kspin_unlock_irqrestore(&p->lock, fl);
    if (last) pipe_try_free(p);
    return ret;
}

/* True when the current process has a deliverable signal (not ignored). */
static int pipe_signal_pending(void) {
    return signal_interrupt_pending(current_proc);
}

static int pipe_read_pinned(pipe_buf_t *p, char *buf, int len, int nonblock);
int pipe_read(pipe_buf_t *p, char *buf, int len, int nonblock) {
    if (len <= 0) return 0;
    pipe_pin(p);
    return pipe_unpin(p, pipe_read_pinned(p, buf, len, nonblock));
}

static int pipe_read_pinned(pipe_buf_t *p, char *buf, int len, int nonblock) {
    /* Wait for data; POSIX: return as soon as any is available (a read may
     * return fewer than len bytes). */
    /* NB: do NOT bound/timeout these blocking reads for Firefox.  A ~500ms
     * timeout-to-EAGAIN was tried (2026-06-26) to recover the IO thread from the
     * empty-self-heal-pipe hang, and it REGRESSED HARD (5/5 runs stalled, 4 with
     * no window at all) — Firefox's launch-phase blocking pipe reads are
     * load-bearing (legitimate sync barriers that must wait indefinitely), and
     * any timeout breaks them.  Classic indefinite blocking is required. */
    uint32_t fl = kspin_lock_irqsave(&p->lock);
    int err = 1;
    while (p->count == 0) {
        if (p->nwriters == 0) { err = 0; break; }        /* EOF */
        if (nonblock) { err = -11; break; }               /* -EAGAIN */
        if (pipe_signal_pending()) { err = -4; break; }   /* -EINTR */
        sleep_locked(p, &p->lock);
    }
    if (err <= 0) {
        kspin_unlock_irqrestore(&p->lock, fl);
        return err;
    }
    int take = len;
    if ((uint32_t)take > p->count) take = (int)p->count;
    uint32_t head = p->head;
    kspin_unlock_irqrestore(&p->lock, fl);
    /* buf is the caller's user buffer: fault-safe copies, in at most two runs
     * (the ring may wrap).  The bytes are consumed only once they have landed,
     * so a read into a bad buffer fails with -EFAULT and loses nothing. */
    int first = (int)(PIPE_BUF_SIZE - head);
    if (first > take) first = take;
    if (copy_to_user(buf, &p->data[head], (size_t)first) < 0)
        return -14;
    if (take > first && copy_to_user(buf + first, &p->data[0], (size_t)(take - first)) < 0)
        return -14;
    fl = kspin_lock_irqsave(&p->lock);
    p->head   = (p->head + (uint32_t)take) % PIPE_BUF_SIZE;
    p->count -= (uint32_t)take;
    wake_up(p);   /* wake any blocked writers */
    kspin_unlock_irqrestore(&p->lock, fl);
    io_wake_poll();
    return take;
}

static int pipe_write_pinned(pipe_buf_t *p, const char *buf, int len, int nonblock);
int pipe_write(pipe_buf_t *p, const char *buf, int len, int nonblock) {
    if (len <= 0) return 0;
    pipe_pin(p);
    return pipe_unpin(p, pipe_write_pinned(p, buf, len, nonblock));
}

static int pipe_write_pinned(pipe_buf_t *p, const char *buf, int len, int nonblock) {
    uint32_t fl = kspin_lock_irqsave(&p->lock);
    if (p->nreaders == 0) {
        kspin_unlock_irqrestore(&p->lock, fl);
        signal_send(current_proc, SIGPIPE);
        return -32;  /* -EPIPE */
    }
    int n = 0;
    while (n < len) {
        int err = 1;
        while (p->count == PIPE_BUF_SIZE) {
            if (p->nreaders == 0) { err = -32; break; }       /* reader gone */
            if (nonblock) { err = -11; break; }               /* -EAGAIN */
            if (pipe_signal_pending()) { err = -4; break; }   /* -EINTR */
            sleep_locked(p, &p->lock);
        }
        if (err <= 0) {
            kspin_unlock_irqrestore(&p->lock, fl);
            if (err == -32) signal_send(current_proc, SIGPIPE);   /* Linux pipe_write */
            return n ? n : err;
        }
        int space = (int)(PIPE_BUF_SIZE - p->count);
        int put   = len - n;
        if (put > space) put = space;
        uint32_t tail = (p->head + p->count) % PIPE_BUF_SIZE;
        kspin_unlock_irqrestore(&p->lock, fl);
        /* Fault-safe copies from the caller's user buffer, in at most two
         * runs around the ring end; a bad buffer ends the write with -EFAULT
         * (or the count already written).  The bytes become readable only
         * once count covers them, below. */
        int first = (int)(PIPE_BUF_SIZE - tail);
        if (first > put) first = put;
        if (copy_from_user(&p->data[tail], buf + n, (size_t)first) < 0 ||
            (put > first &&
             copy_from_user(&p->data[0], buf + n + first, (size_t)(put - first)) < 0))
            return n ? n : -14;
        n += put;
        fl = kspin_lock_irqsave(&p->lock);
        p->count += (uint32_t)put;
        wake_up(p);   /* wake any blocked readers */
        kspin_unlock_irqrestore(&p->lock, fl);
        io_wake_poll();
        fl = kspin_lock_irqsave(&p->lock);
    }
    kspin_unlock_irqrestore(&p->lock, fl);
    return n;
}

static void pipe_try_free(pipe_buf_t *p) {
    uint32_t fl = kspin_lock_irqsave(&p->lock);
    if (p->nreaders <= 0 && p->nwriters <= 0) {
        wake_up(p);  /* wake anyone still sleeping on it */
        if (p->pins > 0) {          /* the last pipe_unpin() comes back here */
            kspin_unlock_irqrestore(&p->lock, fl);
            return;
        }
        if (p->fifo) {
            /* The FIFO's node still points at this buffer and the next open
             * reuses it; like Linux, data nobody read is dropped here. */
            p->head  = 0;
            p->count = 0;
            kspin_unlock_irqrestore(&p->lock, fl);
            return;
        }
        kspin_unlock_irqrestore(&p->lock, fl);
        kfree(p);
        return;
    }
    kspin_unlock_irqrestore(&p->lock, fl);
}

void pipe_add_reader(pipe_buf_t *p) {
    uint32_t fl = kspin_lock_irqsave(&p->lock);
    p->nreaders++;
    kspin_unlock_irqrestore(&p->lock, fl);
}

void pipe_add_writer(pipe_buf_t *p) {
    uint32_t fl = kspin_lock_irqsave(&p->lock);
    p->nwriters++;
    kspin_unlock_irqrestore(&p->lock, fl);
}

void pipe_close_read(pipe_buf_t *p) {
    uint32_t fl = kspin_lock_irqsave(&p->lock);
    p->nreaders--;
    wake_up(p);       /* wake writers — they'll get EPIPE */
    kspin_unlock_irqrestore(&p->lock, fl);
    io_wake_poll();        /* wake pollers/select waiting on POLLERR (nreaders==0) */
    pipe_try_free(p);
}

void pipe_close_write(pipe_buf_t *p) {
    uint32_t fl = kspin_lock_irqsave(&p->lock);
    p->nwriters--;
    wake_up(p);       /* wake readers — they'll get EOF */
    kspin_unlock_irqrestore(&p->lock, fl);
    io_wake_poll();   /* wake pollers/select: nwriters==0 → POLLHUP/EOF readable.
                       * Pollers sleep on the poll channel, not on this pipe,
                       * so wake_up(p) alone would not rouse them until their
                       * poll-timeout re-check. */
    pipe_try_free(p);
}

pipe_buf_t *pipe_fifo_alloc(void) {
    pipe_buf_t *p = pipe_alloc();
    if (!p) return NULL;
    p->nreaders = 0;
    p->nwriters = 0;
    p->fifo     = 1;
    return p;
}

void pipe_fifo_free(pipe_buf_t *p) {
    if (!p) return;
    uint32_t fl = kspin_lock_irqsave(&p->lock);
    wake_up(p);
    if (p->pins > 0) {          /* a sleeper still holds it: it frees it */
        p->fifo = 0;
        p->nreaders = p->nwriters = 0;
        kspin_unlock_irqrestore(&p->lock, fl);
        return;
    }
    kspin_unlock_irqrestore(&p->lock, fl);
    kfree(p);
}
