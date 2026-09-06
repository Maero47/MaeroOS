#include "lwip_glue.h"
#include "net.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../kernel/printk.h"

#include "lwip/init.h"
#include "lwip/dhcp.h"
#include "lwip/ip4_addr.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

static struct netif lwip_eth0;
static netif_t *host_eth0;
static int lwip_ready;

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
    host_eth0 = net_find_interface("eth0");
    if (!host_eth0) {
        printk("[LWIP] no eth0; skipping lwIP init\n");
        return;
    }

    ip4_addr_t ipaddr, netmask, gw;
    IP4_ADDR(&ipaddr, 0, 0, 0, 0);
    IP4_ADDR(&netmask, 0, 0, 0, 0);
    IP4_ADDR(&gw, 0, 0, 0, 0);

    lwip_init();
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

void net_lwip_poll(void) {
    if (!lwip_ready)
        return;
    sys_check_timeouts();
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
