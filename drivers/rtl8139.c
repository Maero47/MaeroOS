#include "rtl8139.h"
#include "pci.h"
#include "../arch/i686/cpu/irq.h"
#include "../arch/i686/cpu/pic.h"
#include "../arch/i686/include/io.h"
#include "../include/kernel/config.h"
#include "../kernel/printk.h"
#include "../lib/string.h"
#include "../net/net.h"
#include "../proc/scheduler.h"
#include <registers.h>
#include <stdint.h>

#define RTL_VENDOR_ID 0x10EC
#define RTL_DEVICE_ID 0x8139

#define RTL_REG_MAC0      0x00
#define RTL_REG_TCR       0x40
#define RTL_REG_RCR       0x44
#define RTL_REG_RBSTART   0x30
#define RTL_REG_CAPR      0x38
#define RTL_REG_CMD       0x37
#define RTL_REG_IMR       0x3C
#define RTL_REG_ISR       0x3E
#define RTL_REG_CONFIG1   0x52

#define RTL_CMD_RESET     0x10
#define RTL_CMD_RE        0x08
#define RTL_CMD_TE        0x04
#define RTL_CMD_BUFE      0x01

#define RTL_TX_STAT_BASE  0x10
#define RTL_TX_ADDR_BASE  0x20
#define RTL_TX_DESC_COUNT 4
#define RTL_TX_OK         0x00008000U
#define RTL_TX_ABORT      0x00004000U

#define RTL_RCR_AAP       0x00000001U
#define RTL_RCR_APM       0x00000002U
#define RTL_RCR_AM        0x00000004U
#define RTL_RCR_AB        0x00000008U
#define RTL_RCR_WRAP      0x00000080U
#define RTL_RCR_MXDMA_UNL 0x00000700U
#define RTL_RCR_RBLEN_8K  0x00000000U
#define RTL_RCR_RBLEN_32K 0x00001000U
#define RTL_RCR_RBLEN_64K 0x00001800U

static rtl8139_info_t info;
#define RX_RING 32768U
static uint8_t rx_buffer[RX_RING + 16 + 1500] __attribute__((aligned(16)));
static uint8_t tx_buffer[RTL_TX_DESC_COUNT][1792] __attribute__((aligned(16)));
static netif_t *rtl_netif;

static uint8_t rtl_inb(uint8_t reg) {
    return inb((uint16_t)(info.io_base + reg));
}

static uint16_t rtl_inw(uint8_t reg) {
    return inw((uint16_t)(info.io_base + reg));
}

static uint32_t rtl_inl(uint8_t reg) {
    return inl((uint16_t)(info.io_base + reg));
}

static void rtl_outb(uint8_t reg, uint8_t val) {
    outb((uint16_t)(info.io_base + reg), val);
}

static void rtl_outw(uint8_t reg, uint16_t val) {
    outw((uint16_t)(info.io_base + reg), val);
}

static void rtl_outl(uint8_t reg, uint32_t val) {
    outl((uint16_t)(info.io_base + reg), val);
}

static uint32_t rtl_virt_to_phys(const void *ptr) {
    return (uint32_t)((uintptr_t)ptr - KERNEL_VMA);
}

static int rtl8139_net_send(netif_t *iface, const void *data, uint32_t len) {
    (void)iface;
    return rtl8139_send(data, len);
}

static int rtl8139_net_poll(netif_t *iface) {
    (void)iface;
    return rtl8139_poll();
}

int rtl8139_send(const void *data, uint32_t len) {
    if (!info.present)
        return -19;
    if (!data || len == 0 || len > 1792)
        return -22;

    uint32_t desc = info.tx_index % RTL_TX_DESC_COUNT;
    uint8_t tsad = (uint8_t)(RTL_TX_ADDR_BASE + desc * 4);
    uint8_t tsd = (uint8_t)(RTL_TX_STAT_BASE + desc * 4);

    memcpy(tx_buffer[desc], data, len);

    rtl_outl(tsad, rtl_virt_to_phys(tx_buffer[desc]));
    rtl_outl(tsd, len);

    for (int i = 0; i < 100000; i++) {
        uint32_t st = rtl_inl(tsd);
        if (st & RTL_TX_OK) {
            info.tx_index = (desc + 1) % RTL_TX_DESC_COUNT;
            return (int)len;
        }
        if (st & RTL_TX_ABORT)
            return -5;
    }

    return -11;
}

int rtl8139_poll(void) {
    if (!info.present)
        return 0;

    int packets = 0;
    while (!(rtl_inb(RTL_REG_CMD) & RTL_CMD_BUFE)) {
        uint32_t off = info.rx_offset % RX_RING;
        uint16_t status = rx_buffer[off] | ((uint16_t)rx_buffer[off + 1] << 8);
        uint16_t len = rx_buffer[off + 2] | ((uint16_t)rx_buffer[off + 3] << 8);

        if (len < 4 || len > 8192) {
            if (rtl_netif)
                rtl_netif->rx_dropped++;
            break;
        }

        uint32_t data_off = (off + 4U) % RX_RING;
        uint8_t packet[1600];
        uint32_t payload_len = (uint32_t)len - 4U; /* RTL includes CRC */
        if (payload_len > sizeof(packet)) {
            if (rtl_netif)
                rtl_netif->rx_dropped++;
        } else {
            for (uint32_t i = 0; i < payload_len; i++)
                packet[i] = rx_buffer[(data_off + i) % RX_RING];
            net_receive_ethernet(rtl_netif, packet, payload_len);
            packets++;
        }

        uint32_t advance = (uint32_t)len + 4U;
        advance = (advance + 3U) & ~3U;
        info.rx_offset = (info.rx_offset + advance) % RX_RING;
        /* CAPR tells the chip how far we've read.  It MUST span the whole
         * ring — masking to 0x1FFF (8 KB) left the chip thinking 8 KB..32 KB
         * was perpetually unread, so after ~8 KB received it saw the ring as
         * full and stopped RX forever (downloads stalled).  Use modulo over
         * the real 32 KB ring; the -16 is the RTL8139's fixed read offset. */
        rtl_outw(RTL_REG_CAPR,
                 (uint16_t)((info.rx_offset + RX_RING - 16U) % RX_RING));

        if (!(status & 0x0001U) && rtl_netif)
            rtl_netif->rx_dropped++;
    }

    return packets;
}

/*
 * RX interrupt: acknowledge and wake blocked I/O sleepers.  Packet
 * processing (lwIP) is NOT reentrant, so it stays in process context —
 * woken socket/poll callers drain the ring via net_poll_all().
 */
static void rtl8139_irq(registers_t *regs) {
    uint16_t isr;

    (void)regs;
    if (!info.present) return;
    /* Must be a 16-bit read: QEMU's model ignores byte accesses to ISR
     * (returning 0 and leaving the line asserted → interrupt storm). */
    isr = rtl_inw(RTL_REG_ISR);
    if (!isr) return;
    rtl_outw(RTL_REG_ISR, isr);   /* write-1-to-clear (real hardware) */
    io_wake();
}

void rtl8139_init(void) {
    memset(&info, 0, sizeof(info));

    const pci_device_t *dev = pci_find_device(RTL_VENDOR_ID, RTL_DEVICE_ID);
    if (!dev) {
        printk("[RTL8139] not present\n");
        return;
    }

    uint32_t bar0 = dev->bar[0];
    if (!(bar0 & 1U)) {
        printk("[RTL8139] memory BAR unsupported for now: 0x%08x\n",
               (unsigned)bar0);
        return;
    }

    info.io_base = (uint16_t)(bar0 & ~0x3U);
    info.irq = dev->irq_line;

    uint32_t cmd = pci_read_config32(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= 0x00000005U; /* I/O space + bus mastering */
    pci_write_config32(dev->bus, dev->slot, dev->func, 0x04, cmd);

    rtl_outb(RTL_REG_CONFIG1, 0x00); /* power on */
    rtl_outb(RTL_REG_CMD, RTL_CMD_RESET);
    for (int i = 0; i < 100000; i++) {
        if (!(rtl_inb(RTL_REG_CMD) & RTL_CMD_RESET))
            break;
    }

    for (int i = 0; i < 6; i++)
        info.mac[i] = rtl_inb((uint8_t)(RTL_REG_MAC0 + i));

    uint32_t rx_phys = rtl_virt_to_phys(rx_buffer);
    rtl_outl(RTL_REG_RBSTART, rx_phys);
    /* RX OK + RX error interrupts; the handler just acks and wakes I/O
     * sleepers (polling remains the data path). */
    rtl_outw(RTL_REG_IMR, 0x0005);
    rtl_outw(RTL_REG_ISR, 0xFFFF);

    uint32_t rcr = RTL_RCR_MXDMA_UNL | RTL_RCR_RBLEN_32K |
                   RTL_RCR_AB | RTL_RCR_AM | RTL_RCR_APM | RTL_RCR_WRAP;
    rtl_outl(RTL_REG_RCR, rcr);
    rtl_outb(RTL_REG_CMD, RTL_CMD_RE | RTL_CMD_TE);

    info.rx_config = rtl_inl(RTL_REG_RCR);
    info.tx_config = rtl_inl(RTL_REG_TCR);
    info.rx_offset = 0;
    info.tx_index = 0;
    info.present = 1;

    rtl_netif = net_register("eth0", info.mac, NET_MTU_ETHERNET, &info,
                             rtl8139_net_send, rtl8139_net_poll);

    if (info.irq >= 1 && info.irq <= 15) {
        irq_install_handler((int)info.irq, rtl8139_irq);
        pic_unmask((int)info.irq);
    }

    printk("[RTL8139] io=0x%04x irq=%u mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
           info.io_base, info.irq,
           info.mac[0], info.mac[1], info.mac[2],
           info.mac[3], info.mac[4], info.mac[5]);
}

const rtl8139_info_t *rtl8139_get_info(void) {
    return &info;
}
