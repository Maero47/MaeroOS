#include "lwip_glue.h"
#include "net.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../kernel/printk.h"
#include "../fs/vfs.h"

#include "lwip/init.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"
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
    lwif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
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
        vfs_close(node);
        return;
    }
    if (vfs_truncate(node, 0) < 0 ||
        vfs_write(node, 0, len, (const uint8_t *)resolv_text) != len)
        printk_klog("[NET] writing /etc/resolv.conf failed\n");
    else
        printk_klog("[NET] /etc/resolv.conf written from DHCP\n");
    vfs_close(node);
}

void net_lwip_poll(void) {
    if (!lwip_inited)
        return;
    sys_check_timeouts();
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
