#include "socket.h"
#include "lwip_glue.h"
#include "net.h"
#include "firewall.h"
#include "xsock.h"
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
#include "lwip/ip6_addr.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/priv/tcp_priv.h"   /* TF_ACK_NOW for the window-update nudge */

/* Sleep until I/O activity (NIC IRQ etc.) or a short deadline. */
static void net_io_sleep(uint32_t ticks) {
    if (!current_proc) { yield(); return; }
    current_proc->wake_tick = pit_ticks() + ticks;
    sleep_on(&io_activity);
}

/* SO_RCVTIMEO/SO_SNDTIMEO: a wait of `ms` (0 = none) started at `start`
 * has run out.  The PIT runs at 100 Hz. */
static int timed_out(uint32_t ms, uint32_t start) {
    return ms && (uint32_t)(pit_ticks() - start) >= (ms + 9) / 10;
}

#define AF_INET_K      2
#define AF_INET6_K    10
#define SOCK_STREAM_K  1
#define SOCK_DGRAM_K   2
#define SOCK_RAW_K     3
#define AF_NETLINK_K  16
#define AF_PACKET_K   17
#define IPPROTO_TCP_K  6
#define IPPROTO_IP_K   0
#define IP_OPTIONS_K   4
#define IPPROTO_UDP_K 17
#define IPPROTO_ICMP_K 1
#define IPPROTO_ICMPV6_K 58

/* The table itself is small (~100 bytes a slot): the per-socket buffers, a
 * 64 KiB RX ring for a stream socket or a 6 KiB datagram queue for a UDP one,
 * come from the kernel heap when the socket is created and go back on its last
 * close.  Held in the slot they made the table 2.2 MiB of .bss at 32 entries,
 * and the kernel image must fit boot.asm's initial mapping (linker.ld). */
#define MAX_NET_SOCKETS 128
#define UDP_QUEUE_DEPTH 4
#define UDP_PACKET_MAX 1536
#define TCP_RX_SIZE 65536   /* per-socket RX ring (burst headroom) */
/* listen() backlog ceiling (Linux clamps to somaxconn).  Established
 * connections waiting for accept() and half-open ones together count against
 * it (lwIP's TCP_LISTEN_BACKLOG with tcp_backlog_delayed), and each queued one
 * holds a socket slot and a TCP pcb (MEMP_NUM_TCP_PCB). */
#define ACCEPTQ_MAX 32

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
    ip_addr_t addr;
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
    /* listen(): the listening pcb replaces s->tcp (which stays NULL, so the
     * connection paths see "no connection"), and accepted connections wait
     * in acceptq (ACCEPTQ_MAX slots from kmalloc), each holding the one
     * reference accept() hands to the new descriptor. */
    int listening;
    struct tcp_pcb *lpcb;
    net_socket_t **acceptq;
    int aq_head;
    int aq_count;
    int backlog;
    /* setsockopt state, also applied to every pcb the socket gets later
     * (an accepted connection inherits its listener's). */
    int opt_reuseaddr;
    int opt_keepalive;
    int opt_nodelay;
    uint32_t keep_idle_ms, keep_intvl_ms, keep_cnt;   /* 0 = lwIP default */
    int v6only;             /* IPV6_V6ONLY (AF_INET6): no v4-mapped peers */
    int linger_on;          /* SO_LINGER: l_onoff, l_linger (seconds) */
    int linger_s;
    uint32_t rcvtimeo_ms;   /* SO_RCVTIMEO: 0 = wait forever */
    uint32_t sndtimeo_ms;   /* SO_SNDTIMEO */
    /* AF_NETLINK, AF_PACKET and AF_INET SOCK_RAW (net/xsock.h): the
     * family's operations and state; NULL for lwIP TCP/UDP. */
    const xsock_ops_t *xops;
    void *xs;
};

static net_socket_t sockets[MAX_NET_SOCKETS];

static uint16_t bswap16(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

/* ── Socket names <-> lwIP addresses ─────────────────────────────────────
 * An AF_INET6 socket is dual-stack unless IPV6_V6ONLY: its pcb is of
 * IPADDR_TYPE_ANY, an IPv4 peer is named ::ffff:a.b.c.d, and connecting or
 * sending to such a name goes out over IPv4 (RFC 4291 2.5.5.2, RFC 3493 5.3).
 * Link-local addresses carry their zone (sin6_scope_id, lwIP's netif index,
 * which matches the interface index: lo 1, eth0 2); one without a scope id
 * is eth0's. */

static int is_v4mapped(const uint8_t *a) {
    static const uint8_t pfx[12] = { 0,0,0,0, 0,0,0,0, 0,0,0xff,0xff };
    return memcmp(a, pfx, 12) == 0;
}

static int is_zero16(const uint8_t *a) {
    for (int i = 0; i < 16; i++)
        if (a[i]) return 0;
    return 1;
}

/* A name from the caller into an lwIP address.  `bind` turns :: into the
 * wildcard of the socket's kind (IPADDR_TYPE_ANY when dual-stack). */
static int sa_to_ip(net_socket_t *s, const net_sockaddr_t *sa, ip_addr_t *ip,
                    int bind) {
    if (s->domain == AF_INET_K) {
        if (sa->family != AF_INET_K)
            return -97;                                  /* -EAFNOSUPPORT */
        ip_addr_set_ip4_u32(ip, sa->addr);
        return 0;
    }
    if (sa->family != AF_INET6_K)
        return -97;
    if (is_v4mapped(sa->addr6)) {
        if (s->v6only)
            return bind ? -22 : -101;                    /* EINVAL / ENETUNREACH */
        uint32_t a4;
        memcpy(&a4, sa->addr6 + 12, 4);
        ip_addr_set_ip4_u32(ip, a4);
        return 0;
    }
    if (is_zero16(sa->addr6) && bind) {
        if (s->v6only)
            ip_addr_copy(*ip, *IP6_ADDR_ANY);
        else
            ip_addr_copy(*ip, *IP_ANY_TYPE);
        return 0;
    }
    ip_addr_set_zero_ip6(ip);
    memcpy(ip_2_ip6(ip)->addr, sa->addr6, 16);
    ip6_addr_clear_zone(ip_2_ip6(ip));
    if (ip6_addr_islinklocal(ip_2_ip6(ip)) || ip6_addr_ismulticast_linklocal(ip_2_ip6(ip))) {
        uint32_t zone = sa->scope_id ? sa->scope_id : (uint32_t)net_lwip_eth_zone();
        ip6_addr_set_zone(ip_2_ip6(ip), (u8_t)zone);
    }
    return 0;
}

/* An lwIP address as the socket's kind of name. */
static void ip_to_sa(net_socket_t *s, const ip_addr_t *ip, uint16_t port_host,
                     net_sockaddr_t *out) {
    memset(out, 0, sizeof(*out));
    out->port = bswap16(port_host);
    if (s->domain == AF_INET_K) {
        out->family = AF_INET_K;
        if (ip && IP_IS_V4(ip))
            out->addr = ip4_addr_get_u32(ip_2_ip4(ip));
        return;
    }
    out->family = AF_INET6_K;
    if (!ip || IP_IS_ANY_TYPE_VAL(*ip))
        return;                                          /* :: */
    if (IP_IS_V4(ip)) {
        uint32_t a4 = ip4_addr_get_u32(ip_2_ip4(ip));
        if (a4 == 0)
            return;
        out->addr6[10] = out->addr6[11] = 0xff;
        memcpy(out->addr6 + 12, &a4, 4);
        return;
    }
    memcpy(out->addr6, ip_2_ip6(ip)->addr, 16);
    if (ip6_addr_has_zone(ip_2_ip6(ip)))
        out->scope_id = ip6_addr_zone(ip_2_ip6(ip));
}

/* A destination is reachable: IPv4 always (lwIP decides), IPv6 only on lo,
 * the link, eth0's prefixes, or via a router learnt from an RA.  lwIP would
 * otherwise send a global destination out of eth0 from its link-local
 * address and the connect would hang until it timed out, where Linux fails
 * at once with ENETUNREACH (no route) and Happy Eyeballs moves on to IPv4. */
static int ip_reachable(const ip_addr_t *ip) {
    if (!IP_IS_V6(ip))
        return 1;
    const ip6_addr_t *a = ip_2_ip6(ip);
    if (ip6_addr_isloopback(a) || ip6_addr_islinklocal(a) || ip6_addr_ismulticast(a))
        return 1;
    uint8_t r[16];
    if (net_lwip_ip6_router(r))
        return 1;
    return net_lwip_ip6_onlink((const uint8_t *)a->addr);
}

/* Linux's ENETUNREACH when IPv6 has no route; ok otherwise. */
static int route_check(const ip_addr_t *ip) {
    return ip_reachable(ip) ? 0 : -101;
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
    ip_addr_copy(pkt->addr, *addr);
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

/* The socket's options onto a TCP pcb (a new connection or the listener). */
static void tcp_apply_opts(net_socket_t *s, struct tcp_pcb *pcb) {
    if (!pcb)
        return;
    if (s->opt_reuseaddr) ip_set_option(pcb, SOF_REUSEADDR);
    else                  ip_reset_option(pcb, SOF_REUSEADDR);
    if (s->opt_keepalive) ip_set_option(pcb, SOF_KEEPALIVE);
    else                  ip_reset_option(pcb, SOF_KEEPALIVE);
    if (pcb->state == LISTEN)
        return;             /* a tcp_pcb_listen has no more fields */
    if (s->opt_nodelay) tcp_nagle_disable(pcb);
    else                tcp_nagle_enable(pcb);
    if (s->keep_idle_ms)  pcb->keep_idle = s->keep_idle_ms;
    if (s->keep_intvl_ms) pcb->keep_intvl = s->keep_intvl_ms;
    if (s->keep_cnt)      pcb->keep_cnt = s->keep_cnt;
}

void net_sockets_init(void) {
    memset(sockets, 0, sizeof(sockets));
}

static net_socket_t *socket_free_slot(void) {
    for (int i = 0; i < MAX_NET_SOCKETS; i++)
        if (!sockets[i].used)
            return &sockets[i];
    printk("[NET] socket: all %d sockets in use\n", MAX_NET_SOCKETS);
    return NULL;
}

static int xsocket_create_locked(int domain, int type, int protocol,
                                 net_socket_t **out) {
    net_socket_t *s = socket_free_slot();
    if (!s)
        return -24;
    const xsock_ops_t *ops = NULL;
    void *x = NULL;
    int r = domain == AF_NETLINK_K ? netlink_create(type, protocol, &ops, &x)
          : domain == AF_PACKET_K  ? packet_create(type, protocol, &ops, &x)
          : type == SOCK_DGRAM_K   ? ping_create(domain, &ops, &x)
          : domain == AF_INET6_K   ? rawip6_create(protocol, &ops, &x)
          : rawip_create(protocol, &ops, &x);
    if (r < 0)
        return r;
    memset(s, 0, sizeof(*s));
    s->used = 1;
    s->refs = 1;
    s->domain = domain;
    s->type = type;
    s->protocol = protocol;
    s->xops = ops;
    s->xs = x;
    *out = s;
    return 0;
}

static int socket_create_locked(int domain, int type, int protocol, net_socket_t **out) {
    if (domain == AF_NETLINK_K || domain == AF_PACKET_K ||
        (domain == AF_INET_K && type == SOCK_RAW_K))
        return xsocket_create_locked(domain, type, protocol, out);
    if (domain == AF_INET6_K && type == SOCK_RAW_K)
        return xsocket_create_locked(domain, type, protocol, out);
    /* ICMP "ping sockets" (SOCK_DGRAM, IPPROTO_ICMP / IPPROTO_ICMPV6). */
    if ((domain == AF_INET_K || domain == AF_INET6_K) && type == SOCK_DGRAM_K &&
        protocol == (domain == AF_INET_K ? IPPROTO_ICMP_K : IPPROTO_ICMPV6_K))
        return xsocket_create_locked(domain, type, protocol, out);
    if (domain != AF_INET_K && domain != AF_INET6_K)
        return -97;
    if (type != SOCK_DGRAM_K && type != SOCK_STREAM_K)
        return -94;
    if (type == SOCK_DGRAM_K && protocol != 0 && protocol != IPPROTO_UDP_K)
        return -93;
    if (type == SOCK_STREAM_K && protocol != 0 && protocol != IPPROTO_TCP_K)
        return -93;

    net_socket_t *s = socket_free_slot();
    if (!s)
        return -24;

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
    u8_t iptype = domain == AF_INET6_K ? IPADDR_TYPE_ANY : IPADDR_TYPE_V4;
    if (type == SOCK_DGRAM_K)
        upcb = udp_new_ip_type(iptype);
    else
        tpcb = tcp_new_ip_type(iptype);
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

static void socket_release_locked(net_socket_t *s);

/* Close the listening pcb (lwIP frees it at once; half-open connections lose
 * their listener and are dropped) and release every connection still waiting
 * for accept(): their pcbs are closed, so the peers see the connection end
 * (Linux resets them).  accept() then fails with EINVAL. */
static void listen_stop(net_socket_t *s) {
    if (s->lpcb) {
        tcp_arg(s->lpcb, NULL);
        tcp_accept(s->lpcb, NULL);
        tcp_close(s->lpcb);
        s->lpcb = NULL;
    }
    while (s->aq_count > 0) {
        net_socket_t *c = s->acceptq[s->aq_head];
        s->aq_head = (s->aq_head + 1) % ACCEPTQ_MAX;
        s->aq_count--;
        if (c->tcp)
            tcp_backlog_accepted(c->tcp);
        socket_release_locked(c);
    }
}

static void socket_release_locked(net_socket_t *s) {
    if (!s || !s->used)
        return;
    if (--s->refs > 0)
        return;
    if (s->xops) {
        s->xops->release(s->xs);
        memset(s, 0, sizeof(*s));
        return;
    }
    if (s->udp)
        udp_remove(s->udp);
    /* SO_LINGER on with a zero timeout: close() resets the connection
     * (Linux tcp_close -> tcp_disconnect), dropping unsent data. */
    if (s->tcp && s->linger_on && s->linger_s == 0) {
        struct tcp_pcb *pcb = s->tcp;
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_poll(pcb, NULL, 0);
        s->tcp = NULL;
        tcp_abort(pcb);
    }
    socket_detach_pcb(s, 0);
    listen_stop(s);
    /* No lwIP callback can reach s any more (udp_remove, and the detach
     * cleared the TCP pcb's arg), so the buffers can go. */
    kfree(s->tcp_rx);
    kfree(s->queue);
    kfree(s->acceptq);
    memset(s, 0, sizeof(*s));
}

static int socket_bind_locked(net_socket_t *s, const net_sockaddr_in_t *addr) {
    if (!s || !s->used || !addr)
        return -9;
    /* Linux inet_bind: a socket binds once (EINVAL after that, also once
     * it listens or connected), and only to an address of this host. */
    if (s->type == SOCK_STREAM_K &&
        (!s->tcp || s->tcp->local_port != 0 || s->tcp->state != CLOSED))
        return -22;
    if (s->type == SOCK_DGRAM_K && s->udp->local_port != 0)
        return -22;
    ip_addr_t ip;
    int cr = sa_to_ip(s, addr, &ip, 1);
    if (cr < 0)
        return cr;
    if (IP_IS_V4_VAL(ip) ? !net_lwip_addr_is_local(ip4_addr_get_u32(ip_2_ip4(&ip)))
        : IP_IS_V6_VAL(ip) && !net_lwip_addr6_is_local((const uint8_t *)ip_2_ip6(&ip)->addr))
        return -99;                                      /* -EADDRNOTAVAIL */
    err_t e = s->type == SOCK_DGRAM_K
        ? udp_bind(s->udp, &ip, bswap16(addr->port))
        : tcp_bind(s->tcp, &ip, bswap16(addr->port));
    if (e == ERR_OK) return 0;
    return e == ERR_USE ? -98 : -22;                     /* -EADDRINUSE */
}

/* lwIP accept callback: a connection to a listening socket completed its
 * handshake.  It becomes a socket of its own at once (so data the peer sends
 * before accept() lands in its ring) and waits in the listener's queue,
 * counted against the backlog until accept() takes it.  Returning an error
 * makes lwIP abort the new pcb. */
static err_t tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    net_socket_t *ls = (net_socket_t *)arg;
    if (!ls || !ls->listening || !newpcb || err != ERR_OK)
        return ERR_VAL;
    if (ls->aq_count >= ACCEPTQ_MAX)
        return ERR_MEM;
    net_socket_t *c = socket_free_slot();
    if (!c)
        return ERR_MEM;
    uint8_t *ring = (uint8_t *)kmalloc(TCP_RX_SIZE);
    if (!ring)
        return ERR_MEM;
    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->refs = 1;
    c->domain = ls->domain;
    c->v6only = ls->v6only;
    c->linger_on = ls->linger_on;
    c->linger_s = ls->linger_s;
    c->type = SOCK_STREAM_K;
    c->protocol = IPPROTO_TCP_K;
    c->tcp = newpcb;
    c->tcp_rx = ring;
    c->tcp_state = TCP_STATE_CONNECTED;
    c->connected = 1;
    c->was_connected = 1;
    ip_addr_copy(c->remote_addr, newpcb->remote_ip);
    c->remote_port = newpcb->remote_port;
    c->opt_reuseaddr = ls->opt_reuseaddr;
    c->opt_keepalive = ls->opt_keepalive;
    c->opt_nodelay = ls->opt_nodelay;
    c->keep_idle_ms = ls->keep_idle_ms;
    c->keep_intvl_ms = ls->keep_intvl_ms;
    c->keep_cnt = ls->keep_cnt;
    c->rcvtimeo_ms = ls->rcvtimeo_ms;
    c->sndtimeo_ms = ls->sndtimeo_ms;
    tcp_arg(newpcb, c);
    tcp_recv(newpcb, tcp_recv_cb);
    tcp_err(newpcb, tcp_err_cb);
    tcp_poll(newpcb, tcp_poll_cb, 2);
    tcp_apply_opts(c, newpcb);
    tcp_backlog_delayed(newpcb);
    ls->acceptq[(ls->aq_head + ls->aq_count) % ACCEPTQ_MAX] = c;
    ls->aq_count++;
    return ERR_OK;
}

static int socket_listen_locked(net_socket_t *s, int backlog) {
    if (!s || !s->used)
        return -9;
    if (s->type != SOCK_STREAM_K)
        return -95;                                      /* -EOPNOTSUPP */
    if (backlog < 1) backlog = 1;
    if (backlog > ACCEPTQ_MAX) backlog = ACCEPTQ_MAX;
    if (s->listening) {
        /* listen() again only changes the backlog (Linux inet_listen). */
        if (!s->lpcb)
            return -22;
        s->backlog = backlog;
        tcp_backlog_set(s->lpcb, (u8_t)backlog);
        return 0;
    }
    if (!s->tcp || s->tcp->state != CLOSED || s->was_connected)
        return -22;
    net_socket_t **q = (net_socket_t **)kmalloc(ACCEPTQ_MAX * sizeof(*q));
    if (!q)
        return -12;
    /* An unbound socket gets an ephemeral port (Linux inet_autobind). */
    const ip_addr_t *any = s->domain == AF_INET_K ? IP4_ADDR_ANY
                         : s->v6only ? IP6_ADDR_ANY : IP_ANY_TYPE;
    if (s->tcp->local_port == 0 && tcp_bind(s->tcp, any, 0) != ERR_OK) {
        kfree(q);
        return -98;
    }
    err_t e = ERR_OK;
    struct tcp_pcb *l = tcp_listen_with_backlog_and_err(s->tcp, (u8_t)backlog, &e);
    if (!l) {
        kfree(q);
        return e == ERR_USE ? -98 : -12;
    }
    /* lwIP freed the original pcb; the listener carries s as its arg. */
    s->tcp = NULL;
    s->lpcb = l;
    s->acceptq = q;
    s->aq_head = s->aq_count = 0;
    s->backlog = backlog;
    s->listening = 1;
    tcp_arg(l, s);
    tcp_accept(l, tcp_accept_cb);
    tcp_apply_opts(s, l);
    return 0;
}

/* One accept attempt: the next queued connection (its reference passes to
 * the caller), -EAGAIN when none waits, -EINVAL when not listening. */
static int socket_accept_locked(net_socket_t *s, net_socket_t **out) {
    if (!s->listening || !s->lpcb)
        return -22;
    if (s->aq_count == 0)
        return -11;
    net_socket_t *c = s->acceptq[s->aq_head];
    s->aq_head = (s->aq_head + 1) % ACCEPTQ_MAX;
    s->aq_count--;
    if (c->tcp)
        tcp_backlog_accepted(c->tcp);   /* frees a backlog place */
    *out = c;
    return 0;
}

/* A blocking connect()'s wait for its handshake (Linux inet_wait_for_connect):
 * 0 once established, the socket error if it failed, -ETIMEDOUT after 3 s, or
 * -EINTR (restart per SA_RESTART) when a signal arrives.  The handshake
 * carries on either way; a later blocking connect() waits for it again.
 * Pinned across the sleeps (see net_socket_recvfrom). */
static int connect_wait(net_socket_t *s) {
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
        /* A signal interrupts the wait (sock_intr_errno: -ERESTARTSYS,
         * so EINTR or a restart per SA_RESTART). */
        if (current_proc && signal_interrupt_pending(current_proc)) {
            r = -4;
            break;
        }
        net_io_sleep(2);
    }
    net_socket_release(s);
    return r;
}

int net_socket_connect(net_socket_t *s, const net_sockaddr_in_t *addr,
                       int nonblock) {
    if (!s || !s->used || !addr)
        return -9;
    ip_addr_t dst;
    int cr = sa_to_ip(s, addr, &dst, 0);
    if (cr < 0)
        return cr;
    /* Egress firewall: block outbound by remote ip/port if a rule matches
     * (its rules are IPv4). */
    if (IP_IS_V4_VAL(dst)) {
        int proto = (s->type == SOCK_DGRAM_K) ? FW_UDP : FW_TCP;
        int fr = firewall_check(FW_OUT, proto, ip4_addr_get_u32(ip_2_ip4(&dst)),
                                bswap16(addr->port));
        if (fr < 0) return fr;
    }
    if ((cr = route_check(&dst)) < 0)
        return cr;
    if (s->type == SOCK_DGRAM_K) {
        ip_addr_copy(s->remote_addr, dst);
        s->remote_port = bswap16(addr->port);
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
    /* A handshake already under way: -EALREADY for a non-blocking socket,
     * but a blocking one waits for it (__inet_stream_connect on
     * SS_CONNECTING with a timeout) — which is also what a connect()
     * restarted after a signal (SA_RESTART) must do. */
    if (st == TCP_STATE_CONNECTING)
        return nonblock ? -114 : connect_wait(s);         /* -EALREADY */
    if (was) return -106;                                 /* -EISCONN */
    /* A failed non-blocking attempt not yet collected with SO_ERROR reports
     * its error here (inet_stream_connect -> sock_error). */
    if (pending) return pending;
    if (!have_pcb) return -22;      /* a failed attempt released the pcb */

    /* Only a new attempt takes the address: one already under way (or
     * done) keeps its own peer, whatever a repeated connect() names. */
    ip_addr_copy(s->remote_addr, dst);
    s->remote_port = bswap16(addr->port);
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
    return connect_wait(s);
}

int net_socket_domain(net_socket_t *s) {
    return s && s->used ? s->domain : AF_INET_K;
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
        ip_addr_t ip;
        int cr = sa_to_ip(s, addr, &ip, 0);
        if (cr == 0)
            cr = route_check(&ip);
        if (cr < 0) {
            pbuf_free(p);
            return cr;
        }
        if (IP_IS_V4_VAL(ip) &&
            firewall_check(FW_OUT, FW_UDP, ip4_addr_get_u32(ip_2_ip4(&ip)),
                           bswap16(addr->port)) < 0) {
            pbuf_free(p);
            return -13;   /* -EACCES */
        }
        e = udp_sendto(s->udp, p, &ip, bswap16(addr->port));
    } else if (s->connected) {
        e = udp_send(s->udp, p);
    } else {
        pbuf_free(p);
        return -89;
    }

    pbuf_free(p);
    net_poll_all();
    if (e == ERR_VAL)
        return -22;       /* an IPv4 name on a v6-only socket or the like */
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
        if (addr)
            ip_to_sa(s, &pkt->addr, pkt->port, addr);
        return (int)n;
    }
    uint32_t n = pkt->len;
    if (n > len)
        n = len;
    memcpy(buf, pkt->data, n);

    if (addr)
        ip_to_sa(s, &pkt->addr, pkt->port, addr);

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
    if (s->xops)
        r = s->xops->read_ready(s->xs);
    else if (s->listening)
        r = s->aq_count > 0 || !s->lpcb;    /* accept() would not block */
    else if (s->type == SOCK_STREAM_K)
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
    if (s->listening)
        r = 0;
    else if (s->type == SOCK_STREAM_K)
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
    ip_to_sa(s, NULL, 0, out);
    preempt_disable();
    int r = 0;
    if (peer) {
        if (!s->connected)
            r = -107;
        else
            ip_to_sa(s, &s->remote_addr, s->remote_port, out);
    } else if (s->tcp || s->lpcb) {
        struct tcp_pcb *pcb = s->tcp ? s->tcp : s->lpcb;
        ip_to_sa(s, &pcb->local_ip, pcb->local_port, out);
    } else if (s->udp) {
        ip_to_sa(s, &s->udp->local_ip, s->udp->local_port, out);
    }
    preempt_enable();
    return r;
}

static int socket_shutdown_locked(net_socket_t *s, int how) {
    if (!s || !s->used)
        return -9;
    if (s->type != SOCK_STREAM_K)
        return 0;
    if (s->listening) {
        /* Linux inet_shutdown: shutting a listener's receive side stops it
         * (a waiting accept() returns EINVAL); SHUT_WR alone does nothing. */
        if (how == 0 || how == 2)
            listen_stop(s);
        return 0;
    }
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
    if (s && s->used && s->xops)
        return net_socket_xsendto(s, buf, len, addr, addr ? sizeof(*addr) : 0,
                                  flags);
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
    uint32_t start = pit_ticks();
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
        if (timed_out(s->sndtimeo_ms, start))
            break;           /* SO_SNDTIMEO: -EAGAIN or partial */
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
    if (s && s->used && s->xops) {
        uint32_t alen = sizeof(*addr);
        uint8_t abuf[128];
        int r = net_socket_xrecvfrom(s, buf, len, addr ? abuf : NULL, &alen,
                                     flags);
        if (addr) memcpy(addr, abuf, sizeof(*addr));
        return r > (int)len ? (int)len : r;
    }
    /* recv BLOCKS until data/EOF (TCP) or a datagram (UDP), as on Linux:
     * the io_activity sleep wakes instantly on NIC interrupts.  O_NONBLOCK/
     * MSG_DONTWAIT make it -EAGAIN instead, and SO_RCVTIMEO bounds the wait
     * (then -EAGAIN too). */
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
    uint32_t start = pit_ticks();
    int idle_polls = 0;
    int r;
    for (;;) {
        preempt_disable();
        r = socket_recvfrom_locked(s, (uint8_t *)buf + done, len - done,
                                   addr, peek);
        preempt_enable();
        if (s->type != SOCK_STREAM_K && r != -11) break;   /* one datagram */
        if (r > 0) {
            done += (uint32_t)r;
            if (!(flags & NET_MSG_WAITALL) || peek || done >= len) break;
            continue;
        }
        if (r != -11) break;
        if (flags & NET_MSG_DONTWAIT) break;
        if (timed_out(s->rcvtimeo_ms, start)) break;
        if (current_proc && signal_interrupt_pending(current_proc)) {
            r = -4;      /* -EINTR */
            break;
        }
        /* Ring empty and still connected: periodically re-advertise our
         * (open) receive window with a forced ACK.  TCP window-update ACKs
         * are not retransmitted, so if one is dropped the peer can stall
         * forever believing our window is still 0.  This nudge recovers it. */
        if (s->type == SOCK_STREAM_K && ++idle_polls >= 25) {   /* ~0.5s */
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


int net_socket_listen(net_socket_t *s, int backlog) {
    preempt_disable();
    int r = socket_listen_locked(s, backlog);
    preempt_enable();
    return r;
}

int net_socket_is_listening(net_socket_t *s) {
    return s && s->used && s->listening;
}

int net_socket_accept(net_socket_t *s, net_socket_t **out, int nonblock) {
    /* Blocks until a connection is queued (pinned across the sleeps, as in
     * recvfrom); -EAGAIN for O_NONBLOCK or once SO_RCVTIMEO runs out, -EINTR
     * on a signal. */
    preempt_disable();
    int pinned = s && s->used;
    if (pinned) s->refs++;
    preempt_enable();
    if (!pinned) return -9;
    if (s->type != SOCK_STREAM_K) {
        net_socket_release(s);
        return -95;                                      /* -EOPNOTSUPP */
    }
    uint32_t start = pit_ticks();
    int r;
    for (;;) {
        net_poll_all();
        preempt_disable();
        r = socket_accept_locked(s, out);
        preempt_enable();
        if (r != -11 || nonblock || timed_out(s->rcvtimeo_ms, start))
            break;
        if (current_proc && signal_interrupt_pending(current_proc)) {
            r = -4;
            break;
        }
        net_io_sleep(2);
    }
    net_socket_release(s);
    return r;
}

/* Socket options (Linux values).  Timeouts come as struct timeval: 8 bytes
 * for SO_RCVTIMEO_OLD/SO_SNDTIMEO_OLD (20/21, 32-bit time_t) or 16 for the
 * _NEW (66/67, 64-bit time_t; musl on i386 tries these first). */
#define SOL_SOCKET_K        1
#define SO_REUSEADDR_K      2
#define SO_TYPE_K           3
#define SO_ERROR_K          4
#define SO_SNDBUF_K         7
#define SO_RCVBUF_K         8
#define SO_KEEPALIVE_K      9
#define SO_RCVTIMEO_OLD_K   20
#define SO_SNDTIMEO_OLD_K   21
#define SO_ACCEPTCONN_K     30
#define SO_PROTOCOL_K       38
#define SO_DOMAIN_K         39
#define SO_RCVTIMEO_NEW_K   66
#define SO_SNDTIMEO_NEW_K   67
#define SO_LINGER_K         13
#define IP_TTL_K            2
#define IPPROTO_IPV6_K      41
#define IPV6_UNICAST_HOPS_K 16
#define IPV6_V6ONLY_K       26
#define TCP_NODELAY_K       1
#define TCP_KEEPIDLE_K      4
#define TCP_KEEPINTVL_K     5
#define TCP_KEEPCNT_K       6

static int timeval_to_ms(const void *val, uint32_t len, int wide, uint32_t *ms) {
    int64_t sec, usec;
    if (wide) {
        if (len < 16) return -22;
        sec = ((const int64_t *)val)[0];
        usec = ((const int64_t *)val)[1];
    } else {
        if (len < 8) return -22;
        sec = ((const int32_t *)val)[0];
        usec = ((const int32_t *)val)[1];
    }
    if (usec < 0 || usec >= 1000000) return -33;          /* -EDOM */
    if (sec < 0) { *ms = 0; return 0; }                   /* Linux: no timeout */
    if (sec > 0x7FFFFFFF / 1000) sec = 0x7FFFFFFF / 1000;
    int64_t t = sec * 1000 + ((uint32_t)usec + 999) / 1000;
    *ms = t > 0x7FFFFFFF ? 0x7FFFFFFF : (uint32_t)t;
    return 0;
}

static uint32_t timeval_from_ms(uint32_t ms, int wide, void *val) {
    if (wide) {
        ((int64_t *)val)[0] = (int64_t)(ms / 1000);
        ((int64_t *)val)[1] = (ms % 1000) * 1000;
        return 16;
    }
    ((int32_t *)val)[0] = (int32_t)(ms / 1000);
    ((int32_t *)val)[1] = (int32_t)((ms % 1000) * 1000);
    return 8;
}

static void socket_apply_opts_locked(net_socket_t *s) {
    tcp_apply_opts(s, s->tcp);
    tcp_apply_opts(s, s->lpcb);
    if (s->udp) {
        if (s->opt_reuseaddr) ip_set_option(s->udp, SOF_REUSEADDR);
        else                  ip_reset_option(s->udp, SOF_REUSEADDR);
    }
}

/* setsockopt.  Options this stack has no use for are accepted and ignored,
 * as before (SO_SNDBUF, SO_LINGER, IP_TOS, ...). */
int net_socket_setopt(net_socket_t *s, int level, int name,
                      const void *val, uint32_t len) {
    if (!s || !s->used)
        return -9;
    int iv = 0;
    int is_tv = level == SOL_SOCKET_K &&
        (name == SO_RCVTIMEO_OLD_K || name == SO_SNDTIMEO_OLD_K ||
         name == SO_RCVTIMEO_NEW_K || name == SO_SNDTIMEO_NEW_K);
    if (s->xops && !is_tv) {
        preempt_disable();
        int r = s->xops->setopt(s->xs, level, name, val, len);
        preempt_enable();
        return r == 1 ? 0 : r;    /* the rest are accepted and ignored */
    }
    if (is_tv) {
        uint32_t ms;
        int wide = name == SO_RCVTIMEO_NEW_K || name == SO_SNDTIMEO_NEW_K;
        int r = timeval_to_ms(val, len, wide, &ms);
        if (r < 0) return r;
        if (name == SO_RCVTIMEO_OLD_K || name == SO_RCVTIMEO_NEW_K)
            s->rcvtimeo_ms = ms;
        else
            s->sndtimeo_ms = ms;
        return 0;
    }
    if (len >= 4)
        iv = *(const int32_t *)val;
    else if (len >= 1)
        iv = *(const uint8_t *)val;
    preempt_disable();
    int r = 0;
    if (level == SOL_SOCKET_K && name == SO_LINGER_K) {
        /* struct linger { int l_onoff; int l_linger; } */
        if (len < 8)
            r = -22;
        else {
            s->linger_on = ((const int32_t *)val)[0] != 0;
            s->linger_s = ((const int32_t *)val)[1] < 0 ? 0 : ((const int32_t *)val)[1];
        }
    } else if (level == IPPROTO_IPV6_K && name == IPV6_V6ONLY_K) {
        /* Linux: only on an AF_INET6 socket, and only before it binds. */
        int bound = (s->tcp && s->tcp->local_port) || s->lpcb ||
                    (s->udp && s->udp->local_port) || s->was_connected;
        if (s->domain != AF_INET6_K)
            r = -92;                                     /* -ENOPROTOOPT */
        else if (len < 4)
            r = -22;
        else if (bound)
            r = -22;
        else {
            s->v6only = iv != 0;
            u8_t t = s->v6only ? IPADDR_TYPE_V6 : IPADDR_TYPE_ANY;
            if (s->tcp) {
                IP_SET_TYPE_VAL(s->tcp->local_ip, t);
                IP_SET_TYPE_VAL(s->tcp->remote_ip, t);
            }
            if (s->udp) {
                IP_SET_TYPE_VAL(s->udp->local_ip, t);
                IP_SET_TYPE_VAL(s->udp->remote_ip, t);
            }
        }
    } else if (level == SOL_SOCKET_K && name == SO_REUSEADDR_K) {
        s->opt_reuseaddr = iv != 0;
    } else if (level == SOL_SOCKET_K && name == SO_KEEPALIVE_K) {
        s->opt_keepalive = iv != 0;
    } else if (level == IPPROTO_TCP_K && s->type == SOCK_STREAM_K) {
        if (len < 4)
            r = -22;
        else if (name == TCP_NODELAY_K)
            s->opt_nodelay = iv != 0;
        else if (name == TCP_KEEPIDLE_K || name == TCP_KEEPINTVL_K ||
                 name == TCP_KEEPCNT_K) {
            if (iv < 1 || iv > 32767)
                r = -22;
            else if (name == TCP_KEEPIDLE_K)  s->keep_idle_ms = (uint32_t)iv * 1000;
            else if (name == TCP_KEEPINTVL_K) s->keep_intvl_ms = (uint32_t)iv * 1000;
            else                              s->keep_cnt = (uint32_t)iv;
        }
    }
    if (r == 0)
        socket_apply_opts_locked(s);
    preempt_enable();
    return r;
}

/* getsockopt: writes up to *len bytes of the value, sets *len to its size.
 * 1 when the option is unknown here (the caller reports 0, as before). */
int net_socket_getopt(net_socket_t *s, int level, int name,
                      void *val, uint32_t *len) {
    if (!s || !s->used)
        return -9;
    uint8_t buf[16];
    uint32_t n = 4;
    int32_t v = 0;
    if (s->xops) {
        preempt_disable();
        int r = s->xops->getopt(s->xs, level, name, val, len);
        preempt_enable();
        if (r != 1)
            return r;
        if (level == SOL_SOCKET_K && name == SO_ERROR_K) {
            v = 0;
            goto out;
        }
    }
    if (level == SOL_SOCKET_K) {
        switch (name) {
        case SO_REUSEADDR_K:  v = s->opt_reuseaddr; break;
        case SO_KEEPALIVE_K:  v = s->opt_keepalive; break;
        case SO_TYPE_K:       v = s->type; break;
        case SO_ERROR_K:      v = -net_socket_take_error(s); break;
        case SO_SNDBUF_K:
        case SO_RCVBUF_K:     v = 65536; break;
        case SO_ACCEPTCONN_K: v = s->listening; break;
        case SO_LINGER_K: {
            int32_t l[2] = { s->linger_on, s->linger_s };
            memcpy(buf, l, 8);
            n = 8;
            break;
        }
        case SO_PROTOCOL_K:   v = s->protocol; break;
        case SO_DOMAIN_K:     v = s->domain; break;
        case SO_RCVTIMEO_OLD_K: case SO_RCVTIMEO_NEW_K:
            n = timeval_from_ms(s->rcvtimeo_ms, name == SO_RCVTIMEO_NEW_K, buf);
            break;
        case SO_SNDTIMEO_OLD_K: case SO_SNDTIMEO_NEW_K:
            n = timeval_from_ms(s->sndtimeo_ms, name == SO_SNDTIMEO_NEW_K, buf);
            break;
        default: return 1;
        }
    } else if (level == IPPROTO_TCP_K && s->type == SOCK_STREAM_K) {
        switch (name) {
        case TCP_NODELAY_K:   v = s->opt_nodelay; break;
        case TCP_KEEPIDLE_K:  v = s->keep_idle_ms ? (int32_t)(s->keep_idle_ms / 1000) : 7200; break;
        case TCP_KEEPINTVL_K: v = s->keep_intvl_ms ? (int32_t)(s->keep_intvl_ms / 1000) : 75; break;
        case TCP_KEEPCNT_K:   v = s->keep_cnt ? (int32_t)s->keep_cnt : 9; break;
        default: return 1;
        }
    } else if (level == IPPROTO_IPV6_K && name == IPV6_V6ONLY_K &&
               s->domain == AF_INET6_K) {
        v = s->v6only;
    } else if ((level == IPPROTO_IPV6_K && name == IPV6_UNICAST_HOPS_K) ||
               (level == IPPROTO_IP_K && name == IP_TTL_K)) {
        v = 64;                                          /* lwIP's default TTL */
    } else if (level == IPPROTO_IP_K && name == IP_OPTIONS_K) {
        /* No IP options are ever kept: an empty value.  Four zero bytes
         * read as options, and sshd-session drops a connection that carries
         * any (it refuses source routing). */
        *len = 0;
        return 0;
    } else {
        return 1;
    }
out:
    if (n == 4)
        memcpy(buf, &v, 4);
    uint32_t c = *len < n ? *len : n;
    memcpy(val, buf, c);
    *len = c;
    return 0;
}

/* ── AF_NETLINK / AF_PACKET / raw IP (net/xsock.h) ──────────────────────── */

int net_socket_is_x(net_socket_t *s) {
    return s && s->used && s->xops != NULL;
}

int net_socket_xbind(net_socket_t *s, const void *addr, uint32_t alen) {
    if (!net_socket_is_x(s)) return -9;
    preempt_disable();
    int r = s->xops->bind(s->xs, addr, alen);
    preempt_enable();
    return r;
}

int net_socket_xconnect(net_socket_t *s, const void *addr, uint32_t alen) {
    if (!net_socket_is_x(s)) return -9;
    preempt_disable();
    int r = s->xops->connect(s->xs, addr, alen);
    preempt_enable();
    return r;
}

int net_socket_xgetname(net_socket_t *s, int peer, void *addr, uint32_t *alen) {
    if (!net_socket_is_x(s)) return -9;
    preempt_disable();
    int r = s->xops->getname(s->xs, peer, addr, alen);
    preempt_enable();
    return r;
}

/* A datagram never waits for room: it goes out or fails whole. */
int net_socket_xsendto(net_socket_t *s, const void *buf, uint32_t len,
                       const void *addr, uint32_t alen, int flags) {
    (void)flags;
    if (!net_socket_is_x(s)) return -9;
    preempt_disable();
    int r = s->xops->send(s->xs, buf, len, addr, alen);
    preempt_enable();
    return r;
}

/* The next datagram, waiting for one unless MSG_DONTWAIT/O_NONBLOCK, up to
 * SO_RCVTIMEO.  Returns the datagram's full size, which may exceed len. */
int net_socket_xrecvfrom(net_socket_t *s, void *buf, uint32_t len,
                         void *addr, uint32_t *alen, int flags) {
    preempt_disable();
    int pinned = net_socket_is_x(s);
    if (pinned) s->refs++;
    preempt_enable();
    if (!pinned) return -9;
    uint32_t start = pit_ticks();
    int r;
    for (;;) {
        preempt_disable();
        r = s->xops->recv(s->xs, buf, len, addr, alen,
                          (flags & NET_MSG_PEEK) != 0);
        preempt_enable();
        if (r != -11) break;
        if (flags & NET_MSG_DONTWAIT) break;
        if (timed_out(s->rcvtimeo_ms, start)) break;
        if (current_proc && signal_interrupt_pending(current_proc)) {
            r = -4;
            break;
        }
        net_poll_all();
        net_io_sleep(2);
    }
    net_socket_release(s);
    return r;
}
