#include "socket.h"
#include "lwip_glue.h"
#include "net.h"
#include "firewall.h"
#include "../lib/string.h"
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

#define MAX_NET_SOCKETS 32
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
    int tcp_state;
    int tcp_error;
    uint8_t tcp_rx[TCP_RX_SIZE];
    uint32_t tcp_rx_head;
    uint32_t tcp_rx_tail;
    uint32_t tcp_rx_count;
    ip_addr_t remote_addr;
    uint16_t remote_port;
    udp_packet_t queue[UDP_QUEUE_DEPTH];
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
        s->tcp_error = 0;
    } else {
        s->tcp_state = TCP_STATE_ERROR;
        s->tcp_error = -101;
    }
    return ERR_OK;
}

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
                         err_t err) {
    net_socket_t *s = (net_socket_t *)arg;
    if (!s)
        return ERR_ARG;
    if (!p) {
        s->tcp_state = TCP_STATE_CLOSED;
        s->connected = 0;
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
    s->tcp_state = TCP_STATE_ERROR;
    s->tcp_error = (err == ERR_ABRT) ? -104 : -101;
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

    struct udp_pcb *upcb = NULL;
    struct tcp_pcb *tpcb = NULL;
    if (type == SOCK_DGRAM_K)
        upcb = udp_new_ip_type(IPADDR_TYPE_V4);
    else
        tpcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!upcb && !tpcb)
        return -12;

    for (int i = 0; i < MAX_NET_SOCKETS; i++) {
        if (!sockets[i].used) {
            net_socket_t *s = &sockets[i];
            memset(s, 0, sizeof(*s));
            s->used = 1;
            s->refs = 1;
            s->domain = domain;
            s->type = type;
            s->protocol = protocol ? protocol :
                          (type == SOCK_DGRAM_K ? IPPROTO_UDP_K : IPPROTO_TCP_K);
            s->udp = upcb;
            s->tcp = tpcb;
            if (s->udp)
                udp_recv(s->udp, udp_recv_cb, s);
            if (s->tcp) {
                tcp_arg(s->tcp, s);
                tcp_recv(s->tcp, tcp_recv_cb);
                tcp_err(s->tcp, tcp_err_cb);
                tcp_poll(s->tcp, tcp_poll_cb, 2);
                s->tcp_state = TCP_STATE_NONE;
            }
            *out = s;
            return 0;
        }
    }

    if (upcb)
        udp_remove(upcb);
    if (tpcb)
        tcp_abort(tpcb);
    return -24;
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
    if (s->tcp) {
        tcp_arg(s->tcp, NULL);
        tcp_recv(s->tcp, NULL);
        tcp_err(s->tcp, NULL);
        tcp_poll(s->tcp, NULL, 0);
        if (tcp_close(s->tcp) != ERR_OK)
            tcp_abort(s->tcp);
    }
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

int net_socket_connect(net_socket_t *s, const net_sockaddr_in_t *addr) {
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
    uint32_t start = pit_ticks();
    while ((uint32_t)(pit_ticks() - start) < 300U) {
        net_poll_all();
        if (s->tcp_state == TCP_STATE_CONNECTED)
            return 0;
        if (s->tcp_state == TCP_STATE_ERROR)
            return s->tcp_error ? s->tcp_error : -101;
        net_io_sleep(2);
    }
    return -110;
}

static int socket_sendto_locked(net_socket_t *s, const void *buf, uint32_t len,
                                const net_sockaddr_in_t *addr) {
    if (!s || !s->used || !buf)
        return -9;
    if (s->type == SOCK_STREAM_K) {
        if (!s->connected || !s->tcp)
            return -107;
        uint32_t left = len;
        const uint8_t *p = (const uint8_t *)buf;
        uint32_t sent = 0;
        while (left > 0) {
            uint16_t chunk = (uint16_t)left;
            uint16_t avail = tcp_sndbuf(s->tcp);
            if (avail == 0)
                break;
            if (chunk > avail)
                chunk = avail;
            if (chunk > 1460)
                chunk = 1460;
            err_t e = tcp_write(s->tcp, p, chunk, TCP_WRITE_FLAG_COPY);
            if (e != ERR_OK)
                break;
            tcp_output(s->tcp);
            p += chunk;
            left -= chunk;
            sent += chunk;
            net_poll_all();
        }
        return sent ? (int)sent : -11;
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

static int socket_recvfrom_locked(net_socket_t *s, void *buf, uint32_t len,
                                  net_sockaddr_in_t *addr) {
    if (!s || !s->used || !buf)
        return -9;

    net_poll_all();
    if (s->type == SOCK_STREAM_K) {
        if (s->tcp_rx_count == 0) {
            if (s->tcp_state == TCP_STATE_CLOSED)
                return 0;
            if (s->tcp_state == TCP_STATE_ERROR)
                return s->tcp_error ? s->tcp_error : -104;
            return -11;
        }
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
            s->tcp_state == TCP_STATE_ERROR;
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
        r = s->connected && s->tcp && tcp_sndbuf(s->tcp) > 0;
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
    err_t e = tcp_shutdown(s->tcp, shut_rx, shut_tx);
    return e == ERR_OK ? 0 : -107;
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

int net_socket_sendto(net_socket_t *s, const void *buf, uint32_t len,
                      const net_sockaddr_in_t *addr) {
    preempt_disable();
    int r = socket_sendto_locked(s, buf, len, addr);
    preempt_enable();
    return r;
}

int net_socket_recvfrom(net_socket_t *s, void *buf, uint32_t len,
                        net_sockaddr_in_t *addr) {
    /* TCP recv BLOCKS until data/EOF (Linux default semantics) — the
     * io_activity sleep wakes instantly on NIC interrupts.  UDP stays
     * non-blocking (-EAGAIN): existing probes and the DNS resolver use
     * retry loops and some intentionally test for EAGAIN. */
    int idle_polls = 0;
    for (;;) {
        int r;
        preempt_disable();
        r = socket_recvfrom_locked(s, buf, len, addr);
        preempt_enable();
        if (r != -11 || s->type != SOCK_STREAM_K) return r;
        if (current_proc && signal_interrupt_pending(current_proc))
            return -4;   /* -EINTR */
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
}

int net_socket_shutdown(net_socket_t *s, int how) {
    preempt_disable();
    int r = socket_shutdown_locked(s, how);
    preempt_enable();
    return r;
}

