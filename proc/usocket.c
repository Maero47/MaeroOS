#include "usocket.h"
#include "signal.h"
#include "process.h"
#include "scheduler.h"
#include "../fs/vfs.h"
#include "../mm/heap.h"
#include "../lib/string.h"
#include "../kernel/printk.h"
#include "syscall.h"       /* copy_to_user / copy_from_user */
#include <stddef.h>

#define UBUF_SIZE  65536    /* per-direction ring */
#define UBACKLOG   16       /* pending connections per listener */

/* SCM_RIGHTS in-flight fd batch.  Ancillary file descriptors travel WITH the
 * byte stream, exactly as on Linux: unix_stream_sendmsg attaches them to the
 * FIRST skb of the message, and unix_stream_read_generic detaches them as soon
 * as a recvmsg consumes any chunk of that skb.  So a batch is tagged with the
 * stream position where its message STARTS and becomes deliverable once the
 * reader has taken at least one byte from there — an 8 KiB sendmsg read in
 * 4 KiB pieces hands the fds over with the first piece.  Firefox's
 * multiprocess IPC depends on this.  On a record socket the tag is the start
 * of the record, so the fds go with the recvmsg that takes the record.
 *
 * The batch is queued BEFORE its bytes are written, which is what makes the tag
 * meaningful to a concurrent reader: the reader can only pass `at` by consuming
 * bytes that do not exist until after the batch is in the queue, so it can
 * never drain past the tag while the batch is invisible.  A sendmsg whose data
 * never makes it takes the batch back out again (usocket_cancel_fds). */
typedef struct uscm {
    struct uscm *next;
    uint32_t     id;                  /* identity for usocket_cancel_fds() */
    uint32_t     at;                  /* tx total_in where the message starts */
    int          nfds;
    proc_file_t  files[SCM_MAX_FDS];  /* retained file refs (ownership in queue) */
} uscm_t;

/* A refcounted unidirectional ring: tx endpoints feed it, one rx endpoint
 * drains it.  Either side closing (or shutting down) is recorded so the other
 * gets the right end-of-stream behaviour (reader → EOF, writer → EPIPE).  A
 * record ring holds [header][sender name][data] records instead of a byte
 * stream. */
typedef struct ucbuf {
    int      refs;
    int      record;          /* SEQPACKET / DGRAM framing */
    uint32_t head, count;
    uint32_t total_in, total_out;     /* cumulative bytes written / read */
    uscm_t  *scm_head, *scm_tail;     /* FIFO of in-flight SCM_RIGHTS batches */
    int      writer_closed;   /* tx endpoint gone / SHUT_WR → reader EOF */
    int      reader_closed;   /* rx endpoint gone / SHUT_RD → writer EPIPE */
    uint8_t  data[UBUF_SIZE];
} ucbuf_t;

/* Record header: data length in the low 24 bits, sender name length in the
 * high 8; the name bytes follow, then the data. */
#define REC_HDR        4U
#define REC_LEN(h)     ((h) & 0xFFFFFFU)
#define REC_NAMELEN(h) ((h) >> 24)

struct usocket {
    usocket_t   *all_next;    /* every live socket (names, garbage collector) */
    int          refs;        /* descriptors + in-flight batches + pins */
    int          pins;        /* syscalls in progress */
    int          inflight;    /* refs held by queued SCM_RIGHTS batches */
    int          closed;      /* no descriptor left: shut down, name released */
    int          type;
    int          listening;
    int          bound;
    usock_name_t name;        /* getsockname */
    usock_name_t peername;    /* getpeername */
    int          connected;
    int          shut_wr;     /* SHUT_WR on a datagram socket */
    vfs_node_t  *node;        /* socket inode of a filesystem name (retained) */
    usock_cred_t cred;        /* creator's, or the listener's since listen() */
    usock_cred_t peercred;    /* SO_PEERCRED */
    int          has_peercred;
    ucbuf_t     *rx;          /* we read from here (peer writes)  */
    ucbuf_t     *tx;          /* we write here  (peer reads)      */
    usocket_t   *backlog[UBACKLOG];
    int          bl_head, bl_count;
    int          gc_count, gc_cand, gc_alive;   /* unix_gc scratch */
};

static usocket_t *all_sockets;

static void unix_gc(void);

/* ── ring buffer ─────────────────────────────────────────────────────────── */

static ucbuf_t *ucbuf_alloc(int record) {
    ucbuf_t *b = (ucbuf_t *)kmalloc(sizeof(ucbuf_t));
    if (!b) return NULL;
    b->refs = 1;
    b->record = record;
    b->head = b->count = 0;
    b->total_in = b->total_out = 0;
    b->scm_head = b->scm_tail = NULL;
    b->writer_closed = b->reader_closed = 0;
    return b;
}

/* A batch left its queue: the socket refs it holds are no longer in flight
 * (they are about to be installed as descriptors or released). */
static void uscm_unflight(uscm_t *m) {
    for (int i = 0; i < m->nfds; i++)
        if (m->files[i].type == FD_USOCKET && m->files[i].usock)
            m->files[i].usock->inflight--;
}

static void uscm_destroy(uscm_t *m) {
    uscm_unflight(m);
    for (int i = 0; i < m->nfds; i++) fd_release(&m->files[i]);
    kfree(m);
}

/* Close every batch still queued on b.  The list is detached first: closing a
 * batch can release sockets, and with them other rings. */
static void ucbuf_purge_fds(ucbuf_t *b) {
    uscm_t *m = b->scm_head;
    b->scm_head = b->scm_tail = NULL;
    while (m) {
        uscm_t *next = m->next;
        uscm_destroy(m);
        m = next;
    }
}

static void ucbuf_unref(ucbuf_t *b) {
    if (!b) return;
    if (--b->refs <= 0) {
        ucbuf_purge_fds(b);          /* never-delivered SCM_RIGHTS fds */
        wake_up(b);
        kfree(b);
    }
}

/* Is this batch's message already being consumed?  Only then are its fds
 * deliverable.  A batch whose bytes have not been written yet sits at
 * at == total_in, which no reader can have passed, so queuing it early never
 * makes it deliverable early. */
static int uscm_ready(ucbuf_t *b, uscm_t *m) {
    return b->total_out > m->at;
}

/* Pop and close every batch the reader has now passed without collecting: a
 * plain read()/recv(), or a recvmsg with no control buffer, loses the fds
 * (Linux hands them to scm_recv, which destroys the scm when msg_controllen
 * is 0). */
static void ucbuf_drop_ready_fds(ucbuf_t *b) {
    while (b->scm_head && uscm_ready(b, b->scm_head)) {
        uscm_t *m = b->scm_head;
        b->scm_head = m->next;
        if (!b->scm_head) b->scm_tail = NULL;
        uscm_destroy(m);
    }
}

/* Raw ring copies at ring index `pos` (wrapping). */
static void ring_put_k(ucbuf_t *b, uint32_t pos, const void *src, uint32_t n) {
    pos %= UBUF_SIZE;
    uint32_t first = UBUF_SIZE - pos;
    if (first > n) first = n;
    memcpy(&b->data[pos], src, first);
    if (n > first) memcpy(&b->data[0], (const uint8_t *)src + first, n - first);
}

static void ring_get_k(ucbuf_t *b, uint32_t pos, void *dst, uint32_t n) {
    pos %= UBUF_SIZE;
    uint32_t first = UBUF_SIZE - pos;
    if (first > n) first = n;
    memcpy(dst, &b->data[pos], first);
    if (n > first) memcpy((uint8_t *)dst + first, &b->data[0], n - first);
}

/* Gather n bytes from the user iovecs into the ring at `pos`; -EFAULT on a bad
 * buffer (the ring's committed contents are untouched either way). */
static int ring_put_u(ucbuf_t *b, uint32_t pos, const usock_iov_t *iov, int niov,
                      uint32_t n) {
    for (int i = 0; i < niov && n; i++) {
        uint32_t seg = iov[i].len < n ? iov[i].len : n;
        const uint8_t *src = (const uint8_t *)iov[i].base;
        while (seg) {
            uint32_t p = pos % UBUF_SIZE;
            uint32_t run = UBUF_SIZE - p;
            if (run > seg) run = seg;
            if (copy_from_user(&b->data[p], src, run) < 0) return -14;
            src += run; pos += run; seg -= run; n -= run;
        }
    }
    return 0;
}

/* Scatter n bytes of the ring at `pos` into the user iovecs. */
static int ring_get_u(ucbuf_t *b, uint32_t pos, const usock_iov_t *iov, int niov,
                      uint32_t n) {
    for (int i = 0; i < niov && n; i++) {
        uint32_t seg = iov[i].len < n ? iov[i].len : n;
        uint8_t *dst = (uint8_t *)iov[i].base;
        while (seg) {
            uint32_t p = pos % UBUF_SIZE;
            uint32_t run = UBUF_SIZE - p;
            if (run > seg) run = seg;
            if (copy_to_user(dst, &b->data[p], run) < 0) return -14;
            dst += run; pos += run; seg -= run; n -= run;
        }
    }
    return 0;
}

/* ── socket object ───────────────────────────────────────────────────────── */

static void cred_self(usock_cred_t *c) {
    struct proc *p = current_proc;
    c->pid = p ? p->tgid : 0;
    c->uid = p ? p->euid : 0;
    c->gid = p ? p->egid : 0;
}

static int type_record(int type) {
    return type == USOCK_DGRAM || type == USOCK_SEQPACKET;
}

static usocket_t *usocket_alloc(int type) {
    usocket_t *s = (usocket_t *)kmalloc(sizeof(usocket_t));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->refs = 1;
    s->type = type;
    cred_self(&s->cred);
    s->all_next = all_sockets;
    all_sockets = s;
    return s;
}

/* Unlink from the global list and free.  Rings must already be dropped. */
static void usocket_free(usocket_t *s) {
    for (usocket_t **pp = &all_sockets; *pp; pp = &(*pp)->all_next)
        if (*pp == s) { *pp = s->all_next; break; }
    kfree(s);
}

usocket_t *usocket_create(int type) {
    usocket_t *s = usocket_alloc(type);
    if (!s) return NULL;
    /* A datagram socket is a receiver from the start: its queue is what
     * connect()/sendto() of other sockets point at. */
    if (type == USOCK_DGRAM) {
        s->rx = ucbuf_alloc(1);
        if (!s->rx) { usocket_free(s); return NULL; }
    }
    return s;
}

int usocket_type(usocket_t *s) { return s->type; }
int usocket_is_record(usocket_t *s) { return type_record(s->type); }
int usocket_listening(usocket_t *s) { return s->listening; }

const char *usocket_path(usocket_t *s) {
    if (!s || !s->name.len) return "";
    return s->name.path[0] ? s->name.path : s->name.path + 1;   /* abstract */
}

int usocket_socketpair(int type, usocket_t **a, usocket_t **b) {
    int rec = type_record(type);
    usocket_t *sa = usocket_alloc(type);
    usocket_t *sb = usocket_alloc(type);
    ucbuf_t   *b1 = ucbuf_alloc(rec);   /* a → b */
    ucbuf_t   *b2 = ucbuf_alloc(rec);   /* b → a */

    if (!sa || !sb || !b1 || !b2) {
        if (sa) usocket_free(sa);
        if (sb) usocket_free(sb);
        if (b1) kfree(b1);
        if (b2) kfree(b2);
        return -12;                  /* -ENOMEM */
    }
    sa->tx = b1; sa->rx = b2;
    sb->rx = b1; sb->tx = b2;
    b1->refs = 2; b2->refs = 2;      /* one ref per endpoint */
    sa->connected = sb->connected = 1;
    /* unix_socketpair → init_peercred on both ends: the creator's ids. */
    cred_self(&sa->peercred); sa->has_peercred = 1;
    cred_self(&sb->peercred); sb->has_peercred = 1;
    *a = sa; *b = sb;
    return 0;
}

static int name_eq(const usock_name_t *x, const usock_name_t *y) {
    return x->len == y->len && memcmp(x->path, y->path, x->len) == 0;
}

/* The live socket bound to a name: by inode for a filesystem name, by bytes
 * for an abstract one. */
static usocket_t *bound_lookup(const usock_name_t *name, vfs_node_t *node) {
    for (usocket_t *t = all_sockets; t; t = t->all_next) {
        if (!t->bound || t->closed) continue;
        if (node ? t->node == node : (!t->node && name_eq(&t->name, name)))
            return t;
    }
    return NULL;
}

int usocket_bind(usocket_t *s, const usock_name_t *name, vfs_node_t *node) {
    if (s->bound || s->closed) return -22;             /* -EINVAL */
    if (!name || !name->len || name->len > USOCK_PATH_MAX) return -22;
    if (!node && bound_lookup(name, NULL)) return -98; /* -EADDRINUSE */
    s->name = *name;
    s->name.path[s->name.len] = '\0';
    if (node) { vfs_retain(node); s->node = node; }
    s->bound = 1;
    return 0;
}

/* Linux unix_autobind: an abstract name of five hex digits. */
int usocket_autobind(usocket_t *s) {
    static uint32_t ordernum = 1;
    if (s->bound) return 0;
    for (int tries = 0; tries < 0x100000; tries++) {
        usock_name_t n;
        uint32_t v = ordernum++ & 0xFFFFF;
        n.path[0] = '\0';
        for (int i = 0; i < 5; i++)
            n.path[1 + i] = "0123456789abcdef"[(v >> (16 - 4 * i)) & 0xF];
        n.len = 6;
        n.path[6] = '\0';
        if (!bound_lookup(&n, NULL)) return usocket_bind(s, &n, NULL);
    }
    return -98;
}

int usocket_listen(usocket_t *s, int backlog) {
    (void)backlog;
    if (s->type == USOCK_DGRAM) return -95;            /* -EOPNOTSUPP */
    if (!s->bound || s->connected) return -22;         /* -EINVAL */
    s->listening = 1;
    /* The creds a connecting client will see as its peer (unix_listen →
     * init_peercred). */
    cred_self(&s->cred);
    return 0;
}

/* Wait for the listener's backlog to have room; L is pinned by the caller. */
static int backlog_wait(usocket_t *L, int nonblock) {
    while (L->bl_count >= UBACKLOG) {
        if (L->closed || !L->listening) return -111;   /* -ECONNREFUSED */
        if (nonblock) return -11;                      /* -EAGAIN */
        if (signal_interrupt_pending(current_proc)) return -4;
        sleep_on(L);
    }
    return (L->closed || !L->listening) ? -111 : 0;
}

int usocket_connect(usocket_t *s, const usock_name_t *name, vfs_node_t *node,
                    int nonblock) {
    if (s->listening) return -22;                      /* -EINVAL */

    if (s->type == USOCK_DGRAM) {
        /* (Re)set the default destination; AF_UNSPEC dissolves it. */
        ucbuf_t *old = s->tx;
        if (!name) {
            s->tx = NULL;
            s->connected = 0;
            s->has_peercred = 0;
            s->peername.len = 0;
            ucbuf_unref(old);
            return 0;
        }
        usocket_t *T = bound_lookup(name, node);
        if (!T) return -111;                           /* -ECONNREFUSED */
        if (T->type != s->type) return -91;            /* -EPROTOTYPE */
        T->rx->refs++;
        s->tx = T->rx;
        s->connected = 1;
        s->peername = T->name;
        ucbuf_unref(old);
        return 0;
    }

    if (s->connected || s->rx || s->tx) return -106;   /* -EISCONN */
    if (!name) return -22;
    usocket_t *L = bound_lookup(name, node);
    if (!L || !L->listening) return -111;              /* -ECONNREFUSED */
    if (L->type != s->type) return -91;                /* -EPROTOTYPE */

    usocket_pin(L);
    int r = backlog_wait(L, nonblock);
    if (r == 0 && s->closed) r = -9;
    if (r == 0 && (s->connected || s->rx || s->tx)) r = -106;
    if (r < 0) { usocket_unpin(L); return r; }

    int rec = type_record(s->type);
    ucbuf_t   *c2s = ucbuf_alloc(rec);                 /* client → server */
    ucbuf_t   *s2c = ucbuf_alloc(rec);                 /* server → client */
    usocket_t *srv = usocket_alloc(s->type);           /* server-side endpoint */
    if (!c2s || !s2c || !srv) {
        if (c2s) kfree(c2s);
        if (s2c) kfree(s2c);
        if (srv) usocket_free(srv);
        usocket_unpin(L);
        return -12;
    }
    s->tx   = c2s; s->rx   = s2c;
    srv->rx = c2s; srv->tx = s2c;
    c2s->refs = 2; s2c->refs = 2;
    s->connected = srv->connected = 1;

    /* Names: the accepted socket carries the listener's address, and the two
     * ends see each other's (unix_stream_connect). */
    srv->name = L->name;
    srv->peername = s->bound ? s->name : srv->peername;
    s->peername = L->name;
    /* Credentials: the client sees the listener's (as of listen()), the
     * server end sees the connecting process's (unix_stream_connect →
     * init_peercred / copy_peercred). */
    srv->cred = L->cred;
    cred_self(&srv->peercred); srv->has_peercred = 1;
    s->peercred = L->cred;     s->has_peercred = 1;

    L->backlog[(L->bl_head + L->bl_count) % UBACKLOG] = srv;
    L->bl_count++;
    wake_up(L);                                        /* wake accept() */
    io_wake();
    usocket_unpin(L);
    return 0;                                          /* server accepts later */
}

usocket_t *usocket_accept(usocket_t *s, int nonblock, int *err) {
    if (s->type == USOCK_DGRAM) { *err = -95; return NULL; }
    if (!s->listening) { *err = -22; return NULL; }
    while (s->bl_count == 0) {
        if (s->closed) { *err = -9; return NULL; }     /* closed under us */
        if (nonblock) { *err = -11; return NULL; }     /* -EAGAIN */
        if (signal_interrupt_pending(current_proc)) { *err = -4; return NULL; }
        sleep_on(s);
    }
    if (s->closed) { *err = -9; return NULL; }
    usocket_t *srv = s->backlog[s->bl_head];
    s->bl_head = (s->bl_head + 1) % UBACKLOG;
    s->bl_count--;
    wake_up(s);                                        /* a connect() waiting for room */
    *err = 0;
    return srv;                                        /* the backlog's ref */
}

/* ── byte streams ────────────────────────────────────────────────────────── */

static int stream_read(usocket_t *s, void *buf, int len, int flags) {
    if (len <= 0) return 0;
    ucbuf_t *b = s->rx;
    if (!b) return -107;                               /* -ENOTCONN */
    while (b->count == 0) {
        if (s->closed) return -9;                      /* closed under us */
        if (b->writer_closed || b->reader_closed) return 0;   /* EOF */
        if (flags & USOCK_NONBLOCK) return -11;
        if (signal_interrupt_pending(current_proc)) return -4;
        sleep_on(b);
    }
    if (s->closed) return -9;
    int take = len;
    if ((uint32_t)take > b->count) take = (int)b->count;
    /* Never glue two messages that carry different ancillary data: stop at the
     * START of the next SCM_RIGHTS batch, so the fds of one message are never
     * handed over together with bytes of the message after it (Linux
     * unix_stream_read_generic breaks its loop on !unix_skb_scm_eq). */
    for (uscm_t *m = b->scm_head; m; m = m->next)
        if (m->at > b->total_out) {
            uint32_t until = m->at - b->total_out;
            if ((uint32_t)take > until) take = (int)until;
            break;
        }
    /* buf is the caller's user buffer: fault-safe copies, in at most two runs
     * (the ring may wrap).  Nothing is consumed until the bytes have landed,
     * so a bad buffer is -EFAULT with the stream intact. */
    usock_iov_t iov = { buf, (uint32_t)take };
    if (ring_get_u(b, b->head, &iov, 1, (uint32_t)take) < 0)
        return -14;
    if (flags & USOCK_PEEK)
        return take;               /* MSG_PEEK: data (and fds) stay queued */
    b->head = (b->head + (uint32_t)take) % UBUF_SIZE;
    b->count -= (uint32_t)take;
    b->total_out += (uint32_t)take;
    /* The bytes are gone; unless the caller is a recvmsg that can carry them,
     * so are the fds that rode with them. */
    if (!(flags & USOCK_WANTFDS)) ucbuf_drop_ready_fds(b);
    wake_up(b);                                        /* wake blocked writers */
    io_wake();
    return take;
}

/* Writing to a ring nobody reads any more, or after our own SHUT_WR. */
static int epipe(int flags) {
    /* MSG_NOSIGNAL: plain EPIPE, no SIGPIPE (net/socket.c sock_sendmsg ->
     * unix_stream_sendmsg send_sig check). */
    if (!(flags & USOCK_NOSIGNAL)) signal_send(current_proc, SIGPIPE);
    return -32;                                        /* -EPIPE */
}

static int stream_write(usocket_t *s, const void *buf, int len, int flags) {
    if (len <= 0) return 0;
    ucbuf_t *b = s->tx;
    if (!b) return -107;
    const uint8_t *p = (const uint8_t *)buf;
    int n = 0;
    while (n < len) {
        for (;;) {
            if (s->closed) return n ? n : -9;
            if (b->reader_closed || b->writer_closed) {
                int e = epipe(flags);
                return n ? n : e;
            }
            if (b->count < UBUF_SIZE) break;
            if (flags & USOCK_NONBLOCK) return n ? n : -11;
            if (signal_interrupt_pending(current_proc)) return n ? n : -4;
            sleep_on(b);
        }
        int space = (int)(UBUF_SIZE - b->count);
        int put   = len - n;
        if (put > space) put = space;
        /* Fault-safe copies from the caller's user buffer; a bad buffer ends
         * the write with -EFAULT (or the count already written). */
        usock_iov_t iov = { (void *)(p + n), (uint32_t)put };
        if (ring_put_u(b, b->head + b->count, &iov, 1, (uint32_t)put) < 0)
            return n ? n : -14;
        n        += put;
        b->count += (uint32_t)put;
        b->total_in += (uint32_t)put;
        wake_up(b);                                    /* wake blocked readers */
        io_wake();
    }
    return n;
}

/* ── records (SOCK_SEQPACKET, SOCK_DGRAM) ────────────────────────────────── */

static uint32_t iov_total(const usock_iov_t *iov, int niov) {
    uint32_t t = 0;
    for (int i = 0; i < niov; i++) t += iov[i].len;
    return t;
}

static int usocket_send_fds_locked(ucbuf_t *b, proc_file_t *files, int n,
                                   uint32_t *id_out);

int usocket_send_record(usocket_t *s, const usock_iov_t *iov, int niov,
                        proc_file_t *fds, int nfds,
                        const usock_name_t *to, vfs_node_t *tonode, int flags) {
    ucbuf_t *b = NULL;
    usocket_t *T = NULL;
    uint32_t len = iov_total(iov, niov);

    if (to) {
        if (s->type != USOCK_DGRAM)                    /* unix_seqpacket_sendmsg */
            return s->connected ? -106 : -95;          /* -EISCONN / -EOPNOTSUPP */
        T = bound_lookup(to, tonode);
        if (!T) return -111;                           /* -ECONNREFUSED */
        if (T->type != USOCK_DGRAM) return -91;        /* -EPROTOTYPE */
        b = T->rx;
    } else {
        b = s->tx;
        if (!b) return s->type == USOCK_DGRAM ? -107 : -107;   /* -ENOTCONN */
    }

    uint32_t namelen = (s->type == USOCK_DGRAM) ? s->name.len : 0;
    uint32_t rec = REC_HDR + namelen + len;
    if (len > 0xFFFFFFU || rec > UBUF_SIZE) return -90;       /* -EMSGSIZE */

    /* Pin the ring across the wait: a named receiver can close meanwhile. */
    b->refs++;
    int r = 0;
    for (;;) {
        if (s->closed) { r = -9; break; }
        if (s->shut_wr || (s->type != USOCK_DGRAM && b->writer_closed)) {
            r = epipe(flags);
            break;
        }
        if (b->reader_closed) {
            /* A datagram receiver that went away refuses; a stream-like peer
             * that is gone (or shut its reading side) is a broken pipe. */
            r = (s->type == USOCK_DGRAM) ? -111 : epipe(flags);
            break;
        }
        if (UBUF_SIZE - b->count >= rec) break;
        if (flags & USOCK_NONBLOCK) { r = -11; break; }
        if (signal_interrupt_pending(current_proc)) { r = -4; break; }
        sleep_on(b);
    }
    if (r == 0) {
        /* Fill the free space first and commit only once every byte landed:
         * a bad buffer leaves the queue exactly as it was. */
        uint32_t tail = b->head + b->count;
        uint32_t hdr = len | (namelen << 24);
        r = ring_put_u(b, tail + REC_HDR + namelen, iov, niov, len);
        if (r == 0) {
            ring_put_k(b, tail, &hdr, REC_HDR);
            if (namelen) ring_put_k(b, tail + REC_HDR, s->name.path, namelen);
            /* The record and its fds go in together, with no sleep between:
             * no other sender's record can land in between. */
            if (nfds > 0) r = usocket_send_fds_locked(b, fds, nfds, NULL);
            if (r >= 0) {
                b->count += rec;
                b->total_in += rec;
                r = (int)len;
                wake_up(b);
                io_wake();
            }
        }
    }
    ucbuf_unref(b);
    return r;
}

int usocket_recv_record(usocket_t *s, const usock_iov_t *iov, int niov,
                        int flags, int *oflags, usock_name_t *from) {
    ucbuf_t *b = s->rx;
    if (oflags) *oflags = 0;
    if (from) from->len = 0;
    if (!b) return -107;                               /* -ENOTCONN */
    while (b->count == 0) {
        if (s->closed) return -9;
        if (b->reader_closed) return 0;                /* our SHUT_RD */
        /* A connected SEQPACKET peer that is gone is end-of-stream; a DGRAM
         * queue just waits for the next sender (Linux sets SHUTDOWN_MASK on
         * the peer only for stream and seqpacket sockets). */
        if (s->type == USOCK_SEQPACKET && b->writer_closed) return 0;
        if (flags & USOCK_NONBLOCK) return -11;
        if (signal_interrupt_pending(current_proc)) return -4;
        sleep_on(b);
    }
    if (s->closed) return -9;

    uint32_t hdr;
    ring_get_k(b, b->head, &hdr, REC_HDR);
    uint32_t len = REC_LEN(hdr), namelen = REC_NAMELEN(hdr);
    uint32_t want = iov_total(iov, niov);
    uint32_t copy = len < want ? len : want;
    if (ring_get_u(b, b->head + REC_HDR + namelen, iov, niov, copy) < 0)
        return -14;
    if (from) {
        if (s->type == USOCK_DGRAM) {
            from->len = namelen;
            ring_get_k(b, b->head + REC_HDR, from->path, namelen);
            from->path[namelen] = '\0';
        } else {
            *from = s->peername;
        }
    }
    if (len > copy && oflags) *oflags |= USOCK_MSG_TRUNC;
    if (!(flags & USOCK_PEEK)) {
        uint32_t rec = REC_HDR + namelen + len;
        b->head = (b->head + rec) % UBUF_SIZE;
        b->count -= rec;
        b->total_out += rec;
        if (!(flags & USOCK_WANTFDS)) ucbuf_drop_ready_fds(b);
        wake_up(b);
        io_wake();
    }
    /* recv(MSG_TRUNC) on a record socket reports the record's real size. */
    return (int)((flags & USOCK_TRUNC) ? len : copy);
}

int usocket_read(usocket_t *s, void *buf, int len, int flags) {
    if (type_record(s->type)) {
        usock_iov_t iov = { buf, len > 0 ? (uint32_t)len : 0 };
        return usocket_recv_record(s, &iov, 1, flags, NULL, NULL);
    }
    return stream_read(s, buf, len, flags);
}

int usocket_write(usocket_t *s, const void *buf, int len, int flags) {
    if (type_record(s->type)) {
        usock_iov_t iov = { (void *)buf, len > 0 ? (uint32_t)len : 0 };
        return usocket_send_record(s, &iov, 1, NULL, 0, NULL, NULL, flags);
    }
    return stream_write(s, buf, len, flags);
}

/* ── SCM_RIGHTS ──────────────────────────────────────────────────────────── */

static int usocket_send_fds_locked(ucbuf_t *b, proc_file_t *files, int n,
                                   uint32_t *id_out) {
    static uint32_t next_id = 1;
    if (n <= 0) return 0;
    if (n > SCM_MAX_FDS) n = SCM_MAX_FDS;
    uscm_t *m = (uscm_t *)kmalloc(sizeof(uscm_t));
    if (!m) return -12;
    m->next = NULL;
    /* Syscalls run under the BKL, but take the id atomically anyway so this
     * never depends on it; 0 means "no batch", so skip it on wrap. */
    uint32_t id;
    do {
        id = __atomic_fetch_add(&next_id, 1, __ATOMIC_RELAXED);
    } while (id == 0);
    m->id   = id;
    m->at   = b->total_in;        /* the bytes written next ride with it */
    m->nfds = n;
    for (int i = 0; i < n; i++) {
        m->files[i] = files[i];
        if (files[i].type == FD_USOCKET && files[i].usock)
            files[i].usock->inflight++;
    }
    if (b->scm_tail) b->scm_tail->next = m; else b->scm_head = m;
    b->scm_tail = m;
    if (id_out) *id_out = m->id;
    wake_up(b); io_wake();
    return n;
}

/* SCM_RIGHTS on a stream: queue a batch of (already-retained) file refs onto
 * the tx stream, tagged at the CURRENT write position — so this is called
 * BEFORE writing the message's data, and the fds are handed to the first
 * recvmsg that consumes any of the bytes that follow.  Ownership of the refs
 * transfers into the queue.  On success *id_out identifies the batch for
 * usocket_cancel_fds(). */
int usocket_send_fds(usocket_t *s, proc_file_t *files, int n, uint32_t *id_out) {
    ucbuf_t *b = s->tx;
    if (!b) return -107;                               /* -ENOTCONN */
    /* Nobody can ever receive them: the reader is gone for good. */
    if (b->reader_closed) return -32;
    return usocket_send_fds_locked(b, files, n, id_out);
}

/* Take a queued batch back out: the sendmsg that queued it wrote no data at
 * all, so on a stream socket Linux would have created no skb and destroyed the
 * scm (unix_stream_sendmsg never enters its loop for a zero-length message).
 * Returns 1 when the batch was still queued and the caller owns the refs again,
 * 0 when it has already been delivered or dropped and they are gone. */
int usocket_cancel_fds(usocket_t *s, uint32_t id) {
    ucbuf_t *b = (s && id) ? s->tx : NULL;
    if (!b) return 0;
    uscm_t *prev = NULL;
    for (uscm_t *m = b->scm_head; m; prev = m, m = m->next) {
        if (m->id != id) continue;
        if (prev) prev->next = m->next; else b->scm_head = m->next;
        if (b->scm_tail == m) b->scm_tail = prev;
        uscm_unflight(m);
        kfree(m);
        return 1;
    }
    return 0;
}

/* SCM_RIGHTS: pop the next batch whose message the reader has started to
 * consume.  Ownership of the returned refs transfers to the caller (which
 * installs them as fds).  Returns 0 if none ready.  Overflow beyond `max` is
 * dropped. */
int usocket_recv_fds(usocket_t *s, proc_file_t *out, int max) {
    ucbuf_t *b = s->rx;
    if (!b || !b->scm_head) return 0;
    if (!uscm_ready(b, b->scm_head)) return 0;         /* data not yet read */
    uscm_t *m = b->scm_head;
    b->scm_head = m->next;
    if (!b->scm_head) b->scm_tail = NULL;
    uscm_unflight(m);
    int n = m->nfds;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = m->files[i];
    for (int i = n; i < m->nfds; i++) fd_release(&m->files[i]);   /* dropped */
    kfree(m);
    return n;
}

/* True when a batch of fds is ready for delivery, i.e. the reader has started
 * consuming the message they were sent with. */
int usocket_fds_ready(usocket_t *s) {
    ucbuf_t *b = s ? s->rx : NULL;
    return b && b->scm_head && uscm_ready(b, b->scm_head);
}

/* ── shutdown, names, credentials ────────────────────────────────────────── */

/* shutdown(2): SHUT_RD ends our reading (reads return 0, the peer's writes
 * get EPIPE), SHUT_WR ends our writing (the peer reads EOF, our writes get
 * EPIPE) — Linux unix_shutdown sets the mirrored flags on both socks, and
 * returns 0 on an unconnected socket too. */
int usocket_shutdown(usocket_t *s, int how) {
    if (how < 0 || how > 2) return -22;                /* -EINVAL */
    if (s->type == USOCK_DGRAM) {
        /* Our own queue for SHUT_RD; a datagram socket's SHUT_WR only stops
         * this socket's sends (other senders share the receiver's queue). */
        if ((how == 0 || how == 2) && s->rx) {
            s->rx->reader_closed = 1;
            wake_up(s->rx);
        }
        if (how == 1 || how == 2) s->shut_wr = 1;
        io_wake();
        return 0;
    }
    if ((how == 0 || how == 2) && s->rx) {
        s->rx->reader_closed = 1;
        wake_up(s->rx);
    }
    if ((how == 1 || how == 2) && s->tx) {
        s->tx->writer_closed = 1;
        wake_up(s->tx);
    }
    io_wake();
    return 0;
}

int usocket_getname(usocket_t *s, int peer, usock_name_t *out) {
    if (peer) {
        if (!s->connected) return -107;                /* -ENOTCONN */
        *out = s->peername;
    } else {
        *out = s->name;
    }
    return 0;
}

void usocket_peercred(usocket_t *s, usock_cred_t *out) {
    if (s->has_peercred) { *out = s->peercred; return; }
    /* No peer: Linux reports pid 0 and the overflow ids. */
    out->pid = 0;
    out->uid = 65534;
    out->gid = 65534;
}

/* ── readiness ───────────────────────────────────────────────────────────── */

/* POLLHUP: the connected peer is gone.  Closing a connected AF_UNIX stream
 * socket sets the peer's sk_shutdown to SHUTDOWN_MASK (net/unix/af_unix.c
 * unix_release_sock), and unix_poll reports EPOLLHUP for exactly that — both
 * directions dead, which here means our reader end saw the writer go and our
 * writer end saw the reader go. */
int usocket_hup(usocket_t *s) {
    if (!s || !s->rx || !s->tx || s->type == USOCK_DGRAM) return 0;
    /* RCV_SHUTDOWN: the peer stopped writing or we stopped reading;
     * SEND_SHUTDOWN: the peer stopped reading or we stopped writing. */
    int rcv  = s->rx->writer_closed || s->rx->reader_closed;
    int send = s->tx->reader_closed || s->tx->writer_closed;
    return rcv && send;
}

int usocket_read_ready(usocket_t *s) {
    if (s->closed) return 1;
    if (s->listening) return s->bl_count > 0;          /* accept() won't block */
    if (!s->rx) return 1;                              /* unconnected → ret err */
    if (s->rx->count > 0 || s->rx->reader_closed || usocket_fds_ready(s))
        return 1;
    return s->type != USOCK_DGRAM && s->rx->writer_closed;
}

int usocket_write_ready(usocket_t *s) {
    if (s->closed || !s->tx || s->shut_wr) return 1;
    return s->tx->reader_closed || s->tx->writer_closed ||
           s->tx->count < UBUF_SIZE;
}

/* Diagnostic: identify a usocket's rx/tx buffers (shared with the peer) so the
 * launch-phase IPC trace can correlate an epoll-registered channel with the
 * peer's writes (the socket-process reply buffer). */
void *usocket_rx_id(usocket_t *s) { return s ? (void *)s->rx : 0; }
void *usocket_tx_id(usocket_t *s) { return s ? (void *)s->tx : 0; }

void usocket_stat(usocket_t *s, uint32_t *rx, uint32_t *tx, uint32_t *cap,
                  int *peer_gone) {
    if (cap)       *cap       = UBUF_SIZE;
    if (rx)        *rx        = (s && s->rx) ? s->rx->count : 0;
    if (tx)        *tx        = (s && s->tx) ? s->tx->count : 0;
    if (peer_gone) *peer_gone = (s && ((s->rx && s->rx->writer_closed) ||
                                       (s->tx && s->tx->reader_closed))) ? 1 : 0;
}

/* ── lifetime ────────────────────────────────────────────────────────────── */

/* The last descriptor is gone (only syscall pins, if any, remain): shut the
 * socket down as Linux unix_release_sock does, and wake every sleeper so the
 * syscalls still inside return -EBADF instead of touching freed memory. */
static void usocket_teardown(usocket_t *s) {
    if (s->closed) return;
    s->closed = 1;
    s->listening = 0;

    /* Tell the peer we're gone.  Batches queued for us can never be received
     * now, so close them (unix_release_sock purges the receive queue); that
     * is also what frees a socket whose only reference was in our queue. */
    if (s->tx && s->type != USOCK_DGRAM) {
        /* (A datagram tx is the receiver's own queue, shared by every
         * sender: one sender going away ends nothing.) */
        s->tx->writer_closed = 1;
        wake_up(s->tx);
    }
    if (s->rx) {
        s->rx->reader_closed = 1;
        wake_up(s->rx);
        ucbuf_purge_fds(s->rx);
    }
    if (s->bound) {
        s->bound = 0;
        if (s->node) { vfs_close(s->node); s->node = NULL; }
    }
    /* Drop any never-accepted pending connections. */
    while (s->bl_count > 0) {
        usocket_t *srv = s->backlog[s->bl_head];
        s->bl_head = (s->bl_head + 1) % UBACKLOG;
        s->bl_count--;
        usocket_release(srv);
    }
    wake_up(s);
    /* CRITICAL: also wake pollers.  A thread blocked in poll()/epoll_wait
     * sleeps on the global io_activity channel, NOT on these ring buffers, so
     * wake_up() alone does not rouse it — without this a child polling its IPC
     * socketpair never notices the parent's death and leaks forever.
     * pipe_close_write() does the same (see proc/pipe.c). */
    io_wake();
}

/* After any drop of a reference or pin: shut down once no descriptor is left,
 * free once nothing is left, and collect cycles once only in-flight refs are
 * left. */
static void usocket_settle(usocket_t *s) {
    if (s->refs <= 0) {
        usocket_teardown(s);
        ucbuf_unref(s->tx);
        ucbuf_unref(s->rx);
        s->tx = s->rx = NULL;
        usocket_free(s);
        return;
    }
    if (s->refs == s->pins) {
        usocket_teardown(s);
        return;
    }
    if (s->pins == 0 && s->refs == s->inflight)
        unix_gc();
}

void usocket_retain(usocket_t *s) { if (s) s->refs++; }

void usocket_release(usocket_t *s) {
    if (!s) return;
    s->refs--;
    usocket_settle(s);
}

void usocket_pin(usocket_t *s) {
    if (!s) return;
    s->refs++;
    s->pins++;
}

void usocket_unpin(usocket_t *s) {
    if (!s) return;
    s->pins--;
    s->refs--;
    usocket_settle(s);
}

/* ── garbage collection of SCM_RIGHTS cycles ─────────────────────────────────
 * A socket whose every reference sits in an SCM_RIGHTS batch can still be
 * received — unless each of those batches waits in the receive queue of a
 * socket that is itself only reachable that way.  Sending a socket (or its
 * peer) over its own connection and closing both descriptors builds exactly
 * that: the batch keeps the socket alive, and the socket keeps the ring and
 * the batch alive.  Linux breaks such cycles with unix_gc(); this is the same
 * algorithm:
 *   1. candidates: sockets with no descriptor or pin, only in-flight refs;
 *   2. subtract the in-flight refs that sit in candidates' receive queues;
 *      a candidate with some left is referenced from a live queue;
 *   3. everything reachable from such a candidate's queue is live too;
 *   4. the rest is garbage: close the batches in their receive queues, which
 *      drops the cycle's references and frees it.
 * It runs only when a socket's last non-in-flight reference goes, over the
 * sockets that exist, so its cost is bounded by the socket count. */

static void gc_scan(ucbuf_t *b, void (*fn)(usocket_t *)) {
    if (!b) return;
    for (uscm_t *m = b->scm_head; m; m = m->next)
        for (int i = 0; i < m->nfds; i++)
            if (m->files[i].type == FD_USOCKET && m->files[i].usock &&
                m->files[i].usock->gc_cand)
                fn(m->files[i].usock);
}

static void gc_dec(usocket_t *t) { t->gc_count--; }

static int gc_changed;
static void gc_mark(usocket_t *t) {
    if (!t->gc_alive) { t->gc_alive = 1; gc_changed = 1; }
}

static void unix_gc(void) {
    static int running, again;
    if (running) { again = 1; return; }
    running = 1;
    do {
        again = 0;
        int ncand = 0;
        for (usocket_t *s = all_sockets; s; s = s->all_next) {
            s->gc_cand = (!s->closed && s->pins == 0 && s->inflight > 0 &&
                          s->refs == s->inflight);
            s->gc_count = s->inflight;
            s->gc_alive = 0;
            if (s->gc_cand) ncand++;
        }
        if (!ncand) break;
        for (usocket_t *s = all_sockets; s; s = s->all_next)
            if (s->gc_cand) gc_scan(s->rx, gc_dec);
        for (usocket_t *s = all_sockets; s; s = s->all_next)
            if (s->gc_cand && s->gc_count > 0) s->gc_alive = 1;
        do {
            gc_changed = 0;
            for (usocket_t *s = all_sockets; s; s = s->all_next)
                if (s->gc_cand && s->gc_alive) gc_scan(s->rx, gc_mark);
        } while (gc_changed);

        /* Detach the garbage's queued batches first, then close them all:
         * closing frees sockets, which must not happen mid-walk. */
        uscm_t *dead = NULL;
        for (usocket_t *s = all_sockets; s; s = s->all_next) {
            if (!s->gc_cand || s->gc_alive || !s->rx) continue;
            uscm_t *m = s->rx->scm_head;
            s->rx->scm_head = s->rx->scm_tail = NULL;
            while (m) {
                uscm_t *next = m->next;
                m->next = dead;
                dead = m;
                m = next;
            }
        }
        while (dead) {
            uscm_t *next = dead->next;
            uscm_destroy(dead);
            dead = next;
        }
    } while (again);
    running = 0;
}
