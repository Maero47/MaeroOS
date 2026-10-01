#pragma once

#include <stdint.h>
#include "net.h"

void net_lwip_init(void);
void net_lwip_input(netif_t *iface, const void *data, uint32_t len);
void net_lwip_poll(void);
int net_lwip_ipv4(char *buf, uint32_t cap);
/* Write /etc/resolv.conf if DHCP changed the DNS servers (may sleep: call
 * with preemption enabled). */
void net_lwip_write_resolv_conf(void);

/* bind(): 1 for INADDR_ANY, a loopback, multicast or the broadcast
 * address, or eth0's own (network byte order, as in sockaddr_in). */
int net_lwip_addr_is_local(uint32_t addr);
/* The same for IPv6: ::, ::1, multicast or one of eth0's addresses. */
int net_lwip_addr6_is_local(const uint8_t a[16]);
/* Run the loopback queues now (after a send to lo or our own address). */
void net_lwip_kick(void);

/* IPv6 addresses of an interface (lo: ::1/128) for netlink and
 * /proc/net/if_inet6.  scope is the Linux RT_SCOPE value (0 global, 0x20
 * link, 0x10 host).  Returns how many were written. */
typedef struct {
    uint8_t addr[16];
    uint8_t plen;
    uint8_t scope;
    uint8_t tentative;
    uint8_t dynamic;
    uint32_t valid_life, pref_life;   /* seconds, 0 = static */
} net_ip6_info_t;
int net_lwip_ip6_addrs(netif_t *iface, net_ip6_info_t *out, int max);
/* A default router learnt from router advertisements (its link-local
 * address), 0 when there is none. */
int net_lwip_ip6_router(uint8_t out[16]);
/* `a` is in the /64 of one of eth0's valid IPv6 addresses. */
int net_lwip_ip6_onlink(const uint8_t a[16]);
struct netif;
struct netif *net_lwip_netif_of(netif_t *iface);
/* eth0's lwIP netif index (the zone of its link-local addresses). */
int net_lwip_eth_zone(void);

/* eth0's configuration for netlink, SIOC* ioctls and /proc/net (network
 * byte order).  Setting an address or gateway by hand stops the kernel's
 * DHCP client on the interface. */
int net_lwip_iface_is_ip(netif_t *iface);   /* the interface lwIP runs on */
void net_lwip_get_config(uint32_t *ip, uint32_t *mask, uint32_t *gw);
int net_lwip_is_up(void);
int net_lwip_dhcp_owned(void);
int net_lwip_set_addr(uint32_t ip, uint32_t mask);
int net_lwip_set_gw(uint32_t gw);
void net_lwip_set_up(int up);
