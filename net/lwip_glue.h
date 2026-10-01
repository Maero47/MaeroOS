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

/* bind(): 1 for INADDR_ANY, a loopback address or eth0's own (network
 * byte order, as in sockaddr_in). */
int net_lwip_addr_is_local(uint32_t addr);
