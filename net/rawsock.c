/*
 * AF_PACKET (SOCK_RAW / SOCK_DGRAM) and AF_INET SOCK_RAW sockets.
 *
 * AF_PACKET sees every Ethernet frame net.c receives, before lwIP does
 * (net_receive_ethernet -> packet_deliver), and ETH_P_ALL sockets also see
 * the frames sent (net_send).  A SOCK_RAW socket reads and writes whole
 * frames; a SOCK_DGRAM one reads the payload after the 14-byte header, and
 * on send the kernel builds that header from sockaddr_ll.  busybox udhcpc
 * talks DHCP this way, with a classic BPF filter attached (SO_ATTACH_FILTER),
 * which runs here as on Linux: a filter returning 0 drops the frame, any
 * other value truncates it.
 *
 * AF_INET SOCK_RAW is lwIP's raw API: received datagrams of the protocol
 * come with their IP header, sends get one from lwIP unless IP_HDRINCL is
 * set (always for IPPROTO_RAW).  ping and udhcpc's interface probe use it.
 *
 * Both need euid 0 (Linux: CAP_NET_RAW).  The sockaddr_ll and sock_filter
 * layouts follow the Linux UAPI (linux/if_packet.h, linux/filter.h); the BPF
 * interpreter is written from the instruction set's documentation
 * (Documentation/networking/filter.rst, McCanne & Jacobson 1993), no Linux
 * code is used.
 */
#include "xsock.h"
#include "net.h"
#include "lwip_glue.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../proc/process.h"
#include "../proc/scheduler.h"

#include "lwip/raw.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"

#define AF_INET_K     2
#define AF_PACKET_K   17
#define SOL_SOCKET_K  1
#define SOL_PACKET_K  263
#define IPPROTO_IP_K  0
#define IP_HDRINCL_K  3
#define IPPROTO_RAW_K 255
#define SO_ATTACH_FILTER_K 26
#define SO_DETACH_FILTER_K 27
#define SO_LOCK_FILTER_K   44
#define PACKET_ADD_MEMBERSHIP_K  1
#define PACKET_DROP_MEMBERSHIP_K 2
#define ETH_P_ALL_K   0x0003
#define ETH_HLEN      14

#define RING_DEPTH    32
#define FRAME_MAX     1536

static uint16_t bswap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }

static int is_root(void) { return current_proc && current_proc->euid == 0; }

/* ── a ring of received datagrams ────────────────────────────────────────── */

typedef struct {
    uint16_t len;
    uint16_t proto;        /* ethertype, network order (AF_PACKET) */
    int      ifindex;
    uint8_t  pkttype;
    uint8_t  src[6];       /* source MAC, or the source IPv4 (raw IP) */
    uint8_t  data[FRAME_MAX];
} frame_t;

typedef struct {
    frame_t *slots;
    int head, count;
} ring_t;

static int ring_init(ring_t *r) {
    r->slots = (frame_t *)kmalloc(RING_DEPTH * sizeof(frame_t));
    r->head = r->count = 0;
    return r->slots ? 0 : -12;
}

/* The slot to fill, or NULL when the ring is full (the frame is dropped,
 * as a full socket receive buffer drops on Linux). */
static frame_t *ring_push(ring_t *r) {
    if (r->count >= RING_DEPTH) return NULL;
    frame_t *f = &r->slots[(r->head + r->count) % RING_DEPTH];
    r->count++;
    return f;
}

static frame_t *ring_peek(ring_t *r) {
    return r->count ? &r->slots[r->head] : NULL;
}

static void ring_pop(ring_t *r) {
    r->head = (r->head + 1) % RING_DEPTH;
    r->count--;
}

/* ── classic BPF ─────────────────────────────────────────────────────────── */

typedef struct { uint16_t code; uint8_t jt, jf; uint32_t k; } bpf_insn_t;

#define BPF_CLASS(c) ((c) & 0x07)
#define BPF_LD   0x00
#define BPF_LDX  0x01
#define BPF_ST   0x02
#define BPF_STX  0x03
#define BPF_ALU  0x04
#define BPF_JMP  0x05
#define BPF_RET  0x06
#define BPF_MISC 0x07
#define BPF_SIZE(c) ((c) & 0x18)
#define BPF_W 0x00
#define BPF_H 0x08
#define BPF_B 0x10
#define BPF_MODE(c) ((c) & 0xe0)
#define BPF_IMM 0x00
#define BPF_ABS 0x20
#define BPF_IND 0x40
#define BPF_MEM 0x60
#define BPF_LEN 0x80
#define BPF_MSH 0xa0
#define BPF_OP(c) ((c) & 0xf0)
#define BPF_SRC(c) ((c) & 0x08)
#define BPF_X 0x08
#define BPF_MAXINSNS 4096

/* A program is accepted only if every jump lands inside it and it ends in
 * a return, so the interpreter never runs off the end. */
static int bpf_check(const bpf_insn_t *p, uint32_t n) {
    if (n == 0 || n > BPF_MAXINSNS) return -22;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t c = p[i].code;
        switch (BPF_CLASS(c)) {
        case BPF_JMP:
            if (BPF_OP(c) == 0x00) {                  /* JA */
                if (p[i].k >= n - i - 1) return -22;
            } else if (i + 1 + p[i].jt >= n || i + 1 + p[i].jf >= n) {
                return -22;
            }
            break;
        case BPF_LD: case BPF_LDX:
            if (BPF_MODE(c) == BPF_MEM && p[i].k >= 16) return -22;
            break;
        case BPF_ST: case BPF_STX:
            if (p[i].k >= 16) return -22;
            break;
        case BPF_ALU:
            if ((BPF_OP(c) == 0x30 || BPF_OP(c) == 0x90) &&    /* DIV, MOD */
                BPF_SRC(c) != BPF_X && p[i].k == 0)
                return -22;
            break;
        }
    }
    return BPF_CLASS(p[n - 1].code) == BPF_RET ? 0 : -22;
}

static int bpf_load(const uint8_t *pkt, uint32_t len, uint32_t off, int size,
                    uint32_t *out) {
    if (off >= len || len - off < (uint32_t)size) return 0;
    const uint8_t *p = pkt + off;
    *out = size == 4 ? ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
                        (uint32_t)p[2] << 8 | p[3])
         : size == 2 ? ((uint32_t)p[0] << 8 | p[1]) : p[0];
    return 1;
}

/* Runs a checked program over `len` bytes; the number of bytes to keep. */
static uint32_t bpf_run(const bpf_insn_t *prog, const uint8_t *pkt, uint32_t len) {
    uint32_t A = 0, X = 0, M[16];
    memset(M, 0, sizeof(M));
    for (const bpf_insn_t *pc = prog; ; pc++) {
        uint16_t c = pc->code;
        uint32_t k = pc->k, v;
        int sz = BPF_SIZE(c) == BPF_W ? 4 : BPF_SIZE(c) == BPF_H ? 2 : 1;
        switch (BPF_CLASS(c)) {
        case BPF_LD:
            switch (BPF_MODE(c)) {
            case BPF_IMM: A = k; break;
            case BPF_LEN: A = len; break;
            case BPF_MEM: A = M[k]; break;
            case BPF_ABS:
                if (!bpf_load(pkt, len, k, sz, &v)) return 0;
                A = v; break;
            case BPF_IND:
                if (!bpf_load(pkt, len, X + k, sz, &v)) return 0;
                A = v; break;
            default: return 0;
            }
            break;
        case BPF_LDX:
            switch (BPF_MODE(c)) {
            case BPF_IMM: X = k; break;
            case BPF_LEN: X = len; break;
            case BPF_MEM: X = M[k]; break;
            case BPF_MSH:
                if (!bpf_load(pkt, len, k, 1, &v)) return 0;
                X = (v & 0x0F) * 4; break;
            default: return 0;
            }
            break;
        case BPF_ST:  M[k] = A; break;
        case BPF_STX: M[k] = X; break;
        case BPF_ALU: {
            uint32_t s = BPF_SRC(c) == BPF_X ? X : k;
            switch (BPF_OP(c)) {
            case 0x00: A += s; break;
            case 0x10: A -= s; break;
            case 0x20: A *= s; break;
            case 0x30: if (!s) return 0; A /= s; break;
            case 0x40: A |= s; break;
            case 0x50: A &= s; break;
            case 0x60: A = s < 32 ? A << s : 0; break;
            case 0x70: A = s < 32 ? A >> s : 0; break;
            case 0x80: A = (uint32_t)-(int32_t)A; break;
            case 0x90: if (!s) return 0; A %= s; break;
            case 0xa0: A ^= s; break;
            default: return 0;
            }
            break;
        }
        case BPF_JMP: {
            uint32_t s = BPF_SRC(c) == BPF_X ? X : k;
            int t;
            switch (BPF_OP(c)) {
            case 0x00: pc += k; continue;
            case 0x10: t = A == s; break;
            case 0x20: t = A > s; break;
            case 0x30: t = A >= s; break;
            case 0x40: t = (A & s) != 0; break;
            default: return 0;
            }
            pc += t ? pc->jt : pc->jf;
            break;
        }
        case BPF_RET:
            return (c & 0x18) == 0x10 ? A : k;         /* RET A / RET K */
        case BPF_MISC:
            if (c & 0x80) A = X; else X = A;           /* TXA / TAX */
            break;
        }
    }
}

/* ── AF_PACKET ───────────────────────────────────────────────────────────── */

typedef struct psock {
    struct psock *next;
    int       type;          /* 3 SOCK_RAW: whole frames; 2 SOCK_DGRAM */
    uint16_t  proto;         /* ethertype it receives, network order; 0 none */
    int       ifindex;       /* bound interface, 0 = any */
    ring_t    rx;
    bpf_insn_t *filter;
    uint32_t  filter_len;
} psock_t;

static psock_t *packet_socks;

typedef struct {
    uint16_t family, protocol;
    int32_t  ifindex;
    uint16_t hatype;
    uint8_t  pkttype, halen;
    uint8_t  addr[8];
} sockaddr_ll_k;

static void deliver_one(psock_t *s, netif_t *iface, const uint8_t *frame,
                        uint32_t len, uint8_t pkttype) {
    uint32_t off = s->type == 3 ? 0 : ETH_HLEN;
    uint32_t n = len - off;
    if (s->filter) {
        uint32_t keep = bpf_run(s->filter, frame + off, n);
        if (!keep) return;
        if (keep < n) n = keep;
    }
    frame_t *f = ring_push(&s->rx);
    if (!f) return;
    if (n > FRAME_MAX) n = FRAME_MAX;
    memcpy(f->data, frame + off, n);
    f->len = (uint16_t)n;
    f->proto = (uint16_t)(frame[12] | frame[13] << 8);   /* as on the wire */
    f->ifindex = netdev_index(iface);
    f->pkttype = pkttype;
    memcpy(f->src, frame + 6, 6);
}

void packet_deliver(netif_t *iface, const uint8_t *frame, uint32_t len,
                    int outgoing) {
    if (!packet_socks || len < ETH_HLEN) return;
    uint16_t etype = (uint16_t)(frame[12] | frame[13] << 8);
    uint8_t pkttype;
    if (outgoing) pkttype = 4;                                 /* OUTGOING */
    else if (memcmp(frame, iface->mac, 6) == 0) pkttype = 0;   /* HOST */
    else if (frame[0] == 0xFF && frame[1] == 0xFF && frame[2] == 0xFF &&
             frame[3] == 0xFF && frame[4] == 0xFF && frame[5] == 0xFF)
        pkttype = 1;                                           /* BROADCAST */
    else if (frame[0] & 1) pkttype = 2;                        /* MULTICAST */
    else pkttype = 3;                                          /* OTHERHOST */
    int idx = netdev_index(iface);
    for (psock_t *s = packet_socks; s; s = s->next) {
        if (!s->proto) continue;
        if (s->ifindex && s->ifindex != idx) continue;
        int all = s->proto == bswap16(ETH_P_ALL_K);
        if (!all && (outgoing || s->proto != etype)) continue;
        deliver_one(s, iface, frame, len, pkttype);
    }
    if (outgoing) return;
    io_wake();
}

static int pk_bind(void *x, const void *addr, uint32_t alen) {
    psock_t *s = (psock_t *)x;
    sockaddr_ll_k sa;
    if (alen < 12) return -22;
    memset(&sa, 0, sizeof(sa));
    memcpy(&sa, addr, alen < sizeof(sa) ? alen : sizeof(sa));
    if (sa.family != AF_PACKET_K) return -22;
    if (sa.ifindex && !netdev_by_index(sa.ifindex)) return -19;   /* -ENODEV */
    s->ifindex = sa.ifindex;
    if (sa.protocol) s->proto = sa.protocol;
    return 0;
}

static int pk_connect(void *x, const void *addr, uint32_t alen) {
    (void)x; (void)addr; (void)alen;
    return -95;
}

static int pk_getname(void *x, int peer, void *addr, uint32_t *alen) {
    psock_t *s = (psock_t *)x;
    if (peer) return -95;
    sockaddr_ll_k sa;
    memset(&sa, 0, sizeof(sa));
    sa.family = AF_PACKET_K;
    sa.protocol = s->proto;
    sa.ifindex = s->ifindex;
    netif_t *iface = s->ifindex ? netdev_by_index(s->ifindex) : NULL;
    if (iface) {
        sa.hatype = 1;
        sa.halen = 6;
        memcpy(sa.addr, iface->mac, 6);
    }
    memcpy(addr, &sa, sizeof(sa));
    *alen = sizeof(sa);
    return 0;
}

static int pk_send(void *x, const void *buf, uint32_t len,
                   const void *addr, uint32_t alen) {
    psock_t *s = (psock_t *)x;
    sockaddr_ll_k sa;
    memset(&sa, 0, sizeof(sa));
    int has = 0;
    if (addr && alen) {
        if (alen < 12) return -22;
        memcpy(&sa, addr, alen < sizeof(sa) ? alen : sizeof(sa));
        if (sa.family != AF_PACKET_K) return -22;
        has = 1;
    }
    int idx = has && sa.ifindex ? sa.ifindex : s->ifindex;
    netif_t *iface = idx ? netdev_by_index(idx) : NULL;
    if (!iface) return -6;                             /* -ENXIO */
    if (!(net_lwip_iface_is_ip(iface) ? net_lwip_is_up() : 0))
        return -100;                                   /* -ENETDOWN */
    uint8_t frame[FRAME_MAX];
    uint32_t total;
    if (s->type == 3) {                                /* the whole frame */
        if (len < ETH_HLEN) return -22;
        if (len > iface->mtu + ETH_HLEN) return -90;   /* -EMSGSIZE */
        memcpy(frame, buf, len);
        total = len;
    } else {
        if (!has) return -89;                          /* -EDESTADDRREQ */
        if (len > iface->mtu) return -90;
        uint16_t proto = sa.protocol ? sa.protocol : s->proto;
        memcpy(frame, sa.addr, 6);
        memcpy(frame + 6, iface->mac, 6);
        frame[12] = (uint8_t)(proto & 0xFF);           /* network order */
        frame[13] = (uint8_t)(proto >> 8);
        memcpy(frame + ETH_HLEN, buf, len);
        total = len + ETH_HLEN;
    }
    if (total < 60) {                                  /* Ethernet minimum */
        memset(frame + total, 0, 60 - total);
        total = 60;
    }
    int r = net_send(iface, frame, total);   /* ETH_P_ALL taps see it there */
    return r < 0 ? -105 : (int)len;                    /* -ENOBUFS */
}

static int pk_recv(void *x, void *buf, uint32_t len, void *addr,
                   uint32_t *alen, int peek) {
    psock_t *s = (psock_t *)x;
    frame_t *f = ring_peek(&s->rx);
    if (!f) return -11;
    uint32_t n = f->len < len ? f->len : len;
    memcpy(buf, f->data, n);
    if (addr) {
        sockaddr_ll_k sa;
        memset(&sa, 0, sizeof(sa));
        sa.family = AF_PACKET_K;
        sa.protocol = f->proto;
        sa.ifindex = f->ifindex;
        sa.hatype = 1;
        sa.pkttype = f->pkttype;
        sa.halen = 6;
        memcpy(sa.addr, f->src, 6);
        memcpy(addr, &sa, sizeof(sa));
        *alen = sizeof(sa);
    }
    int full = f->len;
    if (!peek) ring_pop(&s->rx);
    return full;
}

static int pk_read_ready(void *x) {
    return ((psock_t *)x)->rx.count > 0;
}

static int pk_setopt(void *x, int level, int name, const void *val, uint32_t len) {
    psock_t *s = (psock_t *)x;
    if (level == SOL_SOCKET_K && name == SO_ATTACH_FILTER_K) {
        /* `val` is the program itself, copied in by the syscall layer. */
        uint32_t n = len / sizeof(bpf_insn_t);
        if (len % sizeof(bpf_insn_t)) return -22;
        int r = bpf_check((const bpf_insn_t *)val, n);
        if (r < 0) return r;
        bpf_insn_t *p = (bpf_insn_t *)kmalloc(len);
        if (!p) return -12;
        memcpy(p, val, len);
        if (s->filter) kfree(s->filter);
        s->filter = p;
        s->filter_len = n;
        /* Frames queued before the filter would not have passed it. */
        s->rx.head = s->rx.count = 0;
        return 0;
    }
    if (level == SOL_SOCKET_K && name == SO_DETACH_FILTER_K) {
        if (!s->filter) return -2;                     /* -ENOENT */
        kfree(s->filter);
        s->filter = NULL;
        return 0;
    }
    if (level == SOL_SOCKET_K && name == SO_LOCK_FILTER_K) return 0;
    if (level == SOL_PACKET_K) {
        if (name == PACKET_ADD_MEMBERSHIP_K || name == PACKET_DROP_MEMBERSHIP_K)
            return 0;    /* promiscuous/multicast: the NIC filter stays */
        return -92;      /* -ENOPROTOOPT (PACKET_AUXDATA, rings, fanout) */
    }
    return 1;
}

static int pk_getopt(void *x, int level, int name, void *val, uint32_t *len) {
    (void)x; (void)name; (void)val; (void)len;
    return level == SOL_PACKET_K ? -92 : 1;
}

static void pk_release(void *x) {
    psock_t *s = (psock_t *)x;
    for (psock_t **pp = &packet_socks; *pp; pp = &(*pp)->next)
        if (*pp == s) { *pp = s->next; break; }
    if (s->filter) kfree(s->filter);
    kfree(s->rx.slots);
    kfree(s);
}

static const xsock_ops_t pk_ops = {
    pk_bind, pk_connect, pk_getname, pk_send, pk_recv, pk_read_ready,
    pk_setopt, pk_getopt, pk_release,
};

int packet_create(int type, int protocol, const xsock_ops_t **ops, void **x) {
    if (type != 2 && type != 3) return -94;            /* -ESOCKTNOSUPPORT */
    if (!is_root()) return -1;                         /* -EPERM */
    psock_t *s = (psock_t *)kmalloc(sizeof(*s));
    if (!s) return -12;
    memset(s, 0, sizeof(*s));
    if (ring_init(&s->rx) < 0) { kfree(s); return -12; }
    s->type = type;
    s->proto = (uint16_t)protocol;         /* htons(ETH_P_*) from the caller */
    s->next = packet_socks;
    packet_socks = s;
    *ops = &pk_ops;
    *x = s;
    return 0;
}

/* ── AF_INET SOCK_RAW ────────────────────────────────────────────────────── */

typedef struct {
    struct raw_pcb *pcb;
    int   proto;
    int   hdrincl;
    ring_t rx;
    uint32_t peer;           /* connect(): default destination */
} rsock_t;

typedef struct {
    uint16_t family, port;
    uint32_t addr;
    uint8_t  zero[8];
} sockaddr_in_k;

static u8_t raw_recv_cb(void *arg, struct raw_pcb *pcb, struct pbuf *p,
                        const ip_addr_t *addr) {
    (void)pcb;
    rsock_t *s = (rsock_t *)arg;
    frame_t *f = ring_push(&s->rx);
    if (f) {
        uint32_t n = p->tot_len < FRAME_MAX ? p->tot_len : FRAME_MAX;
        pbuf_copy_partial(p, f->data, (u16_t)n, 0);
        f->len = (uint16_t)n;
        uint32_t a = ip4_addr_get_u32(ip_2_ip4(addr));
        memcpy(f->src, &a, 4);
        io_wake();
    }
    return 0;      /* not eaten: lwIP goes on to handle it (ICMP echo...) */
}

static int rw_bind(void *x, const void *addr, uint32_t alen) {
    rsock_t *s = (rsock_t *)x;
    sockaddr_in_k sa;
    if (alen < sizeof(sa)) return -22;
    memcpy(&sa, addr, sizeof(sa));
    if (sa.family != AF_INET_K) return -97;
    if (!net_lwip_addr_is_local(sa.addr)) return -99;
    ip_addr_t a;
    ip_addr_set_ip4_u32_val(a, sa.addr);
    return raw_bind(s->pcb, &a) == ERR_OK ? 0 : -22;
}

static int rw_connect(void *x, const void *addr, uint32_t alen) {
    rsock_t *s = (rsock_t *)x;
    sockaddr_in_k sa;
    if (alen < sizeof(sa)) return -22;
    memcpy(&sa, addr, sizeof(sa));
    if (sa.family != AF_INET_K) return -97;
    s->peer = sa.addr;
    return 0;
}

static int rw_getname(void *x, int peer, void *addr, uint32_t *alen) {
    rsock_t *s = (rsock_t *)x;
    sockaddr_in_k sa;
    memset(&sa, 0, sizeof(sa));
    sa.family = AF_INET_K;
    if (peer) {
        if (!s->peer) return -107;
        sa.addr = s->peer;
    } else {
        sa.addr = ip4_addr_get_u32(ip_2_ip4(&s->pcb->local_ip));
    }
    memcpy(addr, &sa, sizeof(sa));
    *alen = sizeof(sa);
    return 0;
}

static int rw_send(void *x, const void *buf, uint32_t len,
                   const void *addr, uint32_t alen) {
    rsock_t *s = (rsock_t *)x;
    uint32_t dst = s->peer;
    if (addr && alen) {
        sockaddr_in_k sa;
        if (alen < sizeof(sa)) return -22;
        memcpy(&sa, addr, sizeof(sa));
        if (sa.family != AF_INET_K) return -97;
        dst = sa.addr;
    }
    if (!dst) return -89;                              /* -EDESTADDRREQ */
    if (len > 65535) return -90;
    if (s->hdrincl && len < 20) return -22;    /* Linux raw_send_hdrinc */
    struct pbuf *p = pbuf_alloc(s->hdrincl ? PBUF_LINK : PBUF_IP, (u16_t)len,
                                PBUF_RAM);
    if (!p) return -105;
    pbuf_take(p, buf, (u16_t)len);
    ip_addr_t a;
    ip_addr_set_ip4_u32_val(a, dst);
    raw_setflags(s->pcb, s->hdrincl ? RAW_FLAGS_HDRINCL : 0);
    err_t e = raw_sendto(s->pcb, p, &a);
    pbuf_free(p);
    if (e == ERR_RTE) return -101;                     /* -ENETUNREACH */
    return e == ERR_OK ? (int)len : -105;
}

static int rw_recv(void *x, void *buf, uint32_t len, void *addr,
                   uint32_t *alen, int peek) {
    rsock_t *s = (rsock_t *)x;
    frame_t *f = ring_peek(&s->rx);
    if (!f) return -11;
    uint32_t n = f->len < len ? f->len : len;
    memcpy(buf, f->data, n);
    if (addr) {
        sockaddr_in_k sa;
        memset(&sa, 0, sizeof(sa));
        sa.family = AF_INET_K;
        memcpy(&sa.addr, f->src, 4);
        memcpy(addr, &sa, sizeof(sa));
        *alen = sizeof(sa);
    }
    int full = f->len;
    if (!peek) ring_pop(&s->rx);
    return full;
}

static int rw_read_ready(void *x) {
    return ((rsock_t *)x)->rx.count > 0;
}

static int rw_setopt(void *x, int level, int name, const void *val, uint32_t len) {
    rsock_t *s = (rsock_t *)x;
    if (level == IPPROTO_IP_K && name == IP_HDRINCL_K) {
        int v = 0;
        if (len >= 4) memcpy(&v, val, 4); else if (len >= 1) v = *(const uint8_t *)val;
        s->hdrincl = v != 0 || s->proto == IPPROTO_RAW_K;
        return 0;
    }
    return 1;
}

static int rw_getopt(void *x, int level, int name, void *val, uint32_t *len) {
    rsock_t *s = (rsock_t *)x;
    if (level == IPPROTO_IP_K && name == IP_HDRINCL_K) {
        int32_t v = s->hdrincl;
        uint32_t n = *len < 4 ? *len : 4;
        memcpy(val, &v, n);
        *len = n;
        return 0;
    }
    return 1;
}

static void rw_release(void *x) {
    rsock_t *s = (rsock_t *)x;
    raw_remove(s->pcb);
    kfree(s->rx.slots);
    kfree(s);
}

static const xsock_ops_t rw_ops = {
    rw_bind, rw_connect, rw_getname, rw_send, rw_recv, rw_read_ready,
    rw_setopt, rw_getopt, rw_release,
};

int rawip_create(int protocol, const xsock_ops_t **ops, void **x) {
    if (!is_root()) return -1;                         /* -EPERM */
    if (protocol <= 0 || protocol > 255) return -93;   /* -EPROTONOSUPPORT */
    rsock_t *s = (rsock_t *)kmalloc(sizeof(*s));
    if (!s) return -12;
    memset(s, 0, sizeof(*s));
    if (ring_init(&s->rx) < 0) { kfree(s); return -12; }
    s->proto = protocol;
    s->hdrincl = protocol == IPPROTO_RAW_K;
    s->pcb = raw_new((u8_t)protocol);
    if (!s->pcb) { kfree(s->rx.slots); kfree(s); return -12; }
    /* IPPROTO_RAW is send-only on Linux. */
    if (protocol != IPPROTO_RAW_K)
        raw_recv(s->pcb, raw_recv_cb, s);
    *ops = &rw_ops;
    *x = s;
    return 0;
}

/* ── AF_INET6 SOCK_RAW and ICMP echo "ping sockets" ─────────────────────────
 *
 * One lwIP raw pcb each.  An AF_INET6 SOCK_RAW socket reads and writes the
 * payload after the IPv6 header (Linux raw(7)/ipv6(7)): for IPPROTO_ICMPV6
 * the kernel fills in the checksum, which needs the pseudo-header.  A ping
 * socket (Linux net/ipv4/ping.c semantics, RFC 792 / RFC 4443 messages)
 * sends ICMP echo requests whose identifier is the socket's "port" and
 * receives only the echo replies carrying it, as ICMP header plus data, with
 * the checksum computed by the kernel; no privilege is needed.
 */

#include "lwip/inet_chksum.h"
#include "../kernel/random.h"
#include "lwip/ip6_addr.h"

#define AF_INET6_K        10
#define IPPROTO_ICMP_K     1
#define IPPROTO_ICMPV6_K  58
#define IPPROTO_IPV6_K    41
#define IPV6_CHECKSUM_K    7
#define ICMP_RING          16

typedef struct {
    uint16_t len;
    ip_addr_t src;
    uint8_t data[FRAME_MAX];
} ipkt_t;

typedef struct isock {
    struct raw_pcb *pcb;
    int domain;            /* AF_INET (ping only) or AF_INET6 */
    int proto;
    int ping;              /* SOCK_DGRAM echo socket */
    uint16_t ident;        /* ping: the echo identifier, host order */
    int connected;
    ip_addr_t peer;
    ipkt_t *ring;
    int head, count;
    struct isock *next;    /* ping: the list of live ping sockets */
} isock_t;

typedef struct {
    uint16_t family, port;
    uint32_t flowinfo;
    uint8_t  addr[16];
    uint32_t scope_id;
} sockaddr_in6_k;

/* Live ping sockets.  An echo identifier belongs to one socket of a family
 * at a time (Linux ping_get_port: bind to a taken one is EADDRINUSE), so a
 * reply reaches only the socket that sent the request.  Free identifiers
 * are picked from a random start, not a guessable sequence. */
static isock_t *ping_socks;

static int ident_in_use(int domain, uint16_t id, const isock_t *except) {
    for (isock_t *p = ping_socks; p; p = p->next)
        if (p != except && p->domain == domain && p->ident == id)
            return 1;
    return 0;
}

/* An unused nonzero identifier, 0 when all 65535 are taken. */
static uint16_t ident_alloc(int domain) {
    uint16_t id;
    random_get_bytes(&id, sizeof(id));
    for (uint32_t n = 0; n < 65536; n++, id++) {
        if (id && !ident_in_use(domain, id, NULL))
            return id;
    }
    return 0;
}

static int is_v4mapped6(const uint8_t *a) {
    static const uint8_t pfx[12] = { 0,0,0,0, 0,0,0,0, 0,0,0xff,0xff };
    return memcmp(a, pfx, 12) == 0;
}

/* The caller's name into an lwIP address of the socket's family. */
static int isock_addr_in(isock_t *s, const void *addr, uint32_t alen,
                         ip_addr_t *ip, uint16_t *port) {
    uint16_t fam;
    if (alen < 2) return -22;
    memcpy(&fam, addr, 2);
    if (s->domain == AF_INET_K) {
        sockaddr_in_k sa;
        if (alen < sizeof(sa)) return -22;
        memcpy(&sa, addr, sizeof(sa));
        if (sa.family != AF_INET_K) return -97;
        ip_addr_set_ip4_u32(ip, sa.addr);
        if (port) *port = bswap16(sa.port);
        return 0;
    }
    sockaddr_in6_k sa;
    memset(&sa, 0, sizeof(sa));
    if (alen < 24) return -22;
    memcpy(&sa, addr, alen < sizeof(sa) ? alen : sizeof(sa));
    if (sa.family != AF_INET6_K) return -97;
    if (is_v4mapped6(sa.addr)) return -101;
    ip_addr_set_zero_ip6(ip);
    memcpy(ip_2_ip6(ip)->addr, sa.addr, 16);
    ip6_addr_clear_zone(ip_2_ip6(ip));
    if (ip6_addr_has_scope(ip_2_ip6(ip), IP6_UNKNOWN))
        ip6_addr_set_zone(ip_2_ip6(ip),
                          (u8_t)(sa.scope_id ? sa.scope_id : (uint32_t)net_lwip_eth_zone()));
    if (port) *port = bswap16(sa.port);
    return 0;
}

static void isock_addr_out(isock_t *s, const ip_addr_t *ip, uint16_t port,
                           void *addr, uint32_t *alen) {
    if (s->domain == AF_INET_K) {
        sockaddr_in_k sa;
        memset(&sa, 0, sizeof(sa));
        sa.family = AF_INET_K;
        sa.port = bswap16(port);
        if (ip && IP_IS_V4(ip)) sa.addr = ip4_addr_get_u32(ip_2_ip4(ip));
        memcpy(addr, &sa, sizeof(sa));
        *alen = sizeof(sa);
        return;
    }
    sockaddr_in6_k sa;
    memset(&sa, 0, sizeof(sa));
    sa.family = AF_INET6_K;
    sa.port = bswap16(port);
    if (ip && IP_IS_V6(ip)) {
        memcpy(sa.addr, ip_2_ip6(ip)->addr, 16);
        if (ip6_addr_has_zone(ip_2_ip6(ip)))
            sa.scope_id = ip6_addr_zone(ip_2_ip6(ip));
    }
    memcpy(addr, &sa, sizeof(sa));
    *alen = sizeof(sa);
}

static u8_t isock_recv_cb(void *arg, struct raw_pcb *pcb, struct pbuf *p,
                          const ip_addr_t *addr) {
    (void)pcb;
    isock_t *s = (isock_t *)arg;
    /* p starts at the IP header: skip it (the protocol matched the first
     * next-header, so an IPv6 packet has no extension headers here). */
    uint16_t hl = 40;
    if (s->domain == AF_INET_K) {
        uint8_t vihl;
        if (pbuf_copy_partial(p, &vihl, 1, 0) != 1) return 0;
        hl = (uint16_t)((vihl & 0x0F) * 4);
    }
    if (p->tot_len < hl) return 0;
    uint16_t n = (uint16_t)(p->tot_len - hl);
    if (s->ping) {
        uint8_t h[8];
        if (n < 8 || pbuf_copy_partial(p, h, 8, hl) != 8) return 0;
        uint8_t reply = s->domain == AF_INET_K ? 0 : 129;
        uint16_t id = (uint16_t)(h[4] << 8 | h[5]);
        if (h[0] != reply || id != s->ident) return 0;
    }
    if (s->count >= ICMP_RING) return 0;
    ipkt_t *k = &s->ring[(s->head + s->count) % ICMP_RING];
    if (n > FRAME_MAX) n = FRAME_MAX;
    pbuf_copy_partial(p, k->data, n, hl);
    k->len = n;
    ip_addr_copy(k->src, *addr);
    s->count++;
    io_wake();
    return 0;      /* not eaten: lwIP's ICMP still answers echo requests */
}

static int is_bind(void *x, const void *addr, uint32_t alen) {
    isock_t *s = (isock_t *)x;
    ip_addr_t a;
    uint16_t port = 0;
    int r = isock_addr_in(s, addr, alen, &a, &port);
    if (r < 0) return r;
    if (IP_IS_V4(&a) ? !net_lwip_addr_is_local(ip4_addr_get_u32(ip_2_ip4(&a)))
                        : !net_lwip_addr6_is_local((const uint8_t *)ip_2_ip6(&a)->addr))
        return -99;
    if (s->ping && port && port != s->ident) {
        if (ident_in_use(s->domain, port, s))
            return -98;                                /* -EADDRINUSE */
    }
    if (raw_bind(s->pcb, &a) != ERR_OK) return -22;
    if (s->ping && port) s->ident = port;
    return 0;
}

static int is_connect(void *x, const void *addr, uint32_t alen) {
    isock_t *s = (isock_t *)x;
    ip_addr_t a;
    int r = isock_addr_in(s, addr, alen, &a, NULL);
    if (r < 0) return r;
    ip_addr_copy(s->peer, a);
    s->connected = 1;
    return 0;
}

static int is_getname(void *x, int peer, void *addr, uint32_t *alen) {
    isock_t *s = (isock_t *)x;
    if (peer) {
        if (!s->connected) return -107;
        isock_addr_out(s, &s->peer, 0, addr, alen);
    } else {
        isock_addr_out(s, &s->pcb->local_ip, s->ping ? s->ident : 0, addr, alen);
    }
    return 0;
}

static int is_send(void *x, const void *buf, uint32_t len,
                   const void *addr, uint32_t alen) {
    isock_t *s = (isock_t *)x;
    ip_addr_t dst;
    if (addr && alen) {
        int r = isock_addr_in(s, addr, alen, &dst, NULL);
        if (r < 0) return r;
    } else if (s->connected) {
        ip_addr_copy(dst, s->peer);
    } else {
        return -89;                                      /* -EDESTADDRREQ */
    }
    if (len > 65535 - 48) return -90;
    /* ICMPv6 carries a checksum at offset 2 that the stack fills in: a
     * message shorter than its 4-byte type/code/checksum has nowhere to put
     * it (Linux rawv6_push_pending_frames: EINVAL).  lwIP's raw_sendto
     * asserts on it, which halts the machine. */
    if (s->proto == IPPROTO_ICMPV6_K && len < 4) return -22;
    if (s->ping) {
        uint8_t req = s->domain == AF_INET_K ? 8 : 128;
        if (len < 8 || ((const uint8_t *)buf)[0] != req ||
            ((const uint8_t *)buf)[1] != 0)
            return -22;
    }
    struct pbuf *p = pbuf_alloc(PBUF_IP, (u16_t)len, PBUF_RAM);
    if (!p) return -105;
    pbuf_take(p, buf, (u16_t)len);
    uint8_t *h = (uint8_t *)p->payload;
    if (s->ping) {
        h[4] = (uint8_t)(s->ident >> 8);
        h[5] = (uint8_t)s->ident;
    }
    if (s->ping || s->proto == IPPROTO_ICMPV6_K) {
        h[2] = h[3] = 0;           /* ICMPv6: lwIP sums with the pseudo-header */
        if (s->domain == AF_INET_K) {
            u16_t c = inet_chksum(h, (u16_t)len);
            memcpy(h + 2, &c, 2);
        }
    }
    err_t e = raw_sendto(s->pcb, p, &dst);
    pbuf_free(p);
    net_lwip_kick();
    if (e == ERR_RTE) return -101;                     /* -ENETUNREACH */
    return e == ERR_OK ? (int)len : -105;
}

static int is_recv(void *x, void *buf, uint32_t len, void *addr,
                   uint32_t *alen, int peek) {
    isock_t *s = (isock_t *)x;
    if (!s->count) return -11;
    ipkt_t *k = &s->ring[s->head];
    uint32_t n = k->len < len ? k->len : len;
    memcpy(buf, k->data, n);
    if (addr)
        isock_addr_out(s, &k->src, 0, addr, alen);
    int full = k->len;
    if (!peek) {
        s->head = (s->head + 1) % ICMP_RING;
        s->count--;
    }
    return full;
}

static int is_read_ready(void *x) {
    return ((isock_t *)x)->count > 0;
}

static int is_setopt(void *x, int level, int name, const void *val, uint32_t len) {
    (void)x; (void)val; (void)len;
    /* IPV6_CHECKSUM on ICMPv6 is always on (Linux refuses to change it);
     * the rest (ICMP6_FILTER, hop limits, IP_RECVTTL...) are accepted and
     * ignored. */
    if (level == IPPROTO_IPV6_K && name == IPV6_CHECKSUM_K)
        return 0;
    return 1;
}

static int is_getopt(void *x, int level, int name, void *val, uint32_t *len) {
    (void)x; (void)level; (void)name; (void)val; (void)len;
    return 1;
}

static void is_release(void *x) {
    isock_t *s = (isock_t *)x;
    for (isock_t **pp = &ping_socks; *pp; pp = &(*pp)->next)
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    raw_remove(s->pcb);
    kfree(s->ring);
    kfree(s);
}

static const xsock_ops_t is_ops = {
    is_bind, is_connect, is_getname, is_send, is_recv, is_read_ready,
    is_setopt, is_getopt, is_release,
};

static int isock_create(int domain, int protocol, int ping,
                        const xsock_ops_t **ops, void **x) {
    isock_t *s = (isock_t *)kmalloc(sizeof(*s));
    if (!s) return -12;
    memset(s, 0, sizeof(*s));
    s->ring = (ipkt_t *)kmalloc(ICMP_RING * sizeof(ipkt_t));
    if (!s->ring) { kfree(s); return -12; }
    s->domain = domain;
    s->proto = protocol;
    s->ping = ping;
    s->pcb = raw_new_ip_type(domain == AF_INET_K ? IPADDR_TYPE_V4 : IPADDR_TYPE_V6,
                             (u8_t)protocol);
    if (!s->pcb) { kfree(s->ring); kfree(s); return -12; }
    if (domain == AF_INET6_K && protocol == IPPROTO_ICMPV6_K) {
        s->pcb->chksum_reqd = 1;
        s->pcb->chksum_offset = 2;
    }
    if (ping) {
        s->ident = ident_alloc(domain);
        if (!s->ident) {
            raw_remove(s->pcb);
            kfree(s->ring);
            kfree(s);
            return -98;                                /* -EADDRINUSE */
        }
        s->next = ping_socks;
        ping_socks = s;
    }
    raw_recv(s->pcb, isock_recv_cb, s);
    *ops = &is_ops;
    *x = s;
    return 0;
}

int rawip6_create(int protocol, const xsock_ops_t **ops, void **x) {
    if (!is_root()) return -1;                         /* -EPERM */
    if (protocol <= 0 || protocol > 255) return -93;
    return isock_create(AF_INET6_K, protocol, 0, ops, x);
}

int ping_create(int domain, const xsock_ops_t **ops, void **x) {
    return isock_create(domain, domain == AF_INET_K ? IPPROTO_ICMP_K
                                                    : IPPROTO_ICMPV6_K, 1, ops, x);
}
