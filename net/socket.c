#include "socket.h"
#include "lwip_glue.h"
#include "net.h"
#include "firewall.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../kernel/printk.h"
#include "../arch/i686/cpu/pit.h"
#include "../proc/scheduler.h"
#include "../proc/process.h"
#include "../proc/signal.h"

#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/ip4_addr.h"
#include "lwip/pbuf.h"
#include "lwip/priv/tcp_priv.h"   /* TF_ACK_NOW for the window-update nudge */

/* Sleep until I/O activity (NIC IRQ etc.) or a short deadline. */
static void net_io_sleep(uint32_t ticks) {
    if (!current_proc) { yield(); return; }
    current_proc->wake_tick = pit_ticks() + ticks;
    sleep_on(&io_activity);
}

#define AF_INET_K      2
#define SOCK_STREAM_K  1
#define SOCK_DGRAM_K   2
#define IPPROTO_TCP_K  6
#define IPPROTO_UDP_K 17

/* The table itself is small (~100 bytes a slot): the per-socket buffers, a
 * 64 KiB RX ring for a stream socket or a 6 KiB datagram queue for a UDP one,
 * come from the kernel heap when the socket is created and go back on its last
 * close.  Held in the slot they made the table 2.2 MiB of .bss at 32 entries,
 * and the kernel image must fit boot.asm's initial mapping (linker.ld). */
#define MAX_NET_SOCKETS 128
#define UDP_QUEUE_DEPTH 4
#define UDP_PACKET_MAX 1536
#define TCP_RX_SIZE 65536   /* per-socket RX ring (burst headroom) */

enum {
    TCP_STATE_NONE = 0,
    TCP_STATE_CONNECTING,
    TCP_STATE_CONNECTED,
    TCP_STATE_CLOSING,
    TCP_STATE_CLOSED,
    TCP_STATE_ERROR,
};

typedef struct udp_packet {
    uint8_t data[UDP_PACKET_MAX];
    uint32_t len;
    uint32_t addr;
    uint16_t port;
} udp_packet_t;

struct net_socket {
    int used;
    int refs;
    int domain;
    int type;
    int protocol;
    struct udp_pcb *udp;
    struct tcp_pcb *tcp;
    int connected;
    int tx_shut;        /* shutdown(SHUT_WR/SHUT_RDWR) done: send is EPIPE */
    int rx_shut;        /* shutdown(SHUT_RD/SHUT_RDWR) done */
    int tcp_state;
    int tcp_error;      /* pending error (SO_ERROR), consumed when reported */
    int was_connected;  /* the handshake completed at some point */
    int peer_fin;       /* the peer's FIN arrived: recv reports EOF */
    uint8_t *tcp_rx;    /* SOCK_STREAM: TCP_RX_SIZE bytes from kmalloc */
    uint32_t tcp_rx_head;
    uint32_t tcp_rx_tail;
    uint32_t tcp_rx_count;
    ip_addr_t remote_addr;
    uint16_t remote_port;
    udp_packet_t *queue;  /* SOCK_DGRAM: UDP_QUEUE_DEPTH packets from kmalloc */
    int qhead;
    int qtail;
    int qcount;
};

static net_socket_t sockets[MAX_NET_SOCKETS];

static uint16_t bswap16(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

static uint32_t tcp_rx_space(net_socket_t *s) {
    return TCP_RX_SIZE - s->tcp_rx_count;
}

static void tcp_rx_push(net_socket_t *s, const uint8_t *data, uint32_t len) {
    for (uint32_t i = 0; i < len && s->tcp_rx_count < TCP_RX_SIZE; i++) {
        s->tcp_rx[s->tcp_rx_tail] = data[i];
        s->tcp_rx_tail = (s->tcp_rx_tail + 1) % TCP_RX_SIZE;
        s->tcp_rx_count++;
    }
}

static uint32_t tcp_rx_pop(net_socket_t *s, uint8_t *buf, uint32_t len) {
    uint32_t n = len;
    if (n > s->tcp_rx_count)
        n = s->tcp_rx_count;
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = s->tcp_rx[s->tcp_rx_head];
        s->tcp_rx_head = (s->tcp_rx_head + 1) % TCP_RX_SIZE;
        s->tcp_rx_count--;
    }
    return n;
}

static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                        const ip_addr_t *addr, u16_t port) {
    (void)pcb;
    net_socket_t *s = (net_socket_t *)arg;
    if (!s || !p)
        return;
    if (s->qcount >= UDP_QUEUE_DEPTH) {
        pbuf_free(p);
        return;
    }

    udp_packet_t *pkt = &s->queue[s->qtail];
    uint32_t len = p->tot_len;
    if (len > UDP_PACKET_MAX)
        len = UDP_PACKET_MAX;
    pbuf_copy_partial(p, pkt->data, (u16_t)len, 0);
    pkt->len = len;
    pkt->addr = ip_2_ip4(addr)->addr;
    pkt->port = port;
    s->qtail = (s->qtail + 1) % UDP_QUEUE_DEPTH;
    s->qcount++;
    pbuf_free(p);
}

static err_t tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)pcb;
    net_socket_t *s = (net_socket_t *)arg;
    if (!s)
        return ERR_ARG;
    if (err == ERR_OK) {
        s->tcp_state = TCP_STATE_CONNECTED;
        s->connected = 1;
        s->was_connected = 1;
        s->tcp_error = 0;
    } else {
        s->tcp_state = TCP_STATE_ERROR;
        s->tcp_error = -111;     /* -ECONNREFUSED */
    }
    return ERR_OK;
}

/*
 * Give the pcb to lwIP for good: no callbacks back into s, tcp_close to let
 * it finish the TCP close on its own, and s->tcp = NULL.  Every path that may
 * outlive the pcb must go through here, because lwIP frees pcbs in states
 * where it never calls the err callback:
 *   - TIME_WAIT (after our FIN, then the peer's): freed by tcp_slowtmr after
 *     2*MSL, or at once by tcp_kill_timewait when a new pcb is needed;
 *   - LAST_ACK completing with TF_RXCLOSED set (tcp_input skips errf when
 *     the application already shut the receive side).
 * A socket still holding the pointer then reached into freed memp memory on
 * close(), possibly a pcb since reused for another connection.
 *
 * In the states this is used from, tcp_close only marks the receive side
 * closed and, from ESTABLISHED/CLOSE_WAIT, sends the FIN; it never fails
 * there, but tcp_abort is kept as the fallback the release path always had.
 * `in_input` is set when called from an lwIP callback inside tcp_input, where
 * aborting the pcb is not allowed.
 */
static void socket_detach_pcb(net_socket_t *s, int in_input) {
    struct tcp_pcb *pcb = s->tcp;
    if (!pcb)
        return;
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_poll(pcb, NULL, 0);
    if (tcp_close(pcb) != ERR_OK && !in_input)
        tcp_abort(pcb);
    s->tcp = NULL;
    s->connected = 0;
}

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
                         err_t err) {
    net_socket_t *s = (net_socket_t *)arg;
    if (!s)
        return ERR_ARG;
    if (!p) {
        s->tcp_state = TCP_STATE_CLOSED;
        s->connected = 0;
        s->peer_fin = 1;
        /* The peer's FIN after ours (shutdown(SHUT_WR)) takes the pcb to
         * CLOSING or TIME_WAIT, from which lwIP frees it without telling
         * us.  Nothing is left to send or receive on it: let go now.  Data
         * already in the ring is still served by recv, then EOF. */
        if (s->tx_shut)
            socket_detach_pcb(s, 1);
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }
    (void)pcb;
    if (p->tot_len > tcp_rx_space(s)) {
        /* Ring full: refuse without freeing (lwIP keeps ownership and
         * redelivers via the TCP timer once we drain).  With the ring (64 KB)
         * larger than the window (~23 KB) this should never actually hit. */
        return ERR_MEM;
    }
    for (struct pbuf *q = p; q; q = q->next)
        tcp_rx_push(s, (const uint8_t *)q->payload, q->len);
    /* Do NOT tcp_recved() here — that would reopen the window immediately and
     * let the peer overrun the ring faster than the app drains it (→ refuse
     * → window 0 → stall).  The window is advanced in recvfrom as the app
     * actually consumes bytes, so the advertised window tracks ring space. */
    pbuf_free(p);
    return ERR_OK;
}

static void tcp_err_cb(void *arg, err_t err) {
    net_socket_t *s = (net_socket_t *)arg;
    if (!s)
        return;
    s->tcp = NULL;
    s->connected = 0;
    /* ERR_CLSD after the peer's FIN is an orderly close (LAST_ACK acked):
     * recv keeps reporting EOF instead of turning it into an error. */
    if (err == ERR_CLSD && s->tcp_state == TCP_STATE_CLOSED)
        return;
    /* The errno Linux's tcp_reset() leaves in sk_err: a reset answering our
     * SYN is ECONNREFUSED, one after the peer's FIN (CLOSE_WAIT: it has gone
     * and our data reached nobody) EPIPE, any other ECONNRESET. */
    int e;
    if (s->tcp_state == TCP_STATE_CONNECTING)
        e = (err == ERR_RST) ? -111 : -110;       /* -ECONNREFUSED/-ETIMEDOUT */
    else if (err == ERR_RST)
        e = s->peer_fin ? -32 : -104;             /* -EPIPE / -ECONNRESET */
    else
        e = (err == ERR_ABRT) ? -104 : -101;
    s->tcp_state = TCP_STATE_ERROR;
    s->tcp_error = e;
}

/* Linux sock_error(): report the pending error once. */
static int take_error_locked(net_socket_t *s) {
    int e = s->tcp_error;
    s->tcp_error = 0;
    return e;
}

static err_t tcp_poll_cb(void *arg, struct tcp_pcb *pcb) {
    (void)pcb;
    net_socket_t *s = (net_socket_t *)arg;
    return s && s->used ? ERR_OK : ERR_ABRT;
}

void net_sockets_init(void) {
    memset(sockets, 0, sizeof(sockets));
}

static int socket_create_locked(int domain, int type, int protocol, net_socket_t **out) {
    if (domain != AF_INET_K)
        return -97;
    if (type != SOCK_DGRAM_K && type != SOCK_STREAM_K)
        return -94;
    if (type == SOCK_DGRAM_K && protocol != 0 && protocol != IPPROTO_UDP_K)
        return -93;
    if (type == SOCK_STREAM_K && protocol != 0 && protocol != IPPROTO_TCP_K)
        return -93;

    net_socket_t *s = NULL;
    for (int i = 0; i < MAX_NET_SOCKETS && !s; i++)
        if (!sockets[i].used)
            s = &sockets[i];
    if (!s) {
        printk("[NET] socket: all %d sockets in use\n", MAX_NET_SOCKETS);
        return -24;
    }

    /* kmalloc takes the heap lock with interrupts off and never sleeps, so
     * it is safe under the caller's preempt_disable. */
    void *buf = type == SOCK_DGRAM_K
        ? kmalloc(UDP_QUEUE_DEPTH * sizeof(udp_packet_t))
        : kmalloc(TCP_RX_SIZE);
    if (!buf) {
        printk("[NET] socket: no memory for the socket buffer\n");
        return -12;
    }

    struct udp_pcb *upcb = NULL;
    struct tcp_pcb *tpcb = NULL;
    if (type == SOCK_DGRAM_K)
        upcb = udp_new_ip_type(IPADDR_TYPE_V4);
    else
        tpcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!upcb && !tpcb) {
        printk("[NET] socket: lwIP has no free %s pcb\n",
               type == SOCK_DGRAM_K ? "UDP" : "TCP");
        kfree(buf);
        return -12;
    }

    memset(s, 0, sizeof(*s));
    s->used = 1;
    s->refs = 1;
    s->domain = domain;
    s->type = type;
    s->protocol = protocol ? protocol :
                  (type == SOCK_DGRAM_K ? IPPROTO_UDP_K : IPPROTO_TCP_K);
    s->udp = upcb;
    s->tcp = tpcb;
    if (s->udp) {
        s->queue = (udp_packet_t *)buf;
        udp_recv(s->udp, udp_recv_cb, s);
    }
    if (s->tcp) {
        s->tcp_rx = (uint8_t *)buf;
        tcp_arg(s->tcp, s);
        tcp_recv(s->tcp, tcp_recv_cb);
        tcp_err(s->tcp, tcp_err_cb);
        tcp_poll(s->tcp, tcp_poll_cb, 2);
        s->tcp_state = TCP_STATE_NONE;
    }
    *out = s;
    return 0;
}

void net_socket_retain(net_socket_t *s) {
    if (s && s->used)
        s->refs++;
}

static void socket_release_locked(net_socket_t *s) {
    if (!s || !s->used)
        return;
    if (--s->refs > 0)
        return;
    if (s->udp)
        udp_remove(s->udp);
    socket_detach_pcb(s, 0);
    /* No lwIP callback can reach s any more (udp_remove, and the detach
     * cleared the TCP pcb's arg), so the buffers can go. */
    kfree(s->tcp_rx);
    kfree(s->queue);
    memset(s, 0, sizeof(*s));
}

static int socket_bind_locked(net_socket_t *s, const net_sockaddr_in_t *addr) {
    if (!s || !s->used || !addr)
        return -9;
    if (addr->family != AF_INET_K)
        return -97;
    ip_addr_t ip;
    ip_addr_set_ip4_u32(&ip, addr->addr);
    err_t e = s->type == SOCK_DGRAM_K
        ? udp_bind(s->udp, &ip, bswap16(addr->port))
        : tcp_bind(s->tcp, &ip, bswap16(addr->port));
    return e == ERR_OK ? 0 : -98;
}

int net_socket_connect(net_socket_t *s, const net_sockaddr_in_t *addr,
                       int nonblock) {
    if (!s || !s->used || !addr)
        return -9;
    if (addr->family != AF_INET_K)
        return -97;
    /* Egress firewall: block outbound by remote ip/port if a rule matches. */
    {
        int proto = (s->type == SOCK_DGRAM_K) ? FW_UDP : FW_TCP;
        int fr = firewall_check(FW_OUT, proto, addr->addr, bswap16(addr->port));
        if (fr < 0) return fr;
    }
    ip_addr_set_ip4_u32(&s->remote_addr, addr->addr);
    s->remote_port = bswap16(addr->port);
    if (s->type == SOCK_DGRAM_K) {
        preempt_disable();
        err_t e = udp_connect(s->udp, &s->remote_addr, s->remote_port);
        preempt_enable();
        if (e != ERR_OK)
            return -101;
        s->connected = 1;
        return 0;
    }

    /* Linux __inet_stream_connect: a handshake already under way is
     * -EALREADY, an established connection -EISCONN. */
    preempt_disable();
    int st = s->tcp_state, have_pcb = (s->tcp != NULL), was = s->was_connected;
    int pending = (st == TCP_STATE_ERROR && !was) ? take_error_locked(s) : 0;
    preempt_enable();
    if (st == TCP_STATE_CONNECTING) return -114;          /* -EALREADY */
    if (was) return -106;                                 /* -EISCONN */
    /* A failed non-blocking attempt not yet collected with SO_ERROR reports
     * its error here (inet_stream_connect -> sock_error). */
    if (pending) return pending;
    if (!have_pcb) return -22;      /* a failed attempt released the pcb */

    s->tcp_state = TCP_STATE_CONNECTING;
    s->tcp_error = 0;
    preempt_disable();
    err_t e = tcp_connect(s->tcp, &s->remote_addr, s->remote_port,
                          tcp_connected_cb);
    preempt_enable();
    if (e != ERR_OK) {
        s->tcp_state = TCP_STATE_ERROR;
        return -101;
    }
    /* O_NONBLOCK: the handshake goes on without us; poll() reports the
     * socket writable when it ends, and SO_ERROR says how. */
    if (nonblock)
        return -115;                                      /* -EINPROGRESS */
    /* Pin across the sleeping wait (see net_socket_recvfrom). */
    net_socket_retain(s);
    int r = -110;
    uint32_t start = pit_ticks();
    while ((uint32_t)(pit_ticks() - start) < 300U) {
        net_poll_all();
        if (s->tcp_state == TCP_STATE_CONNECTED) {
            r = 0;
            break;
        }
        if (s->tcp_state == TCP_STATE_ERROR) {
            preempt_disable();
            r = take_error_locked(s);
            preempt_enable();
            if (!r) r = -101;
            break;
        }
        /* A signal interrupts the wait (Linux inet_wait_for_connect ->
         * sock_intr_errno: -ERESTARTSYS, so EINTR or a restart per
         * SA_RESTART).  The handshake carries on regardless: a restarted
         * or repeated connect() sees -EALREADY until it ends, and poll()
         * plus SO_ERROR report how it did, as on Linux. */
        if (current_proc && signal_interrupt_pending(current_proc)) {
            r = -4;
            break;
        }
        net_io_sleep(2);
    }
    net_socket_release(s);
    return r;
}

int net_socket_is_stream(net_socket_t *s) {
    return s && s->type == SOCK_STREAM_K;
}

static int socket_sendto_locked(net_socket_t *s, const void *buf, uint32_t len,
                                const net_sockaddr_in_t *addr) {
    if (!s || !s->used || !buf)
        return -9;
    if (s->type == SOCK_STREAM_K) {
        if (s->tx_shut)
            return -32;   /* -EPIPE, as Linux after SHUT_WR */
        /* Linux tcp_sendmsg -> sk_stream_error: a pending error is reported
         * once (ECONNRESET after a reset, EPIPE after a reset that followed
         * the peer's FIN); after that the connection is shut both ways and
         * every send is EPIPE. */
        if (s->tcp_state == TCP_STATE_ERROR) {
            int e = take_error_locked(s);
            return e ? e : -32;
        }
        if (s->tcp_state == TCP_STATE_CONNECTING)
            return -11;   /* a blocking sender waits for the handshake */
        if (!s->tcp)
            return s->was_connected ? -32 : -107;
        /* Sending after the peer's FIN is allowed (CLOSE_WAIT): TCP is
         * half-duplex-closeable, and the peer answers with a reset if it has
         * really gone, which the next send reports. */
        if (!s->was_connected)
            return -107;  /* -ENOTCONN */
        if (len == 0)
            return 0;
        uint32_t left = len;
        const uint8_t *p = (const uint8_t *)buf;
        uint32_t sent = 0;
        int err = -11;
        while (left > 0) {
            /* Clamp in 32 bits: a (uint16_t) cast of left would turn a
             * multiple of 64 KiB into a 0-byte write that tcp_write() accepts,
             * spinning here forever with preemption off. */
            uint32_t chunk = left;
            uint32_t avail = tcp_sndbuf(s->tcp);
            if (avail == 0)
                break;
            if (chunk > avail)
                chunk = avail;
            if (chunk > 1460)
                chunk = 1460;
            err_t e = tcp_write(s->tcp, p, (u16_t)chunk, TCP_WRITE_FLAG_COPY);
            /* Only ERR_MEM (send buffer or segment queue full) is worth
             * waiting on; the blocking caller sleeps and retries on -EAGAIN.
             * Anything else (ERR_CONN once the pcb is past ESTABLISHED/
             * CLOSE_WAIT, e.g. after a FIN went out) never clears, and
             * reporting it as -EAGAIN left a blocking send asleep forever. */
            if (e != ERR_OK) {
                if (e != ERR_MEM)
                    err = -32;   /* -EPIPE */
                break;
            }
            tcp_output(s->tcp);
            p += chunk;
            left -= chunk;
            sent += chunk;
            net_poll_all();
        }
        return sent ? (int)sent : err;
    }
    if (len > UDP_PACKET_MAX)
        return -90;

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_POOL);
    if (!p)
        return -12;
    pbuf_take(p, buf, (u16_t)len);

    err_t e;
    if (addr) {
        if (addr->family != AF_INET_K) {
            pbuf_free(p);
            return -97;
        }
        if (firewall_check(FW_OUT, FW_UDP, addr->addr,
                           bswap16(addr->port)) < 0) {
            pbuf_free(p);
            return -13;   /* -EACCES */
        }
        ip_addr_t ip;
        ip_addr_set_ip4_u32(&ip, addr->addr);
        e = udp_sendto(s->udp, p, &ip, bswap16(addr->port));
    } else if (s->connected) {
        e = udp_send(s->udp, p);
    } else {
        pbuf_free(p);
        return -89;
    }

    pbuf_free(p);
    net_poll_all();
    return e == ERR_OK ? (int)len : -101;
}

/* Copy up to `len` bytes from the front of the RX ring without consuming
 * them (MSG_PEEK). */
static uint32_t tcp_rx_peek(net_socket_t *s, uint8_t *buf, uint32_t len) {
    uint32_t n = len < s->tcp_rx_count ? len : s->tcp_rx_count;
    uint32_t at = s->tcp_rx_head;
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = s->tcp_rx[at];
        at = (at + 1) % TCP_RX_SIZE;
    }
    return n;
}

static int socket_recvfrom_locked(net_socket_t *s, void *buf, uint32_t len,
                                  net_sockaddr_in_t *addr, int peek) {
    if (!s || !s->used || !buf)
        return -9;

    net_poll_all();
    if (s->type == SOCK_STREAM_K) {
        if (s->tcp_rx_count == 0) {
            /* Linux tcp_recvmsg: the peer's FIN is EOF even if a reset came
             * after it; else a pending error is reported once, then EOF. */
            if (s->peer_fin || s->rx_shut || s->tcp_state == TCP_STATE_CLOSED)
                return 0;
            if (s->tcp_state == TCP_STATE_ERROR)
                return take_error_locked(s);
            if (!s->was_connected && s->tcp_state != TCP_STATE_CONNECTING)
                return -107;                               /* -ENOTCONN */
            return -11;
        }
        if (peek)
            return (int)tcp_rx_peek(s, (uint8_t *)buf, len);
        int got = (int)tcp_rx_pop(s, (uint8_t *)buf, len);
        /* Advance the TCP receive window by what the app just consumed: this
         * is the flow-control signal that lets the peer keep sending.  Also
         * nudges lwIP to redeliver any refused_data now that the ring drained. */
        if (got > 0 && s->tcp)
            tcp_recved(s->tcp, (uint16_t)got);
        return got;
    }

    if (s->qcount == 0)
        return -11;

    udp_packet_t *pkt = &s->queue[s->qhead];
    if (peek) {                     /* MSG_PEEK: leave the datagram queued */
        uint32_t n = pkt->len < len ? pkt->len : len;
        memcpy(buf, pkt->data, n);
        if (addr) {
            memset(addr, 0, sizeof(*addr));
            addr->family = AF_INET_K;
            addr->port = bswap16(pkt->port);
            addr->addr = pkt->addr;
        }
        return (int)n;
    }
    uint32_t n = pkt->len;
    if (n > len)
        n = len;
    memcpy(buf, pkt->data, n);

    if (addr) {
        memset(addr, 0, sizeof(*addr));
        addr->family = AF_INET_K;
        addr->port = bswap16(pkt->port);
        addr->addr = pkt->addr;
    }

    s->qhead = (s->qhead + 1) % UDP_QUEUE_DEPTH;
    s->qcount--;
    return (int)n;
}

int net_socket_read_ready(net_socket_t *s) {
    if (!s || !s->used)
        return 0;
    net_poll_all();
    preempt_disable();
    int r;
    if (s->type == SOCK_STREAM_K)
        r = s->tcp_rx_count > 0 || s->tcp_state == TCP_STATE_CLOSED ||
            s->tcp_state == TCP_STATE_ERROR || s->peer_fin || s->rx_shut;
    else
        r = s->qcount > 0;
    preempt_enable();
    return r;
}

int net_socket_write_ready(net_socket_t *s) {
    if (!s || !s->used)
        return 0;
    preempt_disable();
    int r = 1;
    if (s->type == SOCK_STREAM_K)
        /* Also "writable" once a send can no longer block: the connection
         * failed or is gone (the send then reports why), as Linux tcp_poll
         * reports a finished non-blocking connect either way. */
        r = (s->was_connected && s->tcp && tcp_sndbuf(s->tcp) > 0) ||
            s->tcp_state == TCP_STATE_ERROR || s->tx_shut ||
            (s->was_connected && !s->tcp);
    preempt_enable();
    return r;
}

/* poll: a pending connection error is POLLERR (tcp_poll on sk_err). */
int net_socket_poll_err(net_socket_t *s) {
    return s && s->used && s->type == SOCK_STREAM_K &&
           s->tcp_state == TCP_STATE_ERROR && s->tcp_error;
}

/* getsockname (peer = 0) / getpeername (peer = 1).  An unbound socket
 * reports 0.0.0.0:0 like Linux's inet_getname; a peer name needs a connection
 * (UDP: connect() having set a default destination), else ENOTCONN. */
int net_socket_getname(net_socket_t *s, int peer, net_sockaddr_in_t *out) {
    if (!s || !s->used || !out)
        return -9;
    memset(out, 0, sizeof(*out));
    out->family = AF_INET_K;
    preempt_disable();
    int r = 0;
    if (peer) {
        if (!s->connected)
            r = -107;
        else {
            out->addr = ip4_addr_get_u32(ip_2_ip4(&s->remote_addr));
            out->port = bswap16(s->remote_port);
        }
    } else if (s->tcp) {
        out->addr = ip4_addr_get_u32(ip_2_ip4(&s->tcp->local_ip));
        out->port = bswap16(s->tcp->local_port);
    } else if (s->udp) {
        out->addr = ip4_addr_get_u32(ip_2_ip4(&s->udp->local_ip));
        out->port = bswap16(s->udp->local_port);
    }
    preempt_enable();
    return r;
}

static int socket_shutdown_locked(net_socket_t *s, int how) {
    if (!s || !s->used)
        return -9;
    if (s->type != SOCK_STREAM_K)
        return 0;
    if (!s->tcp)
        return -107;
    int shut_rx = (how == 0 || how == 2);
    int shut_tx = (how == 1 || how == 2);
    if ((shut_rx || s->rx_shut) && (shut_tx || s->tx_shut)) {
        /* Both directions now shut, in this call or across two.  For the raw
         * API that is tcp_close: the pcb may be freed on the spot, or later
         * without an err callback (see socket_detach_pcb), so it must not be
         * referenced again.  Keeping s->tcp made a later close() tcp_close
         * it a second time.
         *
         * Send the FIN first when the write side is still open.  tcp_close
         * from ESTABLISHED or CLOSE_WAIT with bytes the application never
         * read (they sit in our ring, so rcv_wnd is short) is lwIP's
         * abortive close: an RST, with the unsent and unacked data purged,
         * and the peer loses the reply it was sent.  shutdown() is an
         * orderly close on Linux; that is close()'s behaviour, not this.
         * Once the FIN is queued the pcb is in FIN_WAIT_1 or LAST_ACK, where
         * tcp_close only marks the receive side closed. */
        if (!s->tx_shut)
            tcp_shutdown(s->tcp, 0, 1);
        socket_detach_pcb(s, 0);
        s->tcp_state = TCP_STATE_CLOSED;
        s->rx_shut = s->tx_shut = 1;
        return 0;
    }
    err_t e = tcp_shutdown(s->tcp, shut_rx, shut_tx);
    if (e != ERR_OK)
        return -107;
    if (shut_rx)
        s->rx_shut = 1;
    if (shut_tx)
        s->tx_shut = 1;
    return 0;
}

/*
 * Public socket ops: lwIP is single-threaded and non-reentrant, while the
 * PIT tick can preempt a syscall mid-stack and schedule knetd (which also
 * drives lwIP).  Every op that touches lwIP runs under the preemption guard.
 * None of the _locked bodies sleep.
 */
int net_socket_create(int domain, int type, int protocol, net_socket_t **out) {
    preempt_disable();
    int r = socket_create_locked(domain, type, protocol, out);
    preempt_enable();
    return r;
}

void net_socket_release(net_socket_t *s) {
    preempt_disable();
    socket_release_locked(s);
    preempt_enable();
}

int net_socket_bind(net_socket_t *s, const net_sockaddr_in_t *addr) {
    preempt_disable();
    int r = socket_bind_locked(s, addr);
    preempt_enable();
    return r;
}

int net_socket_take_error(net_socket_t *s) {
    preempt_disable();
    int e = (s && s->used) ? take_error_locked(s) : 0;
    preempt_enable();
    return e;
}

int net_socket_sendto(net_socket_t *s, const void *buf, uint32_t len,
                      const net_sockaddr_in_t *addr, int flags) {
    /* TCP send BLOCKS while the send buffer is full, like recv and like a
     * blocking Linux socket: it returns once all of buf is queued, or with
     * the partial count when a signal or a connection error cuts it short.
     * Returning -EAGAIN on a full buffer made a blocking writer that pushes
     * faster than the peer acks see a spurious error mid-stream.  UDP never
     * waits (a datagram is sent or dropped whole). */
    preempt_disable();
    int pinned = s && s->used;
    if (pinned) s->refs++;   /* pinned across the sleep, as in recvfrom */
    preempt_enable();
    if (!pinned) return -9;

    const uint8_t *p = (const uint8_t *)buf;
    uint32_t done = 0;
    int r;
    for (;;) {
        preempt_disable();
        r = socket_sendto_locked(s, p ? p + done : p, len - done, addr);
        preempt_enable();
        if (s->type != SOCK_STREAM_K)
            break;
        if (r > 0) {
            done += (uint32_t)r;
            if (done >= len)
                break;
            continue;
        }
        if (r != -11)
            break;           /* error: reported unless something went out */
        if (flags & NET_MSG_DONTWAIT)
            break;           /* O_NONBLOCK/MSG_DONTWAIT: -EAGAIN or partial */
        if (current_proc && signal_interrupt_pending(current_proc)) {
            r = -4;          /* -EINTR */
            break;
        }
        /* The buffer drains as the peer acks, which arrives with a NIC
         * interrupt and wakes this sleep. */
        net_io_sleep(2);
    }
    net_socket_release(s);
    return done ? (int)done : r;
}

int net_socket_recvfrom(net_socket_t *s, void *buf, uint32_t len,
                        net_sockaddr_in_t *addr, int flags) {
    /* TCP recv BLOCKS until data/EOF (Linux default semantics) — the
     * io_activity sleep wakes instantly on NIC interrupts.  UDP stays
     * non-blocking (-EAGAIN): existing probes and the DNS resolver use
     * retry loops and some intentionally test for EAGAIN. */
    /* Pin the socket while we may sleep: a sibling thread sharing the fd
     * table can close() it meanwhile, and without our ref the slot would be
     * wiped and possibly reused by an unrelated socket under us. */
    preempt_disable();
    int pinned = s && s->used;
    if (pinned) s->refs++;
    preempt_enable();
    if (!pinned) return -9;

    /* O_NONBLOCK/MSG_DONTWAIT: -EAGAIN instead of waiting.  MSG_WAITALL: a
     * stream read waits until all of `len` has arrived, or EOF, an error or
     * a signal ends it early with the bytes so far (Linux tcp_recvmsg). */
    int peek = (flags & NET_MSG_PEEK) != 0;
    uint32_t done = 0;
    int idle_polls = 0;
    int r;
    for (;;) {
        preempt_disable();
        r = socket_recvfrom_locked(s, (uint8_t *)buf + done, len - done,
                                   addr, peek);
        preempt_enable();
        if (s->type != SOCK_STREAM_K) break;
        if (r > 0) {
            done += (uint32_t)r;
            if (!(flags & NET_MSG_WAITALL) || peek || done >= len) break;
            continue;
        }
        if (r != -11) break;
        if (flags & NET_MSG_DONTWAIT) break;
        if (current_proc && signal_interrupt_pending(current_proc)) {
            r = -4;      /* -EINTR */
            break;
        }
        /* Ring empty and still connected: periodically re-advertise our
         * (open) receive window with a forced ACK.  TCP window-update ACKs
         * are not retransmitted, so if one is dropped the peer can stall
         * forever believing our window is still 0.  This nudge recovers it. */
        if (++idle_polls >= 25) {        /* ~0.5s of waiting */
            idle_polls = 0;
            preempt_disable();
            if (s->tcp && s->tcp_state == TCP_STATE_CONNECTED) {
                s->tcp->flags |= TF_ACK_NOW;
                tcp_output(s->tcp);
            }
            preempt_enable();
        }
        net_io_sleep(2);
    }
    net_socket_release(s);
    return done ? (int)done : r;
}

int net_socket_shutdown(net_socket_t *s, int how) {
    preempt_disable();
    int r = socket_shutdown_locked(s, how);
    preempt_enable();
    return r;
}

