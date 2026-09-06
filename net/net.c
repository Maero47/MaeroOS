#include "net.h"
#include "../arch/i686/cpu/pit.h"
#include "../proc/process.h"
#include "../proc/scheduler.h"
#include "lwip_glue.h"
#include "socket.h"
#include "../lib/string.h"
#include "../kernel/printk.h"

static netif_t interfaces[NET_MAX_INTERFACES];
static int ninterfaces;

void net_init(void) {
    memset(interfaces, 0, sizeof(interfaces));
    ninterfaces = 0;
    net_sockets_init();
}

netif_t *net_register(const char *name, const uint8_t mac[6], uint32_t mtu,
                      void *driver_data, netif_send_fn send,
                      netif_poll_fn poll) {
    if (ninterfaces >= NET_MAX_INTERFACES)
        return 0;

    netif_t *iface = &interfaces[ninterfaces++];
    memset(iface, 0, sizeof(*iface));
    strncpy(iface->name, name, sizeof(iface->name) - 1);
    memcpy(iface->mac, mac, 6);
    iface->mtu = mtu;
    iface->driver_data = driver_data;
    iface->send = send;
    iface->poll = poll;

    printk("[NET] registered %s mac=%02x:%02x:%02x:%02x:%02x:%02x mtu=%u\n",
           iface->name,
           iface->mac[0], iface->mac[1], iface->mac[2],
           iface->mac[3], iface->mac[4], iface->mac[5],
           (unsigned)iface->mtu);
    return iface;
}

int net_interface_count(void) {
    return ninterfaces;
}

netif_t *net_get_interface(int index) {
    if (index < 0 || index >= ninterfaces)
        return 0;
    return &interfaces[index];
}

netif_t *net_find_interface(const char *name) {
    for (int i = 0; i < ninterfaces; i++) {
        if (strcmp(interfaces[i].name, name) == 0)
            return &interfaces[i];
    }
    return 0;
}

void net_poll_all(void) {
    /* lwIP is not reentrant: keep the PIT tick from scheduling another
     * lwIP user (knetd vs. socket syscalls) mid-stack. */
    preempt_disable();
    for (int i = 0; i < ninterfaces; i++) {
        if (interfaces[i].poll)
            interfaces[i].poll(&interfaces[i]);
    }
    net_lwip_poll();
    preempt_enable();
}

int net_send(netif_t *iface, const void *data, uint32_t len) {
    if (!iface || !iface->send || !data || len == 0)
        return -22;
    if (len > iface->mtu + 14)
        return -90;

    int ret = iface->send(iface, data, len);
    if (ret >= 0) {
        iface->tx_packets++;
        iface->tx_bytes += len;
    }
    return ret;
}

void net_receive_ethernet(netif_t *iface, const void *data, uint32_t len) {
    (void)data;
    if (!iface)
        return;
    if (len < 14) {
        iface->rx_dropped++;
        return;
    }

    iface->rx_packets++;
    iface->rx_bytes += len;
    net_lwip_input(iface, data, len);
}

/*
 * Network bottom-half thread: drives lwIP timers and drains the NIC ring
 * continuously so DHCP/TCP progress without any process touching sockets.
 * Woken early by io_wake() (NIC IRQ); otherwise ticks every 50ms.
 */
void knetd(void) {
    for (;;) {
        net_poll_all();
        current_proc->wake_tick = pit_ticks() + 5;
        sleep_on(&io_activity);
    }
}
