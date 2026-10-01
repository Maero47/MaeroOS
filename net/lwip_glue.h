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
