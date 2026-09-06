#pragma once

#include <stdint.h>
#include "net.h"

void net_lwip_init(void);
void net_lwip_input(netif_t *iface, const void *data, uint32_t len);
void net_lwip_poll(void);
int net_lwip_ipv4(char *buf, uint32_t cap);

