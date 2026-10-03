/*
 * virtio network device driver (Virtual I/O Device (VIRTIO) Version 1.2,
 * section 5.1), on the virtio PCI transport (virtio_pci.h).  Written from
 * the specification; no code was copied.
 *
 * Features asked for: VERSION_1, MAC (the address from device config),
 * STATUS (link state in device config, config-change interrupts), CTRL_VQ
 * + CTRL_RX (the control queue's RX mode commands).  Not asked for: any
 * offload (lwIP computes and checks every checksum itself; no GSO), so a
 * frame never exceeds 1514 bytes and every receive buffer is a fixed 2 KiB
 * one, without MRG_RXBUF; MQ (one queue pair); EVENT_IDX.
 *
 * Every frame carries a struct virtio_net_hdr in front: 12 bytes with
 * VERSION_1 (num_buffers is always there), 10 on a legacy device.  Transmit: header and frame in one 2 KiB
 * buffer, one descriptor; completions are reaped on the next send (the
 * transmit queue's interrupts are switched off).  Receive: up to RX_BUFS buffers
 * of 2 KiB, one descriptor each; the interrupt only wakes knetd, and
 * net_poll_all() drains the used ring in process context, as for the other
 * NICs, because lwIP is not reentrant.
 *
 * Multicast: IPv6 neighbour discovery needs the solicited-node groups, so
 * the receive filter is set to "all multicast" over the control queue
 * (VIRTIO_NET_CTRL_RX_ALLMULTI on, PROMISC off) — the equivalent of the
 * e1000's MPE and the Realtek parts' MAR all ones.  A device without
 * CTRL_RX keeps its default (QEMU's: promiscuous).
 */
#include "virtio_net.h"
#include "virtio_pci.h"
#include "virtqueue.h"
#include "../../arch/i686/include/io.h"
#include "../../include/kernel/config.h"
#include "../../kernel/printk.h"
#include "../../lib/printf.h"
#include "../../lib/string.h"
#include "../../net/net.h"
#include "../../proc/scheduler.h"
#include <stdint.h>

/* Feature bits (5.1.3). */
#define VIRTIO_NET_F_CSUM        (1ULL << 0)
#define VIRTIO_NET_F_GUEST_CSUM  (1ULL << 1)
#define VIRTIO_NET_F_MTU         (1ULL << 3)
#define VIRTIO_NET_F_MAC         (1ULL << 5)
#define VIRTIO_NET_F_MRG_RXBUF   (1ULL << 15)
#define VIRTIO_NET_F_STATUS      (1ULL << 16)
#define VIRTIO_NET_F_CTRL_VQ     (1ULL << 17)
#define VIRTIO_NET_F_CTRL_RX     (1ULL << 18)
#define VIRTIO_NET_F_MQ          (1ULL << 22)

/* Device configuration (5.1.4). */
#define CFG_MAC      0
#define CFG_STATUS   6
#define VIRTIO_NET_S_LINK_UP  1

/* Control queue (5.1.6.5). */
#define VIRTIO_NET_CTRL_RX          0
#define VIRTIO_NET_CTRL_RX_PROMISC  0
#define VIRTIO_NET_CTRL_RX_ALLMULTI 1
#define VIRTIO_NET_OK  0

#define Q_RX   0
#define Q_TX   1
#define Q_CTRL 2

#define NET_HDR_MAX 12           /* struct virtio_net_hdr with num_buffers */
#define BUF_SIZE    2048         /* two per page: never crosses one */
#define RX_BUFS     128
#define TX_BUFS     128
#define FRAME_MAX   1514

static struct virtio_pci vp;
static struct virtqueue rxq, txq, ctrlq;
static netif_t *vnet_netif;
static int present;
static uint8_t mac[6];
static int has_ctrl;
static uint32_t hdr_len;              /* 12 with VERSION_1, 10 on a legacy device */

static uint8_t *rx_mem, *tx_mem, *ctrl_mem;
static uint16_t tx_free[TX_BUFS];     /* stack of free TX buffer indices */
static uint16_t tx_nfree;
static uint32_t nrx, ntx;              /* buffers: min(ring size, RX_BUFS/TX_BUFS) */

static volatile int config_changed;
static int link_up;
static uint32_t link_changes;
static uint32_t rx_errors, tx_full;
static int allmulti_ok = -1, promisc_off_ok = -1;   /* -1: not sent */

/* Cookies are buffer index + 1 (virtq_get_used returns NULL for "none"). */
#define COOKIE(i)   ((void *)(uintptr_t)((i) + 1))
#define INDEX(c)    ((uint32_t)(uintptr_t)(c) - 1)

static uint8_t *rx_buf(uint32_t i) { return rx_mem + i * BUF_SIZE; }
static uint8_t *tx_buf(uint32_t i) { return tx_mem + i * BUF_SIZE; }

int virtio_net_present(void) {
    return present;
}

static void rx_post(uint32_t i) {
    struct virtq_buf b = { virtio_virt_to_phys(rx_buf(i)), BUF_SIZE };
    virtq_add(&rxq, &b, 0, 1, COOKIE(i));
}

static void tx_reclaim(void) {
    void *c;
    while ((c = virtq_get_used(&txq, 0)) != 0) {
        uint32_t i = INDEX(c);
        if (i < ntx && tx_nfree < ntx)
            tx_free[tx_nfree++] = (uint16_t)i;
    }
}

static int link_status(void) {
    if (!(vp.features & VIRTIO_NET_F_STATUS))
        return 1;                       /* no status field: always up */
    return (virtio_pci_config16(&vp, CFG_STATUS) & VIRTIO_NET_S_LINK_UP) != 0;
}

/* ── Data path ───────────────────────────────────────────────────────────── */
static int vnet_send(netif_t *iface, const void *data, uint32_t len) {
    (void)iface;
    if (!present)
        return -19;
    if (!data || len == 0 || len > FRAME_MAX)
        return -22;

    tx_reclaim();
    /* The device works the queue while we wait (QEMU: its main loop). */
    for (int i = 0; i < 200000 && tx_nfree == 0; i++) {
        __asm__ volatile("pause");
        tx_reclaim();
    }
    if (tx_nfree == 0) {
        tx_full++;
        return -11;
    }
    uint32_t i = tx_free[--tx_nfree];
    uint8_t *b = tx_buf(i);
    memset(b, 0, hdr_len);          /* no offloads, no GSO */
    memcpy(b + hdr_len, data, len);
    struct virtq_buf vb = { virtio_virt_to_phys(b), hdr_len + len };
    if (virtq_add(&txq, &vb, 1, 0, COOKIE(i)) < 0) {
        tx_free[tx_nfree++] = (uint16_t)i;
        tx_full++;
        return -11;
    }
    virtio_pci_publish(&vp, &txq);
    return (int)len;
}

static int vnet_poll(netif_t *iface) {
    (void)iface;
    if (!present)
        return 0;

    if (config_changed) {
        config_changed = 0;
        int up = link_status();
        if (up != link_up) {
            link_up = up;
            link_changes++;
            printk_klog("[VNET] link %s\n", up ? "up" : "down");
        }
    }

    int packets = 0;
    void *c;
    uint32_t len;
    while (packets < (int)nrx && (c = virtq_get_used(&rxq, &len)) != 0) {
        uint32_t i = INDEX(c);
        if (i >= nrx)
            continue;
        if (len >= hdr_len + 14 && len <= BUF_SIZE) {
            net_receive_ethernet(vnet_netif, rx_buf(i) + hdr_len, len - hdr_len);
            packets++;
        } else {
            rx_errors++;
            if (vnet_netif)
                vnet_netif->rx_dropped++;
        }
        rx_post(i);
    }
    if (packets || rx_errors)
        virtio_pci_publish(&vp, &rxq);
    tx_reclaim();
    return packets;
}

/* IRQ context: note a config change, wake knetd. */
static void vnet_irq(struct virtio_pci *v, uint8_t isr, void *ctx) {
    (void)v;
    (void)ctx;
    if (isr & VIRTIO_ISR_CONFIG)
        config_changed = 1;
    io_wake();
}

/* ── Control queue ───────────────────────────────────────────────────────── */
/* One command, synchronously: class/cmd header and a one-byte argument out,
 * the ack byte in.  Returns the ack (VIRTIO_NET_OK = 0) or -1. */
static int ctrl_cmd(uint8_t cls, uint8_t cmd, uint8_t arg) {
    if (!has_ctrl)
        return -1;
    uint8_t *hdr = ctrl_mem, *data = ctrl_mem + 16, *ack = ctrl_mem + 32;
    hdr[0] = cls;
    hdr[1] = cmd;
    data[0] = arg;
    *ack = 0xFF;
    struct virtq_buf b[3] = {
        { virtio_virt_to_phys(hdr), 2 },
        { virtio_virt_to_phys(data), 1 },
        { virtio_virt_to_phys(ack), 1 },
    };
    if (virtq_add(&ctrlq, b, 2, 1, COOKIE(0)) < 0)
        return -1;
    virtio_pci_publish(&vp, &ctrlq);
    /* QEMU answers inside the notify; allow a slower device ~1 s (the PIT
     * may not run yet: port 0x80 writes are ~1 us each). */
    for (int i = 0; i < 1000000; i++) {
        if (virtq_get_used(&ctrlq, 0))
            return *(volatile uint8_t *)ack;
        io_wait();
    }
    printk("[VNET] control command %u/%u: no answer\n", (unsigned)cls, (unsigned)cmd);
    has_ctrl = 0;                       /* the descriptor stays with the device */
    return -1;
}

/* ── /proc/netif ─────────────────────────────────────────────────────────── */
static int vnet_describe(netif_t *iface, char *buf, uint32_t cap) {
    (void)iface;
    if (!present || cap == 0)
        return 0;
    char irq[24];
    virtio_pci_describe_irq(&vp, irq, sizeof(irq));
    return snprintf(buf, cap,
                    "virtio%s %04x:%04x%s mac=%02x:%02x:%02x:%02x:%02x:%02x link=%s features=0x%08x rxq=%u txq=%u txfree=%u rxmode=%s irqs=%u kicks=%u/%u rxerr=%u txfull=%u",
                    vp.legacy ? "-legacy" : "",
                    (unsigned)vp.pci->vendor_id, (unsigned)vp.pci->device_id, irq,
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                    link_status() ? "up" : "down",
                    (unsigned)vp.features, (unsigned)rxq.size, (unsigned)txq.size,
                    (unsigned)tx_nfree,
                    allmulti_ok == 0 ? "allmulti" : "default",
                    (unsigned)vp.irqs, (unsigned)rxq.kicks, (unsigned)txq.kicks,
                    (unsigned)rx_errors, (unsigned)tx_full);
}

/* ── Bring-up ────────────────────────────────────────────────────────────── */
static void random_mac(const pci_device_t *d) {
    /* Locally administered, from the PCI location (stable across boots). */
    mac[0] = 0x02;
    mac[1] = 0x1A;
    mac[2] = 0xF4;
    mac[3] = d->bus;
    mac[4] = (uint8_t)((d->slot << 3) | d->func);
    mac[5] = 0x01;
}

void virtio_net_init(void) {
    const pci_device_t *d = virtio_pci_find(VIRTIO_ID_NET, 0);
    if (!d) {
        printk("[VNET] not present\n");
        return;
    }
    if (virtio_pci_probe(&vp, d, "VNET") < 0)
        return;

    uint64_t want = VIRTIO_F_VERSION_1 | VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS |
                    VIRTIO_NET_F_CTRL_VQ | VIRTIO_NET_F_CTRL_RX;
    uint64_t offered = vp.device_features;
    if (!(offered & VIRTIO_NET_F_CTRL_VQ))
        want &= ~VIRTIO_NET_F_CTRL_RX;      /* CTRL_RX requires CTRL_VQ */
    if (virtio_pci_set_features(&vp, want) < 0)
        return;
    has_ctrl = (vp.features & VIRTIO_NET_F_CTRL_VQ) != 0;
    /* num_buffers is in the header with VERSION_1 or MRG_RXBUF (5.1.6). */
    hdr_len = (vp.features & VIRTIO_F_VERSION_1) ? 12 : 10;

    virtio_pci_irq_setup(&vp, VIRTIO_IRQ_MSIX, vnet_irq, 0);

    if (virtio_pci_queue_setup(&vp, &rxq, Q_RX, RX_BUFS) < 0 ||
        virtio_pci_queue_setup(&vp, &txq, Q_TX, TX_BUFS) < 0) {
        printk("[VNET] queue setup failed\n");
        virtio_pci_fail(&vp);
        return;
    }
    /* The control queue follows the data queues (index 2 without MQ). */
    if (has_ctrl && virtio_pci_queue_setup(&vp, &ctrlq, Q_CTRL, 16) < 0) {
        printk("[VNET] control queue setup failed; staying in the default RX mode\n");
        has_ctrl = 0;
    }

    /* A legacy device's rings may be larger than the buffers we give them. */
    nrx = rxq.size < RX_BUFS ? rxq.size : RX_BUFS;
    ntx = txq.size < TX_BUFS ? txq.size : TX_BUFS;
    rx_mem = virtio_dma_alloc(nrx * BUF_SIZE);
    tx_mem = virtio_dma_alloc(ntx * BUF_SIZE);
    ctrl_mem = has_ctrl ? virtio_dma_alloc(64) : 0;
    if (!rx_mem || !tx_mem || (has_ctrl && !ctrl_mem)) {
        printk("[VNET] out of memory for buffers\n");
        virtio_pci_fail(&vp);
        return;
    }
    for (uint32_t i = 0; i < nrx; i++)
        rx_post(i);
    tx_nfree = 0;
    for (uint32_t i = ntx; i-- > 0;)
        tx_free[tx_nfree++] = (uint16_t)i;
    virtq_disable_cb(&txq);             /* reaped on send and poll */

    if (vp.features & VIRTIO_NET_F_MAC)
        virtio_pci_config_read(&vp, CFG_MAC, mac, 6);
    else
        random_mac(d);

    virtio_pci_driver_ok(&vp);
    virtio_pci_publish(&vp, &rxq);

    if (has_ctrl && (vp.features & VIRTIO_NET_F_CTRL_RX)) {
        promisc_off_ok = ctrl_cmd(VIRTIO_NET_CTRL_RX, VIRTIO_NET_CTRL_RX_PROMISC, 0);
        allmulti_ok = ctrl_cmd(VIRTIO_NET_CTRL_RX, VIRTIO_NET_CTRL_RX_ALLMULTI, 1);
    }
    link_up = link_status();

    present = 1;
    char name[8] = "eth0";
    for (int i = 0; i < 4 && net_find_interface(name); i++)
        name[3] = (char)('1' + i);
    vnet_netif = net_register(name, mac, NET_MTU_ETHERNET, &vp, vnet_send, vnet_poll);
    if (vnet_netif) {
        vnet_netif->driver = "virtio-net";
        vnet_netif->describe = vnet_describe;
    }

    char irq[24];
    virtio_pci_describe_irq(&vp, irq, sizeof(irq));
    printk("[VNET] %04x:%04x virtio %s%s mac=%02x:%02x:%02x:%02x:%02x:%02x%s features=0x%08x%08x rxq=%u txq=%u ctrl=%s link=%s\n",
           (unsigned)d->vendor_id, (unsigned)d->device_id,
           vp.legacy ? "legacy" : "1.x", irq,
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           (vp.features & VIRTIO_NET_F_MAC) ? "" : " (generated)",
           (unsigned)(vp.features >> 32), (unsigned)vp.features,
           (unsigned)rxq.size, (unsigned)txq.size,
           allmulti_ok == 0 && promisc_off_ok == 0 ? "allmulti" :
           has_ctrl ? "rx-cmd-failed" : "none",
           link_up ? "up" : "down");
}
