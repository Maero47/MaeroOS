#include "usocket.h"
#include "signal.h"
#include "process.h"
#include "scheduler.h"
#include "../mm/heap.h"
#include "../lib/string.h"
#include "../kernel/printk.h"
#include <stddef.h>

#define UBUF_SIZE  65536    /* per-direction stream ring */
#define UPATH_MAX  108      /* sockaddr_un sun_path */
#define UBACKLOG   16       /* pending connections per listener */
#define MAX_BOUND  32       /* named listening sockets system-wide */

/* SCM_RIGHTS in-flight fd batch.  Ancillary file descriptors travel WITH the
 * byte stream: a batch is tagged with the stream position (total bytes written)
 * at the point its data ended, and is delivered to the reader once it has
 * consumed up to that position.  Firefox's multiprocess IPC depends on this. */
typedef struct uscm {
    struct uscm *next;
    uint32_t     at;                  /* tx total_in when this batch was queued */
    int          nfds;
    proc_file_t  files[SCM_MAX_FDS];  /* retained file refs (ownership in queue) */
} uscm_t;

/* A refcounted unidirectional byte stream: one tx endpoint feeds it, one rx
 * endpoint drains it.  Either side closing is recorded so the other gets the
 * right end-of-stream behaviour (reader → EOF, writer → EPIPE). */
typedef struct ucbuf {
    int      refs;
    uint32_t head, count;
    uint32_t total_in, total_out;     /* cumulative bytes written / read */
    uscm_t  *scm_head, *scm_tail;     /* FIFO of in-flight SCM_RIGHTS batches */
    int      writer_closed;   /* tx endpoint gone → reader returns EOF */
    int      reader_closed;   /* rx endpoint gone → writer gets EPIPE  */
    uint8_t  data[UBUF_SIZE];
} ucbuf_t;

struct usocket {
    int        used;
    int        refs;
    int        type;
    int        listening;
    int        bound;
    char       path[UPATH_MAX];
    ucbuf_t   *rx;            /* we read from here (peer writes)  */
    ucbuf_t   *tx;            /* we write here  (peer reads)      */
    usocket_t *backlog[UBACKLOG];
    int        bl_head, bl_count;
};

static usocket_t *bound_table[MAX_BOUND];

/* ── ring buffer ─────────────────────────────────────────────────────────── */

static ucbuf_t *ucbuf_alloc(void) {
    ucbuf_t *b = (ucbuf_t *)kmalloc(sizeof(ucbuf_t));
    if (!b) return NULL;
    b->refs = 1;
    b->head = b->count = 0;
    b->total_in = b->total_out = 0;
    b->scm_head = b->scm_tail = NULL;
    b->writer_closed = b->reader_closed = 0;
    return b;
}

static void ucbuf_unref(ucbuf_t *b) {
    if (!b) return;
    if (--b->refs <= 0) {
        /* Drop any never-delivered SCM_RIGHTS fds (close the retained refs). */
        for (uscm_t *m = b->scm_head; m; ) {
            uscm_t *next = m->next;
            for (int i = 0; i < m->nfds; i++) fd_release(&m->files[i]);
            kfree(m);
            m = next;
        }
        wake_up(b);
        kfree(b);
    }
}

/* ── socket object ───────────────────────────────────────────────────────── */

static usocket_t *usocket_alloc(int type) {
    usocket_t *s = (usocket_t *)kmalloc(sizeof(usocket_t));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->used = 1;
    s->refs = 1;
    s->type = type;
    return s;
}

usocket_t *usocket_create(int type) { return usocket_alloc(type); }

const char *usocket_path(usocket_t *s) { return s ? s->path : ""; }

int usocket_socketpair(usocket_t **a, usocket_t **b) {
    usocket_t *sa = usocket_alloc(1);
    usocket_t *sb = usocket_alloc(1);
    ucbuf_t   *b1 = ucbuf_alloc();   /* a → b */
    ucbuf_t   *b2 = ucbuf_alloc();   /* b → a */

    if (!sa || !sb || !b1 || !b2) {
        if (sa) kfree(sa);
        if (sb) kfree(sb);
        if (b1) kfree(b1);
        if (b2) kfree(b2);
        return -12;                  /* -ENOMEM */
    }
    sa->tx = b1; sa->rx = b2;
    sb->rx = b1; sb->tx = b2;
    b1->refs = 2; b2->refs = 2;      /* one ref per endpoint */
    *a = sa; *b = sb;
    return 0;
}

int usocket_bind(usocket_t *s, const char *path) {
    if (s->bound || !path || !path[0]) return -22;     /* -EINVAL */
    for (int i = 0; i < MAX_BOUND; i++)
        if (bound_table[i] && strcmp(bound_table[i]->path, path) == 0)
            return -98;                                /* -EADDRINUSE */
    strncpy(s->path, path, UPATH_MAX - 1);
    s->path[UPATH_MAX - 1] = '\0';
    for (int i = 0; i < MAX_BOUND; i++)
        if (!bound_table[i]) { bound_table[i] = s; s->bound = 1; return 0; }
    s->path[0] = '\0';
    return -105;                                       /* -ENOBUFS */
}

int usocket_listen(usocket_t *s, int backlog) {
    (void)backlog;
    if (!s->bound) return -22;
    s->listening = 1;
    return 0;
}

int usocket_connect(usocket_t *s, const char *path) {
    usocket_t *L = NULL;
    if (s->rx || s->tx) return -22;                    /* already connected */
    for (int i = 0; i < MAX_BOUND; i++)
        if (bound_table[i] && bound_table[i]->listening &&
            strcmp(bound_table[i]->path, path) == 0) { L = bound_table[i]; break; }
    if (!L) return -111;                               /* -ECONNREFUSED */
    if (L->bl_count >= UBACKLOG) return -111;          /* backlog full */

    ucbuf_t   *c2s = ucbuf_alloc();                    /* client → server */
    ucbuf_t   *s2c = ucbuf_alloc();                    /* server → client */
    usocket_t *srv = usocket_alloc(1);                 /* server-side endpoint */
    if (!c2s || !s2c || !srv) {
        if (c2s) kfree(c2s);
        if (s2c) kfree(s2c);
        if (srv) kfree(srv);
        return -12;
    }
    s->tx   = c2s; s->rx   = s2c;
    srv->rx = c2s; srv->tx = s2c;
    c2s->refs = 2; s2c->refs = 2;

    L->backlog[(L->bl_head + L->bl_count) % UBACKLOG] = srv;
    L->bl_count++;
    wake_up(L);                                        /* wake accept() */
    io_wake();
    return 0;                                          /* server accepts later */
}

usocket_t *usocket_accept(usocket_t *s, int nonblock, int *err) {
    if (!s->listening) { *err = -22; return NULL; }
    while (s->bl_count == 0) {
        if (nonblock) { *err = -11; return NULL; }     /* -EAGAIN */
        if (signal_interrupt_pending(current_proc)) { *err = -4; return NULL; }
        sleep_on(s);
    }
    usocket_t *srv = s->backlog[s->bl_head];
    s->bl_head = (s->bl_head + 1) % UBACKLOG;
    s->bl_count--;
    *err = 0;
    return srv;                                        /* refs already 1 */
}

int usocket_read(usocket_t *s, void *buf, int len, int nonblock) {
    if (len <= 0) return 0;
    ucbuf_t *b = s->rx;
    if (!b) return -107;                               /* -ENOTCONN */
    while (b->count == 0) {
        if (b->writer_closed) return 0;                /* EOF */
        if (nonblock) return -11;
        if (signal_interrupt_pending(current_proc)) return -4;
        {   /* FLOW TRACE: a firefox thread about to BLOCK in read with an empty
             * rx buffer — log which socket end so we can pair it with the [uflow]
             * WR side and see if the writer ever targets this rx. */
            extern volatile int g_ipc_launch_started;
            static int sr = 0;
            if (g_ipc_launch_started && sr < 200 && current_proc &&
                current_proc->name[0]=='f' && current_proc->name[1]=='i' &&
                current_proc->name[4]=='f') {
                sr++;
                printk("[uflow] RDBLK pid=%d t%d rx=%x tx=%x wclosed=%d\n",
                       current_proc->pid, current_proc->tgid,
                       (unsigned)(uintptr_t)s->rx, (unsigned)(uintptr_t)s->tx,
                       b->writer_closed);
            }
        }
        sleep_on(b);
    }
    int take = len;
    if ((uint32_t)take > b->count) take = (int)b->count;
    /* Do not read past the next pending SCM_RIGHTS batch boundary, so its fds
     * are delivered together with (and not before) the data they accompany. */
    if (b->scm_head && b->scm_head->at > b->total_out) {
        uint32_t until = b->scm_head->at - b->total_out;
        if ((uint32_t)take > until) take = (int)until;
    }
    uint8_t *p = (uint8_t *)buf;
    for (int i = 0; i < take; i++) {
        p[i] = b->data[b->head];
        b->head = (b->head + 1) % UBUF_SIZE;
    }
    b->count -= (uint32_t)take;
    b->total_out += (uint32_t)take;
    wake_up(b);                                        /* wake blocked writers */
    io_wake();
    return take;
}

int usocket_write(usocket_t *s, const void *buf, int len, int nonblock) {
    if (len <= 0) return 0;
    ucbuf_t *b = s->tx;
    if (!b) return -107;
    const uint8_t *p = (const uint8_t *)buf;
    int n = 0;
    while (n < len) {
        while (b->count == UBUF_SIZE) {
            if (b->reader_closed) {
                signal_send(current_proc, SIGPIPE);
                return n ? n : -32;                    /* -EPIPE */
            }
            if (nonblock) return n ? n : -11;
            if (signal_interrupt_pending(current_proc)) return n ? n : -4;
            sleep_on(b);
        }
        if (b->reader_closed) {
            signal_send(current_proc, SIGPIPE);
            return n ? n : -32;
        }
        int space = (int)(UBUF_SIZE - b->count);
        int put   = len - n;
        if (put > space) put = space;
        for (int i = 0; i < put; i++) {
            uint32_t tail = (b->head + b->count) % UBUF_SIZE;
            b->data[tail] = p[n++];
            b->count++;
        }
        b->total_in += (uint32_t)put;
        wake_up(b);                                    /* wake blocked readers */
        io_wake();
    }
    {   /* [swk] launch-phase diag: a 1-byte write to an AF_UNIX socketpair is a
         * MessagePumpLibevent::ScheduleWork() wakeup kick.  Log it + the peer
         * rx buffer count so we can see if the IO thread's epoll should fire. */
        extern volatile int g_ipc_launch_started;
        extern volatile unsigned g_ff_io_nudge;
        if (g_ipc_launch_started && current_proc &&
            current_proc->name[0]=='f' && current_proc->name[1]=='i' &&
            current_proc->name[4]=='f') {
            /* A firefox unix-socket write during launch = IPC traffic / dispatch;
             * nudge so epoll-parked firefox IO threads self-heal immediately. */
            g_ff_io_nudge++;
            /* FLOW TRACE: who wrote how much to which socket-pair end.  Lets us
             * see whether the content CHILD wrote its hello (and to which sock)
             * and whether the parent ever wrote back — to locate the lost
             * handshake message in the parent↔child exchange. */
            static int sw = 0;
            if (sw < 200) { sw++;
                printk("[uflow] WR pid=%d t%d tx=%x rx=%x len=%d txcount=%d\n",
                       current_proc->pid, current_proc->tgid,
                       (unsigned)(uintptr_t)s->tx, (unsigned)(uintptr_t)s->rx,
                       n, s->tx ? (int)s->tx->count : -1);
            }
        }
    }
    return n;
}

/* SCM_RIGHTS: queue a batch of (already-retained) file refs onto the tx stream,
 * tagged at the current write position so they ride along with the bytes just
 * written.  Ownership of the refs transfers into the queue. */
int usocket_send_fds(usocket_t *s, proc_file_t *files, int n) {
    if (n <= 0) return 0;
    if (n > SCM_MAX_FDS) n = SCM_MAX_FDS;
    ucbuf_t *b = s->tx;
    if (!b) return -107;                               /* -ENOTCONN */
    uscm_t *m = (uscm_t *)kmalloc(sizeof(uscm_t));
    if (!m) return -12;
    m->next = NULL;
    m->at   = b->total_in;        /* deliver once reader consumes up to here */
    m->nfds = n;
    for (int i = 0; i < n; i++) m->files[i] = files[i];
    if (b->scm_tail) b->scm_tail->next = m; else b->scm_head = m;
    b->scm_tail = m;
    wake_up(b); io_wake();
    return n;
}

/* SCM_RIGHTS: pop the next batch whose data the reader has fully consumed.
 * Ownership of the returned refs transfers to the caller (which installs them
 * as fds).  Returns 0 if none ready.  Overflow beyond `max` is dropped. */
int usocket_recv_fds(usocket_t *s, proc_file_t *out, int max) {
    ucbuf_t *b = s->rx;
    if (!b || !b->scm_head) return 0;
    if (b->scm_head->at > b->total_out) return 0;      /* data not yet read */
    uscm_t *m = b->scm_head;
    int n = m->nfds;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = m->files[i];
    for (int i = n; i < m->nfds; i++) fd_release(&m->files[i]);   /* dropped */
    b->scm_head = m->next;
    if (!b->scm_head) b->scm_tail = NULL;
    kfree(m);
    return n;
}

/* True when a recvmsg should wake even with no pending data: a batch of fds is
 * ready for delivery (its accompanying data already consumed). */
int usocket_fds_ready(usocket_t *s) {
    ucbuf_t *b = s->rx;
    return b && b->scm_head && b->scm_head->at <= b->total_out;
}

int usocket_read_ready(usocket_t *s) {
    if (s->listening) return s->bl_count > 0;          /* accept() won't block */
    if (!s->rx) return 1;                              /* unconnected → ret err */
    return s->rx->count > 0 || s->rx->writer_closed ||
           usocket_fds_ready(s);
}

int usocket_write_ready(usocket_t *s) {
    if (!s->tx) return 1;
    return s->tx->reader_closed || s->tx->count < UBUF_SIZE;
}

/* Diagnostic: expose the rx/tx ucbuf pointers so a poll-blocked socket can be
 * correlated (rx ptr) against a writer's tx ptr ([uflow] WR) — to detect a
 * socketpair routing/inheritance MISMATCH (parent polls one pair, child writes
 * another). */
void usocket_dbg_ptrs(usocket_t *s, uint32_t *rx, uint32_t *tx) {
    if (rx) *rx = s ? (uint32_t)(uintptr_t)s->rx : 0;
    if (tx) *tx = s ? (uint32_t)(uintptr_t)s->tx : 0;
}

/* Diagnostic: rx buffer fill + whether the peer (writer) has closed.  -1 rx
 * means there is no rx buffer (unconnected). */
int usocket_rx_state(usocket_t *s, int *wclosed) {
    if (!s || !s->rx) { if (wclosed) *wclosed = -1; return -1; }
    if (wclosed) *wclosed = s->rx->writer_closed;
    return (int)s->rx->count;
}

void usocket_retain(usocket_t *s) { if (s) s->refs++; }

void usocket_release(usocket_t *s) {
    if (!s) return;
    if (--s->refs > 0) return;

    /* Last reference: tell the peer we're gone and drop our ring refs. */
    if (s->tx) { s->tx->writer_closed = 1; wake_up(s->tx); }
    if (s->rx) { s->rx->reader_closed = 1; wake_up(s->rx); }
    /* CRITICAL: also wake pollers.  A thread blocked in poll()/epoll_wait sleeps
     * on the global io_activity channel, NOT on these ring buffers, so wake_up()
     * alone does not rouse it — it only learns of the hangup at its next poll
     * re-check (or never, for an infinite poll).  Without this, a child process
     * polling its IPC socketpair never notices the parent's death (close) and
     * leaks forever, exhausting the process table.  pipe_close_write() already
     * does this (see proc/pipe.c); usockets must too.  Matches Linux waking
     * rd_wait/wr_wait + EPOLLHUP on the last close. */
    io_wake();
    ucbuf_unref(s->tx);
    ucbuf_unref(s->rx);

    if (s->bound)
        for (int i = 0; i < MAX_BOUND; i++)
            if (bound_table[i] == s) { bound_table[i] = NULL; break; }

    /* Drop any never-accepted pending connections. */
    while (s->bl_count > 0) {
        usocket_t *srv = s->backlog[s->bl_head];
        s->bl_head = (s->bl_head + 1) % UBACKLOG;
        s->bl_count--;
        usocket_release(srv);
    }
    kfree(s);
}

/* Diagnostic: identify a usocket's rx/tx buffers (shared with the peer) so the
 * launch-phase IPC trace can correlate an epoll-registered channel with the
 * peer's writes (the socket-process reply buffer). */
void *usocket_rx_id(usocket_t *s) { return s ? (void *)s->rx : 0; }
void *usocket_tx_id(usocket_t *s) { return s ? (void *)s->tx : 0; }
