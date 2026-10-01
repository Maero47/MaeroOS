/*
 * AF_NETLINK / NETLINK_ROUTE (rtnetlink) and the interface ioctls.
 *
 * busybox `ip`, iproute2, musl's getifaddrs()/if_nameindex() and udhcpc's
 * script read and change the network configuration through rtnetlink;
 * `ifconfig`, `route`, `ip link set` and udhcpc itself through the SIOC*
 * ioctls.  Both are views of the same state: net.c's interface table (index =
 * position + 1) and lwIP's configuration of eth0 (lwip_glue.c), which holds
 * one IPv4 address and one default gateway.  So:
 *
 *   RTM_GETLINK / GETADDR / GETROUTE   dumps (and single GETLINK, route get)
 *   RTM_NEWADDR / DELADDR              eth0's address (one at a time)
 *   RTM_NEWROUTE / DELROUTE            the default route; the connected
 *                                      subnet route is implied by the address
 *   RTM_NEWLINK / SETLINK              IFF_UP
 *
 * Anything lwIP cannot hold (a second address, a route to another prefix,
 * another routing table) is refused with EOPNOTSUPP instead of being
 * accepted and dropped.  Replies are queued on the socket during sendmsg()
 * and read back with recvmsg(); multicast groups are accepted but no change
 * notifications are sent.
 *
 * Message layouts follow the Linux UAPI headers (linux/netlink.h,
 * linux/rtnetlink.h, linux/if_link.h, linux/if_addr.h), written here from
 * the documented ABI; no Linux code is used.
 */
#include "xsock.h"
#include "net.h"
#include "lwip_glue.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../mm/heap.h"
#include "../proc/process.h"
#include "../proc/syscall.h"   /* copy_to_user / copy_from_user */

#define AF_NETLINK_K   16
#define AF_INET_K      2
#define AF_INET6_K     10
#define NETLINK_ROUTE_K 0
#define SOL_SOCKET_K   1
#define SOL_NETLINK_K  270

/* netlink.h */
#define NLMSG_NOOP     1
#define NLMSG_ERROR    2
#define NLMSG_DONE     3
#define NLM_F_REQUEST  0x01
#define NLM_F_MULTI    0x02
#define NLM_F_ACK      0x04
#define NLM_F_DUMP     0x300
#define NLM_F_REPLACE  0x100
#define NLM_F_EXCL     0x200
#define NLM_F_CREATE   0x400

/* rtnetlink.h */
#define RTM_NEWLINK    16
#define RTM_DELLINK    17
#define RTM_GETLINK    18
#define RTM_SETLINK    19
#define RTM_NEWADDR    20
#define RTM_DELADDR    21
#define RTM_GETADDR    22
#define RTM_NEWROUTE   24
#define RTM_DELROUTE   25
#define RTM_GETROUTE   26

#define IFLA_ADDRESS   1
#define IFLA_BROADCAST 2
#define IFLA_IFNAME    3
#define IFLA_MTU       4
#define IFLA_QDISC     6
#define IFLA_STATS     7
#define IFLA_TXQLEN    13
#define IFLA_OPERSTATE 16
#define IFLA_LINKMODE  17

#define IFA_ADDRESS    1
#define IFA_LOCAL      2
#define IFA_LABEL      3
#define IFA_BROADCAST  4
#define IFA_CACHEINFO  6
#define IFA_FLAGS      8
#define IFA_F_PERMANENT 0x80

#define RTA_DST        1
#define RTA_OIF        4
#define RTA_GATEWAY    5
#define RTA_PRIORITY   6
#define RTA_PREFSRC    7
#define RTA_TABLE      15
#define RT_TABLE_UNSPEC 0
#define RT_TABLE_MAIN  254
#define RTPROT_KERNEL  2
#define RTPROT_BOOT    3
#define RTPROT_DHCP    16
#define RT_SCOPE_UNIVERSE 0
#define RT_SCOPE_LINK  253
#define RTN_UNICAST    1
#define RTM_F_CLONED   0x200

/* if.h */
#define IFF_UP         0x1
#define IFF_BROADCAST  0x2
#define IFF_RUNNING    0x40
#define IFF_MULTICAST  0x1000
#define IFF_LOWER_UP   0x10000
#define ARPHRD_ETHER   1

#define NL_QUEUE_MAX   (256 * 1024)   /* replies waiting to be read */

static uint32_t bswap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

static int is_root(void) { return current_proc && current_proc->euid == 0; }

/* ── interfaces ──────────────────────────────────────────────────────────── */

int netdev_index(netif_t *iface) {
    for (int i = 0; i < net_interface_count(); i++)
        if (net_get_interface(i) == iface)
            return i + 1;
    return 0;
}

netif_t *netdev_by_index(int index) {
    return net_get_interface(index - 1);
}

static uint32_t if_flags(netif_t *iface) {
    uint32_t f = IFF_BROADCAST | IFF_MULTICAST;
    if (net_lwip_iface_is_ip(iface) && net_lwip_is_up())
        f |= IFF_UP | IFF_RUNNING | IFF_LOWER_UP;
    return f;
}

static int prefix_len(uint32_t mask_be) {
    uint32_t m = bswap32(mask_be);
    int n = 0;
    while (n < 32 && (m & (0x80000000u >> n))) n++;
    return n;
}

static uint32_t mask_of(int plen) {
    return plen <= 0 ? 0 : bswap32(plen >= 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> plen));
}

/* ── message building ────────────────────────────────────────────────────── */

typedef struct nlmsghdr_k {
    uint32_t len;
    uint16_t type, flags;
    uint32_t seq, pid;
} nlmsghdr_k;

typedef struct { uint16_t len, type; } rtattr_k;

typedef struct {
    uint8_t  family, pad;
    uint16_t type;
    int32_t  index;
    uint32_t flags, change;
} ifinfomsg_k;

typedef struct {
    uint8_t  family, prefixlen, flags, scope;
    uint32_t index;
} ifaddrmsg_k;

typedef struct {
    uint8_t  family, dst_len, src_len, tos;
    uint8_t  table, protocol, scope, type;
    uint32_t flags;
} rtmsg_k;

#define ALIGN4(x) (((x) + 3u) & ~3u)

/* A growable reply buffer: one datagram of one or more messages. */
typedef struct {
    uint8_t *buf;
    uint32_t len, cap;
    int      oom;
    uint32_t msg;          /* offset of the message being built */
} nlbuf_t;

static void *nb_put(nlbuf_t *b, uint32_t n) {
    uint32_t need = b->len + ALIGN4(n);
    if (b->oom) return NULL;
    if (need > b->cap) {
        uint32_t cap = b->cap ? b->cap * 2 : 1024;
        while (cap < need) cap *= 2;
        uint8_t *nb = (uint8_t *)kmalloc(cap);
        if (!nb) { b->oom = 1; return NULL; }
        if (b->buf) { memcpy(nb, b->buf, b->len); kfree(b->buf); }
        b->buf = nb;
        b->cap = cap;
    }
    void *p = b->buf + b->len;
    memset(p, 0, ALIGN4(n));
    b->len = need;
    return p;
}

static void nb_begin(nlbuf_t *b, uint16_t type, uint16_t flags,
                     uint32_t seq, uint32_t pid) {
    b->msg = b->len;
    nlmsghdr_k *h = (nlmsghdr_k *)nb_put(b, sizeof(*h));
    if (!h) return;
    h->type = type;
    h->flags = flags;
    h->seq = seq;
    h->pid = pid;
}

static void nb_end(nlbuf_t *b) {
    if (b->oom) return;
    ((nlmsghdr_k *)(b->buf + b->msg))->len = b->len - b->msg;
}

static void nb_attr(nlbuf_t *b, uint16_t type, const void *data, uint32_t n) {
    rtattr_k *a = (rtattr_k *)nb_put(b, sizeof(*a) + n);
    if (!a) return;
    a->len = (uint16_t)(sizeof(*a) + n);
    a->type = type;
    memcpy(a + 1, data, n);
}

static void nb_u32(nlbuf_t *b, uint16_t type, uint32_t v) { nb_attr(b, type, &v, 4); }
static void nb_u8(nlbuf_t *b, uint16_t type, uint8_t v)   { nb_attr(b, type, &v, 1); }
static void nb_str(nlbuf_t *b, uint16_t type, const char *s) {
    nb_attr(b, type, s, (uint32_t)strlen(s) + 1);
}

/* ── the socket ──────────────────────────────────────────────────────────── */

typedef struct nlmsg_q {
    struct nlmsg_q *next;
    uint32_t len;
    uint8_t  data[];
} nlmsg_q_t;

typedef struct nlsock {
    struct nlsock *next;     /* every netlink socket, for port ids */
    uint32_t portid;         /* 0 until bound */
    uint32_t groups;
    nlmsg_q_t *head, *tail;
    uint32_t queued;
    int      drops;          /* replies lost to a full queue: ENOBUFS */
} nlsock_t;

static nlsock_t *nl_all;

static int portid_used(uint32_t id) {
    for (nlsock_t *s = nl_all; s; s = s->next)
        if (s->portid == id) return 1;
    return 0;
}

/* Linux autobind: the thread group id if free, else a negative number. */
static void autobind(nlsock_t *s) {
    static uint32_t next = 0xFFFFF000u;
    if (s->portid) return;
    uint32_t id = current_proc ? (uint32_t)current_proc->tgid : 0;
    if (!id || portid_used(id)) {
        do { id = next--; } while (portid_used(id));
    }
    s->portid = id;
}

static void queue_reply(nlsock_t *s, nlbuf_t *b) {
    if (b->oom || !b->len) { if (b->oom) s->drops = 1; kfree(b->buf); return; }
    if (s->queued + b->len > NL_QUEUE_MAX) { s->drops = 1; kfree(b->buf); return; }
    nlmsg_q_t *m = (nlmsg_q_t *)kmalloc(sizeof(*m) + b->len);
    if (!m) { s->drops = 1; kfree(b->buf); return; }
    m->next = NULL;
    m->len = b->len;
    memcpy(m->data, b->buf, b->len);
    kfree(b->buf);
    if (s->tail) s->tail->next = m; else s->head = m;
    s->tail = m;
    s->queued += m->len;
}

static void reply_error(nlsock_t *s, const nlmsghdr_k *req, int err) {
    nlbuf_t b = {0};
    nb_begin(&b, NLMSG_ERROR, 0, req->seq, s->portid);
    int32_t *e = (int32_t *)nb_put(&b, 4 + sizeof(nlmsghdr_k));
    if (e) {
        *e = err;
        memcpy(e + 1, req, sizeof(nlmsghdr_k));  /* the request's header */
    }
    nb_end(&b);
    queue_reply(s, &b);
}

/* ── attribute parsing ───────────────────────────────────────────────────── */

typedef struct { const void *p[32]; uint16_t len[32]; } attrs_t;

static void parse_attrs(attrs_t *a, const uint8_t *p, uint32_t n) {
    memset(a, 0, sizeof(*a));
    while (n >= sizeof(rtattr_k)) {
        const rtattr_k *ra = (const rtattr_k *)p;
        if (ra->len < sizeof(rtattr_k) || ra->len > n) break;
        uint16_t t = ra->type & 0x3FFF;         /* NLA_F_NESTED etc. */
        if (t < 32) {
            a->p[t] = ra + 1;
            a->len[t] = (uint16_t)(ra->len - sizeof(rtattr_k));
        }
        uint32_t step = ALIGN4(ra->len);
        if (step >= n) break;
        p += step;
        n -= step;
    }
}

static int attr_u32(const attrs_t *a, int t, uint32_t *v) {
    if (!a->p[t] || a->len[t] < 4) return 0;
    memcpy(v, a->p[t], 4);
    return 1;
}

/* ── dumps ───────────────────────────────────────────────────────────────── */

static void put_link(nlbuf_t *b, netif_t *iface, uint16_t flags,
                     uint32_t seq, uint32_t pid) {
    nb_begin(b, RTM_NEWLINK, flags, seq, pid);
    ifinfomsg_k *ifi = (ifinfomsg_k *)nb_put(b, sizeof(*ifi));
    if (ifi) {
        ifi->type = ARPHRD_ETHER;
        ifi->index = netdev_index(iface);
        ifi->flags = if_flags(iface);
        ifi->change = 0xFFFFFFFFu;
    }
    static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    nb_str(b, IFLA_IFNAME, iface->name);
    nb_u32(b, IFLA_TXQLEN, 1000);
    nb_u8(b, IFLA_OPERSTATE, (if_flags(iface) & IFF_UP) ? 6 : 2);  /* UP / DOWN */
    nb_u8(b, IFLA_LINKMODE, 0);
    nb_u32(b, IFLA_MTU, iface->mtu);
    nb_str(b, IFLA_QDISC, "pfifo_fast");
    nb_attr(b, IFLA_ADDRESS, iface->mac, 6);
    nb_attr(b, IFLA_BROADCAST, bcast, 6);
    /* struct rtnl_link_stats: 24 counters. */
    uint32_t st[24];
    memset(st, 0, sizeof(st));
    st[0] = iface->rx_packets;
    st[1] = iface->tx_packets;
    st[2] = iface->rx_bytes;
    st[3] = iface->tx_bytes;
    st[6] = iface->rx_dropped;
    nb_attr(b, IFLA_STATS, st, sizeof(st));
    nb_end(b);
}

static void put_addr(nlbuf_t *b, netif_t *iface, uint16_t flags,
                     uint32_t seq, uint32_t pid) {
    uint32_t ip, mask, gw;
    net_lwip_get_config(&ip, &mask, &gw);
    if (!net_lwip_iface_is_ip(iface) || !ip)
        return;
    int dyn = net_lwip_dhcp_owned();
    nb_begin(b, RTM_NEWADDR, flags, seq, pid);
    ifaddrmsg_k *ifa = (ifaddrmsg_k *)nb_put(b, sizeof(*ifa));
    if (ifa) {
        ifa->family = AF_INET_K;
        ifa->prefixlen = (uint8_t)prefix_len(mask);
        ifa->flags = dyn ? 0 : IFA_F_PERMANENT;
        ifa->scope = RT_SCOPE_UNIVERSE;
        ifa->index = (uint32_t)netdev_index(iface);
    }
    uint32_t bc = ip | ~mask;
    nb_u32(b, IFA_ADDRESS, ip);
    nb_u32(b, IFA_LOCAL, ip);
    nb_u32(b, IFA_BROADCAST, bc);
    nb_str(b, IFA_LABEL, iface->name);
    nb_u32(b, IFA_FLAGS, dyn ? 0 : IFA_F_PERMANENT);
    /* struct ifa_cacheinfo: preferred/valid lifetimes "forever". */
    uint32_t ci[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0 };
    nb_attr(b, IFA_CACHEINFO, ci, sizeof(ci));
    nb_end(b);
}

static uint32_t gw_metric;   /* the metric the default route was added with */

static void put_route(nlbuf_t *b, uint16_t flags, uint32_t seq, uint32_t pid,
                      uint32_t dst, int dst_len, uint32_t gw, uint32_t src,
                      int oif, uint8_t proto, uint8_t scope, uint32_t metric,
                      uint32_t rflags) {
    nb_begin(b, RTM_NEWROUTE, flags, seq, pid);
    rtmsg_k *rt = (rtmsg_k *)nb_put(b, sizeof(*rt));
    if (rt) {
        rt->family = AF_INET_K;
        rt->dst_len = (uint8_t)dst_len;
        rt->table = RT_TABLE_MAIN;
        rt->protocol = proto;
        rt->scope = scope;
        rt->type = RTN_UNICAST;
        rt->flags = rflags;
    }
    nb_u32(b, RTA_TABLE, RT_TABLE_MAIN);
    if (dst_len) nb_u32(b, RTA_DST, dst);
    if (metric) nb_u32(b, RTA_PRIORITY, metric);
    if (gw) nb_u32(b, RTA_GATEWAY, gw);
    if (src) nb_u32(b, RTA_PREFSRC, src);
    nb_u32(b, RTA_OIF, (uint32_t)oif);
    nb_end(b);
}

static netif_t *ip_iface(void) {
    for (int i = 0; i < net_interface_count(); i++)
        if (net_lwip_iface_is_ip(net_get_interface(i)))
            return net_get_interface(i);
    return NULL;
}

static void dump_routes(nlbuf_t *b, uint32_t seq, uint32_t pid) {
    uint32_t ip, mask, gw;
    netif_t *eth = ip_iface();
    net_lwip_get_config(&ip, &mask, &gw);
    if (!eth || !ip || !net_lwip_is_up())
        return;
    int idx = netdev_index(eth);
    if (gw)
        put_route(b, NLM_F_MULTI, seq, pid, 0, 0, gw, 0, idx,
                  net_lwip_dhcp_owned() ? RTPROT_DHCP : RTPROT_BOOT,
                  RT_SCOPE_UNIVERSE, gw_metric, 0);
    put_route(b, NLM_F_MULTI, seq, pid, ip & mask, prefix_len(mask), 0, ip,
              idx, RTPROT_KERNEL, RT_SCOPE_LINK, 0, 0);
}

static void do_dump(nlsock_t *s, const nlmsghdr_k *h, uint32_t plen) {
    nlbuf_t b = {0};
    uint8_t family = plen >= 1 ? ((const uint8_t *)(h + 1))[0] : 0;
    int v4 = family == 0 || family == AF_INET_K;
    switch (h->type) {
    case RTM_GETLINK:
        for (int i = 0; i < net_interface_count(); i++)
            put_link(&b, net_get_interface(i), NLM_F_MULTI, h->seq, s->portid);
        break;
    case RTM_GETADDR:
        if (v4)
            for (int i = 0; i < net_interface_count(); i++)
                put_addr(&b, net_get_interface(i), NLM_F_MULTI, h->seq, s->portid);
        break;
    case RTM_GETROUTE:
        if (v4) dump_routes(&b, h->seq, s->portid);
        break;
    default:
        /* Neighbours, rules, qdiscs...: an empty table. */
        break;
    }
    nb_begin(&b, NLMSG_DONE, NLM_F_MULTI, h->seq, s->portid);
    int32_t *zero = (int32_t *)nb_put(&b, 4);
    if (zero) *zero = 0;
    nb_end(&b);
    queue_reply(s, &b);
}

/* ── requests ────────────────────────────────────────────────────────────── */

static int do_getlink(nlsock_t *s, const nlmsghdr_k *h, const ifinfomsg_k *ifi,
                      const attrs_t *a) {
    netif_t *iface = NULL;
    if (ifi->index > 0) iface = netdev_by_index(ifi->index);
    else if (a->p[IFLA_IFNAME]) {
        char name[16];
        uint32_t n = a->len[IFLA_IFNAME] < 15 ? a->len[IFLA_IFNAME] : 15;
        memcpy(name, a->p[IFLA_IFNAME], n);
        name[n] = 0;
        iface = net_find_interface(name);
    }
    if (!iface) return -19;                            /* -ENODEV */
    nlbuf_t b = {0};
    put_link(&b, iface, 0, h->seq, s->portid);
    queue_reply(s, &b);
    return 0;
}

static int do_setlink(const ifinfomsg_k *ifi, const attrs_t *a) {
    netif_t *iface = ifi->index > 0 ? netdev_by_index(ifi->index) : NULL;
    if (!iface && a->p[IFLA_IFNAME]) {
        char name[16];
        uint32_t n = a->len[IFLA_IFNAME] < 15 ? a->len[IFLA_IFNAME] : 15;
        memcpy(name, a->p[IFLA_IFNAME], n);
        name[n] = 0;
        iface = net_find_interface(name);
    }
    if (!iface) return -19;
    if (!is_root()) return -1;                         /* -EPERM */
    uint32_t mtu;
    if (attr_u32(a, IFLA_MTU, &mtu) && mtu != iface->mtu)
        return -95;                                    /* fixed by the driver */
    uint32_t change = ifi->change ? ifi->change : 0xFFFFFFFFu;
    if (change & IFF_UP) {
        int up = (ifi->flags & IFF_UP) != 0;
        if (net_lwip_iface_is_ip(iface))
            net_lwip_set_up(up);
        else if (up)
            return -95;          /* only the interface lwIP runs on comes up */
    }
    return 0;
}

static int do_newaddr(const nlmsghdr_k *h, const ifaddrmsg_k *ifa,
                      const attrs_t *a, int del) {
    if (ifa->family != AF_INET_K) return -97;          /* -EAFNOSUPPORT */
    if (!is_root()) return -1;
    netif_t *iface = netdev_by_index((int)ifa->index);
    if (!iface) return -19;
    uint32_t want = 0;
    int have = attr_u32(a, IFA_LOCAL, &want) || attr_u32(a, IFA_ADDRESS, &want);
    uint32_t ip, mask, gw;
    net_lwip_get_config(&ip, &mask, &gw);
    if (!net_lwip_iface_is_ip(iface))
        return del ? -99 : -95;
    if (del) {
        if (!ip || (have && want != ip)) return -99;   /* -EADDRNOTAVAIL */
        /* Routes through the address's subnet go with it, as on Linux. */
        net_lwip_set_gw(0);
        gw_metric = 0;
        return net_lwip_set_addr(0, 0);
    }
    if (!have || ifa->prefixlen > 32) return -22;
    uint32_t nmask = mask_of(ifa->prefixlen);
    if (ip == want && mask == nmask)
        return (h->flags & NLM_F_EXCL) ? -17 : 0;      /* -EEXIST */
    /* lwIP holds one address: a different one replaces it. */
    if (ip && ip != want && gw && (gw & nmask) != (want & nmask)) {
        net_lwip_set_gw(0);
        gw_metric = 0;
    }
    return net_lwip_set_addr(want, nmask);
}

static int do_route(const nlmsghdr_k *h, const rtmsg_k *rt, const attrs_t *a,
                    int del) {
    if (rt->family != AF_INET_K) return -97;
    uint32_t table = rt->table, v;
    if (attr_u32(a, RTA_TABLE, &v)) table = v;
    if (table != RT_TABLE_MAIN && table != RT_TABLE_UNSPEC) return -95;
    if (!is_root()) return -1;
    uint32_t ip, mask, gw;
    net_lwip_get_config(&ip, &mask, &gw);
    uint32_t want_gw = 0, oif = 0, dst = 0, metric = 0;
    int has_gw = attr_u32(a, RTA_GATEWAY, &want_gw);
    attr_u32(a, RTA_OIF, &oif);
    attr_u32(a, RTA_DST, &dst);
    attr_u32(a, RTA_PRIORITY, &metric);
    netif_t *eth = ip_iface();
    if (oif && (!eth || (int)oif != netdev_index(eth)))
        return del ? -3 : -95;
    if (rt->dst_len == 0) {                            /* the default route */
        if (del) {
            if (!gw || (has_gw && want_gw != gw)) return -3;   /* -ESRCH */
            gw_metric = 0;
            return net_lwip_set_gw(0);
        }
        if (!has_gw) return -95;    /* lwIP routes off-link only via a gateway */
        if (gw && !(h->flags & NLM_F_REPLACE)) return -17;     /* -EEXIST */
        if (!ip || (want_gw & mask) != (ip & mask)) return -101; /* -ENETUNREACH */
        gw_metric = metric;
        return net_lwip_set_gw(want_gw);
    }
    /* The connected subnet is there while the address is. */
    if (ip && rt->dst_len == prefix_len(mask) && (dst & mask) == (ip & mask) &&
        !has_gw)
        return del ? -95 : ((h->flags & NLM_F_EXCL) ? -17 : 0);
    return del ? -3 : -95;
}

/* `ip route get`: the route a packet to RTA_DST would take. */
static int do_route_get(nlsock_t *s, const nlmsghdr_k *h, const attrs_t *a) {
    uint32_t dst = 0, ip, mask, gw;
    attr_u32(a, RTA_DST, &dst);
    net_lwip_get_config(&ip, &mask, &gw);
    netif_t *eth = ip_iface();
    if (!eth || !ip || !net_lwip_is_up()) return -101;
    uint32_t via = (dst & mask) == (ip & mask) ? 0 : gw;
    if ((dst & mask) != (ip & mask) && !gw) return -101;
    nlbuf_t b = {0};
    put_route(&b, 0, h->seq, s->portid, dst, 32, via, ip, netdev_index(eth),
              RTPROT_BOOT, RT_SCOPE_UNIVERSE, 0, RTM_F_CLONED);
    queue_reply(s, &b);
    return 0;
}

static int handle_msg(nlsock_t *s, const nlmsghdr_k *h) {
    uint32_t plen = h->len - sizeof(*h);
    const uint8_t *body = (const uint8_t *)(h + 1);
    attrs_t a;
    if (!(h->flags & NLM_F_REQUEST)) return 0;         /* not for the kernel */
    if (h->type < RTM_NEWLINK) return 0;               /* NOOP, ERROR, DONE */

    int get = (h->type & 3) == 2;                      /* RTM_GET* */
    if (get && (h->flags & NLM_F_DUMP) == NLM_F_DUMP) {
        do_dump(s, h, plen);
        return 1;                                      /* answered */
    }
    switch (h->type) {
    case RTM_GETLINK: case RTM_NEWLINK: case RTM_SETLINK: case RTM_DELLINK: {
        if (plen < sizeof(ifinfomsg_k)) return -22;
        const ifinfomsg_k *ifi = (const ifinfomsg_k *)body;
        parse_attrs(&a, body + sizeof(*ifi), plen - sizeof(*ifi));
        if (h->type == RTM_GETLINK) {
            int r = do_getlink(s, h, ifi, &a);
            return r < 0 ? r : 1;
        }
        if (h->type == RTM_DELLINK) return -95;
        if (h->type == RTM_NEWLINK && ifi->index <= 0 && !a.p[IFLA_IFNAME])
            return -95;                    /* creating links (veth, vlan...) */
        if (h->type == RTM_NEWLINK && (h->flags & NLM_F_CREATE) &&
            ifi->index <= 0)
            return -95;
        return do_setlink(ifi, &a);
    }
    case RTM_NEWADDR: case RTM_DELADDR: {
        if (plen < sizeof(ifaddrmsg_k)) return -22;
        const ifaddrmsg_k *ifa = (const ifaddrmsg_k *)body;
        parse_attrs(&a, body + sizeof(*ifa), plen - sizeof(*ifa));
        return do_newaddr(h, ifa, &a, h->type == RTM_DELADDR);
    }
    case RTM_NEWROUTE: case RTM_DELROUTE: case RTM_GETROUTE: {
        if (plen < sizeof(rtmsg_k)) return -22;
        const rtmsg_k *rt = (const rtmsg_k *)body;
        parse_attrs(&a, body + sizeof(*rt), plen - sizeof(*rt));
        if (h->type == RTM_GETROUTE) {
            int r = do_route_get(s, h, &a);
            return r < 0 ? r : 1;
        }
        return do_route(h, rt, &a, h->type == RTM_DELROUTE);
    }
    case RTM_GETADDR:
        do_dump(s, h, plen);       /* a single-address get is a dump here */
        return 1;
    }
    return -95;
}

/* ── xsock ops ───────────────────────────────────────────────────────────── */

typedef struct {
    uint16_t family, pad;
    uint32_t pid, groups;
} sockaddr_nl_k;

static int nl_bind(void *x, const void *addr, uint32_t alen) {
    nlsock_t *s = (nlsock_t *)x;
    sockaddr_nl_k sa;
    if (alen < sizeof(sa)) return -22;
    memcpy(&sa, addr, sizeof(sa));
    if (sa.family != AF_NETLINK_K) return -22;
    if (sa.pid) {
        if (s->portid && s->portid != sa.pid) return -22;
        if (!s->portid && portid_used(sa.pid)) return -98;  /* -EADDRINUSE */
        s->portid = sa.pid;
    } else {
        autobind(s);
    }
    s->groups = sa.groups;
    return 0;
}

static int nl_connect(void *x, const void *addr, uint32_t alen) {
    nlsock_t *s = (nlsock_t *)x;
    sockaddr_nl_k sa;
    if (alen < sizeof(sa)) return -22;
    memcpy(&sa, addr, sizeof(sa));
    if (sa.family != AF_NETLINK_K) return -22;
    if (sa.pid) return -111;       /* only the kernel (port 0) talks back */
    autobind(s);
    return 0;
}

static int nl_getname(void *x, int peer, void *addr, uint32_t *alen) {
    nlsock_t *s = (nlsock_t *)x;
    sockaddr_nl_k sa = { AF_NETLINK_K, 0, peer ? 0 : s->portid,
                         peer ? 0 : s->groups };
    memcpy(addr, &sa, sizeof(sa));
    *alen = sizeof(sa);
    return 0;
}

static int nl_send(void *x, const void *buf, uint32_t len,
                   const void *addr, uint32_t alen) {
    nlsock_t *s = (nlsock_t *)x;
    if (addr && alen) {
        sockaddr_nl_k sa;
        if (alen < sizeof(sa)) return -22;
        memcpy(&sa, addr, sizeof(sa));
        if (sa.family != AF_NETLINK_K) return -22;
        if (sa.pid) return -111;
    }
    autobind(s);
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t left = len;
    while (left >= sizeof(nlmsghdr_k)) {
        nlmsghdr_k h;
        memcpy(&h, p, sizeof(h));
        if (h.len < sizeof(h) || h.len > left) break;
        /* Work on an aligned copy: user buffers need not be. */
        nlmsghdr_k *m = (nlmsghdr_k *)kmalloc(h.len);
        if (!m) return -105;                           /* -ENOBUFS */
        memcpy(m, p, h.len);
        int r = handle_msg(s, m);
        if (r < 0 || (r == 0 && (m->flags & NLM_F_ACK)))
            reply_error(s, m, r < 0 ? r : 0);
        kfree(m);
        uint32_t step = ALIGN4(h.len);
        if (step >= left) break;
        p += step;
        left -= step;
    }
    return (int)len;
}

static int nl_recv(void *x, void *buf, uint32_t len, void *addr,
                   uint32_t *alen, int peek) {
    nlsock_t *s = (nlsock_t *)x;
    if (!s->head) {
        if (s->drops) { s->drops = 0; return -105; }  /* -ENOBUFS */
        return -11;
    }
    nlmsg_q_t *m = s->head;
    uint32_t n = m->len < len ? m->len : len;
    memcpy(buf, m->data, n);
    if (addr) {
        sockaddr_nl_k sa = { AF_NETLINK_K, 0, 0, 0 };
        memcpy(addr, &sa, sizeof(sa));
        *alen = sizeof(sa);
    }
    int full = (int)m->len;
    if (!peek) {
        s->head = m->next;
        if (!s->head) s->tail = NULL;
        s->queued -= m->len;
        kfree(m);
    }
    return full;
}

static int nl_read_ready(void *x) {
    nlsock_t *s = (nlsock_t *)x;
    return s->head != NULL || s->drops;
}

static int nl_setopt(void *x, int level, int name, const void *val, uint32_t len) {
    (void)x; (void)name; (void)val; (void)len;
    /* NETLINK_ADD_MEMBERSHIP, EXT_ACK, CAP_ACK, GET_STRICT_CHK, ...:
     * accepted; no notifications are ever multicast. */
    if (level == SOL_NETLINK_K) return 0;
    return 1;
}

static int nl_getopt(void *x, int level, int name, void *val, uint32_t *len) {
    (void)x; (void)name;
    if (level != SOL_NETLINK_K) return 1;
    int32_t v = 0;
    uint32_t n = *len < 4 ? *len : 4;
    memcpy(val, &v, n);
    *len = n;
    return 0;
}

static void nl_release(void *x) {
    nlsock_t *s = (nlsock_t *)x;
    for (nlsock_t **pp = &nl_all; *pp; pp = &(*pp)->next)
        if (*pp == s) { *pp = s->next; break; }
    while (s->head) {
        nlmsg_q_t *m = s->head;
        s->head = m->next;
        kfree(m);
    }
    kfree(s);
}

static const xsock_ops_t nl_ops = {
    nl_bind, nl_connect, nl_getname, nl_send, nl_recv, nl_read_ready,
    nl_setopt, nl_getopt, nl_release,
};

int netlink_create(int type, int protocol, const xsock_ops_t **ops, void **x) {
    if (type != 2 && type != 3) return -94;            /* DGRAM, RAW */
    if (protocol != NETLINK_ROUTE_K) return -93;       /* -EPROTONOSUPPORT */
    nlsock_t *s = (nlsock_t *)kmalloc(sizeof(*s));
    if (!s) return -12;
    memset(s, 0, sizeof(*s));
    s->next = nl_all;
    nl_all = s;
    *ops = &nl_ops;
    *x = s;
    return 0;
}

/* ── interface ioctls ────────────────────────────────────────────────────── */

#define SIOCADDRT      0x890B
#define SIOCDELRT      0x890C
#define SIOCGIFNAME    0x8910
#define SIOCGIFCONF    0x8912
#define SIOCGIFFLAGS   0x8913
#define SIOCSIFFLAGS   0x8914
#define SIOCGIFADDR    0x8915
#define SIOCSIFADDR    0x8916
#define SIOCGIFDSTADDR 0x8917
#define SIOCSIFDSTADDR 0x8918
#define SIOCGIFBRDADDR 0x8919
#define SIOCSIFBRDADDR 0x891A
#define SIOCGIFNETMASK 0x891B
#define SIOCSIFNETMASK 0x891C
#define SIOCGIFMETRIC  0x891D
#define SIOCSIFMETRIC  0x891E
#define SIOCGIFMTU     0x8921
#define SIOCSIFMTU     0x8922
#define SIOCGIFHWADDR  0x8927
#define SIOCGIFINDEX   0x8933
#define SIOCGIFTXQLEN  0x8942
#define SIOCSIFTXQLEN  0x8943
#define SIOCGIFMAP     0x8970
#define SIOCSIFMAP     0x8971

typedef struct {
    char name[16];
    union {
        struct { uint16_t family, port; uint32_t addr; uint8_t zero[8]; } in;
        struct { uint16_t family; uint8_t data[14]; } sa;
        int16_t  flags;
        int32_t  ivalue;
        uint8_t  raw[16];
    } u;
} ifreq_k;

/* struct rtentry on i386: rt_dst at 4, rt_gateway at 20, rt_genmask at 36,
 * rt_flags at 52, rt_metric at 64. */
#define RTENTRY_SIZE 92
#define RTF_UP       0x1
#define RTF_GATEWAY  0x2

static int route_ioctl(uint32_t req, void *uarg) {
    uint8_t rt[RTENTRY_SIZE];
    if (copy_from_user(rt, uarg, sizeof(rt)) < 0) return -14;
    if (!is_root()) return -1;
    uint32_t dst, gw, genmask, ip, mask, cur;
    uint16_t flags;
    int16_t metric;
    memcpy(&dst, rt + 4 + 4, 4);
    memcpy(&gw, rt + 20 + 4, 4);
    memcpy(&genmask, rt + 36 + 4, 4);
    memcpy(&flags, rt + 52, 2);
    memcpy(&metric, rt + 64, 2);
    net_lwip_get_config(&ip, &mask, &cur);
    if (dst != 0 || genmask != 0)
        return -95;                       /* only the default route */
    if (req == SIOCDELRT) {
        if (!cur || ((flags & RTF_GATEWAY) && gw != cur)) return -3;
        gw_metric = 0;
        return net_lwip_set_gw(0);
    }
    if (!(flags & RTF_GATEWAY) || !gw) return -95;
    if (cur) return -17;
    if (!ip || (gw & mask) != (ip & mask)) return -101;
    gw_metric = metric > 0 ? (uint32_t)(metric - 1) : 0;   /* route(8) adds 1 */
    return net_lwip_set_gw(gw);
}

static int ifconf_ioctl(void *uarg) {
    struct { int32_t len; uint32_t buf; } ifc;
    if (copy_from_user(&ifc, uarg, sizeof(ifc)) < 0) return -14;
    uint32_t ip, mask, gw;
    net_lwip_get_config(&ip, &mask, &gw);
    int32_t used = 0;
    for (int i = 0; i < net_interface_count(); i++) {
        netif_t *iface = net_get_interface(i);
        if (!net_lwip_iface_is_ip(iface) || !ip) continue;   /* IPv4 only */
        if (ifc.buf) {
            if (used + (int32_t)sizeof(ifreq_k) > ifc.len) break;
            ifreq_k r;
            memset(&r, 0, sizeof(r));
            strncpy(r.name, iface->name, 15);
            r.u.in.family = AF_INET_K;
            r.u.in.addr = ip;
            if (copy_to_user((void *)(uintptr_t)(ifc.buf + (uint32_t)used),
                             &r, sizeof(r)) < 0)
                return -14;
        }
        used += (int32_t)sizeof(ifreq_k);
    }
    ifc.len = used;
    return copy_to_user(uarg, &ifc, sizeof(ifc.len)) < 0 ? -14 : 0;
}

int netdev_ioctl(uint32_t req, void *uarg) {
    if (req == SIOCADDRT || req == SIOCDELRT) return route_ioctl(req, uarg);
    if (req == SIOCGIFCONF) return ifconf_ioctl(uarg);
    switch (req) {
    case SIOCGIFNAME: case SIOCGIFFLAGS: case SIOCSIFFLAGS: case SIOCGIFADDR:
    case SIOCSIFADDR: case SIOCGIFDSTADDR: case SIOCSIFDSTADDR:
    case SIOCGIFBRDADDR: case SIOCSIFBRDADDR: case SIOCGIFNETMASK:
    case SIOCSIFNETMASK: case SIOCGIFMETRIC: case SIOCSIFMETRIC:
    case SIOCGIFMTU: case SIOCSIFMTU: case SIOCGIFHWADDR: case SIOCGIFINDEX:
    case SIOCGIFTXQLEN: case SIOCSIFTXQLEN: case SIOCGIFMAP: case SIOCSIFMAP:
        break;
    default:
        return 1;
    }
    ifreq_k r;
    if (copy_from_user(&r, uarg, sizeof(r)) < 0) return -14;
    netif_t *iface;
    if (req == SIOCGIFNAME) {
        iface = netdev_by_index(r.u.ivalue);
        if (!iface) return -19;
        memset(r.name, 0, sizeof(r.name));
        strncpy(r.name, iface->name, 15);
        return copy_to_user(uarg, &r, sizeof(r)) < 0 ? -14 : 0;
    }
    r.name[15] = 0;
    iface = net_find_interface(r.name);
    if (!iface) return -19;                            /* -ENODEV */
    int isip = net_lwip_iface_is_ip(iface);
    uint32_t ip = 0, mask = 0, gw = 0;
    if (isip) net_lwip_get_config(&ip, &mask, &gw);
    int set = req == SIOCSIFFLAGS || req == SIOCSIFADDR || req == SIOCSIFDSTADDR ||
              req == SIOCSIFBRDADDR || req == SIOCSIFNETMASK ||
              req == SIOCSIFMETRIC || req == SIOCSIFMTU || req == SIOCSIFTXQLEN ||
              req == SIOCSIFMAP;
    if (set && !is_root()) return -1;
    switch (req) {
    case SIOCGIFFLAGS:
        r.u.flags = (int16_t)if_flags(iface);
        break;
    case SIOCSIFFLAGS: {
        int up = (r.u.flags & IFF_UP) != 0;
        if (isip) net_lwip_set_up(up);
        else if (up) return -95;
        return 0;
    }
    case SIOCGIFADDR: case SIOCGIFDSTADDR: case SIOCGIFBRDADDR:
    case SIOCGIFNETMASK:
        if (!ip) return -99;                           /* -EADDRNOTAVAIL */
        memset(&r.u, 0, sizeof(r.u));
        r.u.in.family = AF_INET_K;
        r.u.in.addr = req == SIOCGIFNETMASK ? mask
                    : req == SIOCGIFBRDADDR ? (ip | ~mask) : ip;
        break;
    case SIOCSIFADDR: case SIOCSIFNETMASK: {
        if (!isip) return -95;
        if (r.u.in.family != AF_INET_K) return -22;
        uint32_t v = r.u.in.addr;
        if (req == SIOCSIFADDR) {
            /* A new address without a netmask yet gets its class's. */
            uint32_t m = mask;
            if (!m) {
                uint8_t a = (uint8_t)(v & 0xFF);
                m = a < 128 ? mask_of(8) : a < 192 ? mask_of(16) : mask_of(24);
            }
            if (!v) { net_lwip_set_gw(0); gw_metric = 0; }
            return net_lwip_set_addr(v, v ? m : 0);
        }
        if (!ip) return -99;
        return net_lwip_set_addr(ip, v);
    }
    case SIOCSIFDSTADDR: case SIOCSIFBRDADDR: case SIOCSIFMETRIC:
    case SIOCSIFTXQLEN: case SIOCSIFMAP:
        return 0;                 /* derived from the address / meaningless */
    case SIOCGIFMETRIC:
        r.u.ivalue = 0;
        break;
    case SIOCGIFMTU:
        r.u.ivalue = (int32_t)iface->mtu;
        break;
    case SIOCSIFMTU:
        return (uint32_t)r.u.ivalue == iface->mtu ? 0 : -22;
    case SIOCGIFHWADDR:
        memset(&r.u, 0, sizeof(r.u));
        r.u.sa.family = ARPHRD_ETHER;
        memcpy(r.u.sa.data, iface->mac, 6);
        break;
    case SIOCGIFINDEX:
        r.u.ivalue = netdev_index(iface);
        break;
    case SIOCGIFTXQLEN:
        r.u.ivalue = 1000;
        break;
    case SIOCGIFMAP:
        memset(&r.u, 0, sizeof(r.u));
        break;
    }
    return copy_to_user(uarg, &r, sizeof(r)) < 0 ? -14 : 0;
}

/* ── /proc/net ───────────────────────────────────────────────────────────── */

uint32_t netdev_proc_dev(char *buf, uint32_t cap) {
    int pos = snprintf(buf, cap,
        "Inter-|   Receive                                                |  Transmit\n"
        " face |bytes    packets errs drop fifo frame compressed multicast|"
        "bytes    packets errs drop fifo colls carrier compressed\n");
    for (int i = 0; i < net_interface_count() && (uint32_t)pos < cap; i++) {
        netif_t *f = net_get_interface(i);
        pos += snprintf(buf + pos, cap - (uint32_t)pos,
            "%6s: %7u %7u    0 %4u    0     0          0         0 "
            "%8u %7u    0    0    0     0       0          0\n",
            f->name, f->rx_bytes, f->rx_packets, f->rx_dropped,
            f->tx_bytes, f->tx_packets);
    }
    return (uint32_t)pos < cap ? (uint32_t)pos : cap;
}

uint32_t netdev_proc_route(char *buf, uint32_t cap) {
    int pos = snprintf(buf, cap,
        "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask"
        "\t\tMTU\tWindow\tIRTT                                                       \n");
    uint32_t ip, mask, gw;
    netif_t *eth = ip_iface();
    net_lwip_get_config(&ip, &mask, &gw);
    if (eth && ip && net_lwip_is_up()) {
        if (gw)
            pos += snprintf(buf + pos, cap - (uint32_t)pos,
                "%s\t00000000\t%08X\t0003\t0\t0\t%u\t00000000\t0\t0\t0"
                "                                                                               \n",
                eth->name, gw, gw_metric);
        pos += snprintf(buf + pos, cap - (uint32_t)pos,
            "%s\t%08X\t00000000\t0001\t0\t0\t0\t%08X\t0\t0\t0"
            "                                                                               \n",
            eth->name, ip & mask, mask);
    }
    return (uint32_t)pos < cap ? (uint32_t)pos : cap;
}
