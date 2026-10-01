#include "lwip_glue.h"
#include "net.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../kernel/printk.h"
#include "../fs/vfs.h"

#include "lwip/init.h"
#include "lwip/dhcp.h"
#include "lwip/prot/dhcp.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip6_addr.h"
#include "lwip/nd6.h"
#include "lwip/mld6.h"
#include "lwip/priv/nd6_priv.h"
#include "netif/ethernet.h"
#include "lwip/ethip6.h"
#include "../proc/scheduler.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

static struct netif lwip_eth0;
static netif_t *host_eth0;
static int lwip_inited;    /* lwip_init() done: pools and timers usable */
static int lwip_ready;     /* ... and eth0 attached */

static err_t maero_lwip_output(struct netif *lwif, struct pbuf *p) {
    (void)lwif;
    if (!host_eth0)
        return ERR_IF;

    uint8_t frame[1536];
    uint32_t pos = 0;
    for (struct pbuf *q = p; q; q = q->next) {
        if (pos + q->len > sizeof(frame))
            return ERR_BUF;
        memcpy(frame + pos, q->payload, q->len);
        pos += q->len;
    }

    return net_send(host_eth0, frame, pos) >= 0 ? ERR_OK : ERR_IF;
}

static err_t maero_lwip_init_if(struct netif *lwif) {
    if (!host_eth0)
        return ERR_IF;

    lwif->name[0] = 'e';
    lwif->name[1] = 'n';
    lwif->output = etharp_output;
    lwif->linkoutput = maero_lwip_output;
    lwif->hwaddr_len = 6;
    memcpy(lwif->hwaddr, host_eth0->mac, 6);
    lwif->mtu = host_eth0->mtu;
    lwif->output_ip6 = ethip6_output;
    /* MLD6: lwIP joins the solicited-node groups and reports them; the NIC
     * takes every multicast frame (e1000 MPE, rtl8139 MAR all ones). */
    lwif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
                  NETIF_FLAG_ETHERNET | NETIF_FLAG_MLD6 | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

void net_lwip_init(void) {
    /* The stack comes up even with no NIC: socket() only needs the memp
     * pools, and connect()/sendto() then fail with ENETUNREACH (no route),
     * as on a Linux box with no interface up.  Skipping lwip_init left every
     * pool empty, so each socket() failed with ENOMEM and a bogus "no free
     * UDP pcb" (72 of them in one no-network Firefox boot). */
    lwip_init();
    lwip_inited = 1;

    host_eth0 = net_find_interface("eth0");
    if (!host_eth0) {
        printk("[LWIP] no eth0; lwIP up with no interface\n");
        return;
    }

    ip4_addr_t ipaddr, netmask, gw;
    IP4_ADDR(&ipaddr, 0, 0, 0, 0);
    IP4_ADDR(&netmask, 0, 0, 0, 0);
    IP4_ADDR(&gw, 0, 0, 0, 0);

    if (!netif_add(&lwip_eth0, &ipaddr, &netmask, &gw, NULL,
                   maero_lwip_init_if, ethernet_input)) {
        printk("[LWIP] netif_add failed\n");
        return;
    }

    netif_set_default(&lwip_eth0);
    /* IPv6: the EUI-64 link-local address (DAD runs once the link is up),
     * then SLAAC from router advertisements; the router solicitations go
     * out from nd6_tmr after netif_set_link_up. */
    netif_create_ip6_linklocal_address(&lwip_eth0, 1);
    lwip_eth0.ip6_autoconfig_enabled = 1;
    netif_set_up(&lwip_eth0);
    netif_set_link_up(&lwip_eth0);
    dhcp_start(&lwip_eth0);
    lwip_ready = 1;
    printk("[LWIP] eth0 DHCP started\n");
}

void net_lwip_input(netif_t *iface, const void *data, uint32_t len) {
    if (!lwip_ready || iface != host_eth0 || !data || len == 0)
        return;

    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
    if (!p)
        return;
    pbuf_take(p, data, (u16_t)len);
    if (ethernet_input(p, &lwip_eth0) != ERR_OK)
        pbuf_free(p);
}

/*
 * /etc/resolv.conf from the DHCP lease.  lwIP's DHCP client hands the DNS
 * option to dns_setserver(); net_lwip_poll() notices a change and stages the
 * new file here, and knetd writes it out (net_lwip_write_resolv_conf) once it
 * is out of the preempt-disabled lwIP section, since the write can sleep on
 * the disk.
 */
static char resolv_text[256];
static uint32_t resolv_servers[DNS_MAX_SERVERS];
static volatile int resolv_pending;

static void resolv_check(void) {
    if (!dhcp_supplied_address(&lwip_eth0))
        return;

    uint32_t now[DNS_MAX_SERVERS];
    int any = 0, changed = 0;
    for (int i = 0; i < DNS_MAX_SERVERS; i++) {
        const ip_addr_t *a = dns_getserver((u8_t)i);
        now[i] = ip_addr_isany(a) ? 0 : ip4_addr_get_u32(ip_2_ip4(a));
        if (now[i]) any = 1;
        if (now[i] != resolv_servers[i]) changed = 1;
    }
    if (!any || !changed)
        return;

    int pos = snprintf(resolv_text, sizeof(resolv_text),
                       "# Written by the MaeroOS kernel from the DHCP lease on eth0.\n");
    for (int i = 0; i < DNS_MAX_SERVERS; i++) {
        resolv_servers[i] = now[i];
        if (!now[i])
            continue;
        ip4_addr_t a;
        ip4_addr_set_u32(&a, now[i]);
        pos += snprintf(resolv_text + pos, sizeof(resolv_text) - (uint32_t)pos,
                        "nameserver %u.%u.%u.%u\n",
                        ip4_addr1(&a), ip4_addr2(&a), ip4_addr3(&a), ip4_addr4(&a));
    }
    resolv_pending = 1;
}

void net_lwip_write_resolv_conf(void) {
    if (!resolv_pending)
        return;
    resolv_pending = 0;

    /* With a disk attached /etc is the disk's (writable); on an initrd-only
     * boot it is the read-only initrd and the shipped file stays. */
    vfs_node_t *node = vfs_open("/etc/resolv.conf");
    if (!node) {
        vfs_node_t *dir = vfs_open("/etc");
        if (dir && dir->create_fn &&
            dir->create_fn(dir, "resolv.conf", VFS_FLAG_FILE) == 0)
            node = vfs_open("/etc/resolv.conf");
        if (dir)
            vfs_close(dir);
    }
    if (!node || !node->write_fn || !node->truncate_fn) {
        printk_klog("[NET] /etc/resolv.conf is read-only; DHCP DNS not written\n");
        if (node)
            vfs_close(node);
        return;
    }

    /* Same servers as the file already names (every boot on the same
     * network): leave the disk alone. */
    uint32_t len = (uint32_t)strlen(resolv_text);
    char cur[sizeof(resolv_text)];
    if (node->size == len &&
        vfs_read(node, 0, len, (uint8_t *)cur) == len &&
        memcmp(cur, resolv_text, len) == 0) {
        printk_klog("[NET] /etc/resolv.conf from DHCP: unchanged\n");
        vfs_close(node);
        return;
    }
    if (vfs_truncate(node, 0) < 0 ||
        vfs_write(node, 0, len, (const uint8_t *)resolv_text) != len)
        printk_klog("[NET] writing /etc/resolv.conf failed\n");
    else
        printk_klog("[NET] /etc/resolv.conf from DHCP: written\n");
    vfs_close(node);
}

/* Deliver what lo and eth0's own-address loopback queued (NO_SYS: nobody
 * else does).  A delivery can queue the answer (SYN -> SYN|ACK -> ACK), so
 * go a few rounds; a reader asleep on io_activity is woken. */
static void loop_poll(void) {
    int moved = 0;
    for (int round = 0; round < 8; round++) {
        int queued = 0;
        struct netif *n;
        NETIF_FOREACH(n)
            if (n->loop_first) queued = 1;
        if (!queued)
            break;
        netif_poll_all();
        moved = 1;
    }
    if (moved)
        io_wake();
}

void net_lwip_poll(void) {
    if (!lwip_inited)
        return;
    sys_check_timeouts();
    loop_poll();
    if (!lwip_ready)
        return;
    static int bound;
    if (!bound && !ip4_addr_isany_val(*netif_ip4_addr(&lwip_eth0))) {
        char b[80];
        bound = 1;
        net_lwip_ipv4(b, sizeof(b));
        printk_klog("[LWIP] eth0 bound%s\n", b);
    }
    resolv_check();
}

int net_lwip_ipv4(char *buf, uint32_t cap) {
    if (!lwip_ready || cap == 0)
        return 0;

    const ip4_addr_t *ip = netif_ip4_addr(&lwip_eth0);
    const ip4_addr_t *mask = netif_ip4_netmask(&lwip_eth0);
    const ip4_addr_t *gw = netif_ip4_gw(&lwip_eth0);

    return snprintf(buf, cap,
                    " ip=%u.%u.%u.%u mask=%u.%u.%u.%u gw=%u.%u.%u.%u",
                    ip4_addr1(ip), ip4_addr2(ip), ip4_addr3(ip), ip4_addr4(ip),
                    ip4_addr1(mask), ip4_addr2(mask), ip4_addr3(mask), ip4_addr4(mask),
                    ip4_addr1(gw), ip4_addr2(gw), ip4_addr3(gw), ip4_addr4(gw));
}

void net_lwip_kick(void) {
    if (lwip_inited)
        loop_poll();
}

int net_lwip_addr6_is_local(const uint8_t a[16]) {
    ip6_addr_t ip;
    memcpy(ip.addr, a, 16);
    ip6_addr_clear_zone(&ip);
    if (ip6_addr_isany(&ip) || ip6_addr_isloopback(&ip) || ip6_addr_ismulticast(&ip))
        return 1;
    if (!lwip_ready)
        return 0;
    for (int i = 0; i < LWIP_IPV6_NUM_ADDRESSES; i++)
        if (!ip6_addr_isinvalid(netif_ip6_addr_state(&lwip_eth0, i)) &&
            !memcmp(netif_ip6_addr(&lwip_eth0, i)->addr, a, 16))
            return 1;
    return 0;
}

int net_lwip_ip6_addrs(netif_t *iface, net_ip6_info_t *out, int max) {
    if (!lwip_inited || !iface)
        return 0;
    if (iface->loopback) {
        if (max < 1) return 0;
        memset(&out[0], 0, sizeof(out[0]));
        out[0].addr[15] = 1;
        out[0].plen = 128;
        out[0].scope = 0x10;                       /* host */
        return 1;
    }
    if (!lwip_ready || iface != host_eth0)
        return 0;
    int n = 0;
    for (int i = 0; i < LWIP_IPV6_NUM_ADDRESSES && n < max; i++) {
        u8_t st = netif_ip6_addr_state(&lwip_eth0, i);
        if (ip6_addr_isinvalid(st))
            continue;
        const ip6_addr_t *a = netif_ip6_addr(&lwip_eth0, i);
        memcpy(out[n].addr, a->addr, 16);
        out[n].plen = 64;
        out[n].scope = ip6_addr_islinklocal(a) ? 0x20 : 0;   /* link / global */
        out[n].tentative = ip6_addr_istentative(st) != 0;
        /* SLAAC addresses have a lifetime; the link-local one is permanent. */
        out[n].dynamic = !ip6_addr_islinklocal(a);
#if LWIP_IPV6_ADDRESS_LIFETIMES
        out[n].valid_life = netif_ip6_addr_valid_life(&lwip_eth0, i);
        out[n].pref_life = netif_ip6_addr_pref_life(&lwip_eth0, i);
#endif
        n++;
    }
    return n;
}

int net_lwip_ip6_onlink(const uint8_t a[16]) {
    if (!lwip_ready)
        return 0;
    for (int i = 0; i < LWIP_IPV6_NUM_ADDRESSES; i++)
        if (ip6_addr_isvalid(netif_ip6_addr_state(&lwip_eth0, i)) &&
            !memcmp(netif_ip6_addr(&lwip_eth0, i)->addr, a, 8))
            return 1;
    return 0;
}

int net_lwip_ip6_router(uint8_t out[16]) {
    if (!lwip_ready)
        return 0;
    for (int i = 0; i < LWIP_ND6_NUM_ROUTERS; i++) {
        if (default_router_list[i].neighbor_entry &&
            default_router_list[i].invalidation_timer > 0) {
            memcpy(out, default_router_list[i].neighbor_entry->next_hop_address.addr, 16);
            return 1;
        }
    }
    return 0;
}

/* The lwIP netif behind an interface index (lo is lwIP's loop netif). */
struct netif *net_lwip_netif_of(netif_t *iface) {
    if (!lwip_inited || !iface)
        return NULL;
    if (iface->loopback)
        return netif_find("lo0");
    return (lwip_ready && iface == host_eth0) ? &lwip_eth0 : NULL;
}

int net_lwip_addr_is_local(uint32_t addr) {
    ip4_addr_t a;
    ip4_addr_set_u32(&a, addr);
    if (ip4_addr_isany_val(a) || ip4_addr1(&a) == 127 ||
        ip4_addr_ismulticast(&a) || addr == 0xFFFFFFFFu)
        return 1;
    return lwip_ready && ip4_addr_eq(&a, netif_ip4_addr(&lwip_eth0));
}

/* ── Interface configuration for netlink, SIOC* ioctls and /proc/net ──────
 * Addresses are in network byte order, as in sockaddr_in.  lwIP keeps one
 * IPv4 address per interface and its default gateway is the only route
 * besides the connected subnet; net/netlink.c maps rtnetlink onto that. */

int net_lwip_eth_zone(void) {
    return lwip_ready ? netif_get_index(&lwip_eth0) : 0;
}

int net_lwip_iface_is_ip(netif_t *iface) {
    return lwip_ready && iface && iface == host_eth0;
}

void net_lwip_get_config(uint32_t *ip, uint32_t *mask, uint32_t *gw) {
    *ip = *mask = *gw = 0;
    if (!lwip_ready)
        return;
    *ip = ip4_addr_get_u32(netif_ip4_addr(&lwip_eth0));
    *mask = ip4_addr_get_u32(netif_ip4_netmask(&lwip_eth0));
    *gw = ip4_addr_get_u32(netif_ip4_gw(&lwip_eth0));
}

int net_lwip_is_up(void) {
    return lwip_ready && netif_is_up(&lwip_eth0);
}

int net_lwip_dhcp_owned(void) {
    return lwip_ready && dhcp_supplied_address(&lwip_eth0);
}

/* Someone configures the interface by hand (ip addr, ifconfig, udhcpc's
 * script): the kernel's own DHCP client lets go of it, without a RELEASE,
 * so a later renewal cannot overwrite the address it was given. */
static void take_over(void) {
    /* Not dhcp_stop(): that sends a RELEASE and clears the address.  With
     * the state OFF lwIP's DHCP timers skip the interface (dhcp_coarse_tmr,
     * dhcp_fine_tmr) and replies no longer match a request. */
    struct dhcp *d = netif_dhcp_data(&lwip_eth0);
    if (!d || d->state == DHCP_STATE_OFF)
        return;
    d->state = DHCP_STATE_OFF;
    d->t0_timeout = d->t1_renew_time = d->t2_rebind_time = 0;
    d->request_timeout = 0;
    printk_klog("[NET] eth0 configured by hand; kernel DHCP client stopped\n");
}

int net_lwip_set_addr(uint32_t ip, uint32_t mask) {
    if (!lwip_ready)
        return -19;                                    /* -ENODEV */
    ip4_addr_t a, m;
    ip4_addr_set_u32(&a, ip);
    ip4_addr_set_u32(&m, mask);
    uint32_t cur_ip, cur_mask, cur_gw;
    net_lwip_get_config(&cur_ip, &cur_mask, &cur_gw);
    if (cur_ip == ip && cur_mask == mask)
        return 0;
    take_over();
    netif_set_netmask(&lwip_eth0, &m);
    netif_set_ipaddr(&lwip_eth0, &a);
    return 0;
}

int net_lwip_set_gw(uint32_t gw) {
    if (!lwip_ready)
        return -19;
    ip4_addr_t g;
    ip4_addr_set_u32(&g, gw);
    if (ip4_addr_get_u32(netif_ip4_gw(&lwip_eth0)) == gw)
        return 0;
    take_over();
    netif_set_gw(&lwip_eth0, &g);
    return 0;
}

void net_lwip_set_up(int up) {
    if (!lwip_ready)
        return;
    if (up) netif_set_up(&lwip_eth0);
    else    netif_set_down(&lwip_eth0);
}
