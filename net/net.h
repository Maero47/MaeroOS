#pragma once

#include <stdint.h>
#include <stddef.h>

#define NET_MAX_INTERFACES 4
#define NET_MTU_ETHERNET 1500

typedef struct maero_netif netif_t;

typedef int (*netif_send_fn)(netif_t *iface, const void *data, uint32_t len);
typedef int (*netif_poll_fn)(netif_t *iface);

struct maero_netif {
    char name[16];
    uint8_t mac[6];
    uint32_t mtu;
    void *driver_data;
    netif_send_fn send;
    netif_poll_fn poll;
    uint32_t tx_packets;
    uint32_t tx_bytes;
    uint32_t rx_packets;
    uint32_t rx_bytes;
    uint32_t rx_dropped;
};

void net_init(void);
netif_t *net_register(const char *name, const uint8_t mac[6], uint32_t mtu,
                      void *driver_data, netif_send_fn send,
                      netif_poll_fn poll);
int net_interface_count(void);
netif_t *net_get_interface(int index);
netif_t *net_find_interface(const char *name);
void net_poll_all(void);
int net_send(netif_t *iface, const void *data, uint32_t len);
void net_receive_ethernet(netif_t *iface, const void *data, uint32_t len);

/* Network bottom-half kernel thread body (spawn if a NIC exists). */
void knetd(void);
