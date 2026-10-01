/*
 * Intel 8254x ("e1000") gigabit Ethernet driver.
 *
 * Written from Intel's "PCI/PCI-X Family of Gigabit Ethernet Controllers
 * Software Developer's Manual" (8254x SDM, doc 317453) and the OSDev wiki's
 * Intel 8254x page; no code was copied.  Covers the 8254x parts QEMU and
 * VirtualBox emulate (82540EM 8086:100E is QEMU's `-nic model=e1000`,
 * 82545EM 100F, 82543GC 1004 VirtualBox's "Intel PRO/1000 T Server") and the
 * related 8254x/82574 IDs that keep the same legacy descriptor format.
 *
 * Model: legacy RX/TX descriptor rings in .bss (physical = virtual -
 * KERNEL_VMA, as for the RTL8139's buffers), the register window mapped
 * uncached at a fixed kernel window.  As with the RTL8139 the interrupt only
 * acknowledges and wakes I/O sleepers; frames are drained in process context
 * by net_poll_all(), because lwIP is not reentrant.
 */
#include "e1000.h"
#include "e1000_txq.h"
#include "pci.h"
#include "../arch/i686/cpu/irq.h"
#include "../arch/i686/cpu/pit.h"
#include "../arch/i686/cpu/pic.h"
#include "../arch/i686/include/io.h"
#include "../arch/i686/mm/paging.h"
#include "../include/kernel/config.h"
#include "../kernel/printk.h"
#include "../lib/printf.h"
#include "../lib/string.h"
#include "../net/net.h"
#include "../proc/scheduler.h"
#include <registers.h>
#include <stdint.h>

#define E1000_VENDOR_ID 0x8086

/* Register offsets (8254x SDM section 13). */
#define REG_CTRL     0x0000
#define REG_STATUS   0x0008
#define REG_EECD     0x0010
#define REG_EERD     0x0014
#define REG_ICR      0x00C0
#define REG_ITR      0x00C4
#define REG_IMS      0x00D0
#define REG_IMC      0x00D8
#define REG_RCTL     0x0100
#define REG_TCTL     0x0400
#define REG_TIPG     0x0410
#define REG_RDBAL    0x2800
#define REG_RDBAH    0x2804
#define REG_RDLEN    0x2808
#define REG_RDH      0x2810
#define REG_RDT      0x2818
#define REG_RDTR     0x2820
#define REG_TDBAL    0x3800
#define REG_TDBAH    0x3804
#define REG_TDLEN    0x3808
#define REG_TDH      0x3810
#define REG_TDT      0x3818
#define REG_MTA      0x5200   /* 128 dwords */
#define REG_RAL0     0x5400
#define REG_RAH0     0x5404

#define CTRL_LRST    (1U << 3)
#define CTRL_ASDE    (1U << 5)
#define CTRL_SLU     (1U << 6)
#define CTRL_ILOS    (1U << 7)
#define CTRL_RST     (1U << 26)
#define CTRL_VME     (1U << 30)
#define CTRL_PHY_RST (1U << 31)

#define STATUS_FD    (1U << 0)
#define STATUS_LU    (1U << 1)

#define RCTL_EN      (1U << 1)
#define RCTL_SBP     (1U << 2)
#define RCTL_UPE     (1U << 3)
#define RCTL_MPE     (1U << 4)
#define RCTL_BAM     (1U << 15)
#define RCTL_BSIZE_2048 0U       /* BSIZE=00, BSEX=0 */
#define RCTL_SECRC   (1U << 26)

#define TCTL_EN      (1U << 1)
#define TCTL_PSP     (1U << 3)
#define TCTL_CT(x)   ((uint32_t)(x) << 4)
#define TCTL_COLD(x) ((uint32_t)(x) << 12)
#define TCTL_RTLC    (1U << 24)

/* Interrupt cause bits (ICR/IMS/IMC). */
#define INT_TXDW     (1U << 0)
#define INT_LSC      (1U << 2)
#define INT_RXSEQ    (1U << 3)
#define INT_RXDMT0   (1U << 4)
#define INT_RXO      (1U << 6)
#define INT_RXT0     (1U << 7)

#define RXD_STAT_DD  0x01
#define RXD_STAT_EOP 0x02

#define TXD_CMD_EOP  0x01
#define TXD_CMD_IFCS 0x02
#define TXD_CMD_RS   0x08
#define TXD_STAT_DD  0x01

/* Legacy receive descriptor (SDM 3.2.3). */
struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t csum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed));

/* Legacy transmit descriptor (SDM 3.3.3). */
struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed));

#define RX_DESCS   64        /* RDLEN must be a multiple of 128 bytes */
#define TX_DESCS   32
#define BUF_SIZE   2048

/* The register file is 128 KiB (BAR0).  It is mapped uncached at
 * E1000_MMIO_VIRT, inside the PDEs 768-1022 every page directory snapshots
 * at creation (see the kernel virtual map in include/kernel/config.h). */

static const uint16_t e1000_ids[] = {
    0x1000, 0x1001, 0x1004, 0x1008, 0x1009, 0x100C, 0x100D, 0x100E, 0x100F,
    0x1010, 0x1011, 0x1012, 0x1013, 0x1015, 0x1016, 0x1017, 0x1018, 0x1019,
    0x101A, 0x101D, 0x101E, 0x1026, 0x1027, 0x1028, 0x1075, 0x1076, 0x1077,
    0x1078, 0x1079, 0x107A, 0x107B, 0x107C, 0x108A, 0x1099, 0x10B5, 0x10D3,
};

static e1000_info_t info;
static struct e1000_txq txq;
static volatile uint8_t *mmio;
static netif_t *e1000_netif;

static struct e1000_rx_desc rx_ring[RX_DESCS] __attribute__((aligned(128)));
static struct e1000_tx_desc tx_ring[TX_DESCS] __attribute__((aligned(128)));
static uint8_t rx_bufs[RX_DESCS][BUF_SIZE] __attribute__((aligned(16)));
static uint8_t tx_bufs[TX_DESCS][BUF_SIZE] __attribute__((aligned(16)));

static uint32_t rd32(uint32_t reg) {
    return *(volatile uint32_t *)(mmio + reg);
}

static void wr32(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(mmio + reg) = val;
}

static uint32_t virt_to_phys(const void *p) {
    return (uint32_t)((uintptr_t)p - KERNEL_VMA);
}

/* ~1 us per port-0x80 write; the PIT is not running yet at init time. */
static void delay_us(uint32_t us) {
    while (us--)
        io_wait();
}

/*
 * One 16-bit EEPROM word through EERD.  The 82540/82545/82543 family has the
 * address at bit 8 and DONE at bit 4; the 82541/82547/82571+/82574 family
 * moved them to bit 2 and bit 1.  Which one this part uses is found on the
 * first read (`layout` 0 = unknown, 1 = old, 2 = new).  Returns -1 if neither
 * completes, e.g. no EEPROM at all.
 */
static int eeprom_layout;

static int eerd_try(uint8_t addr, int shift, uint32_t done, uint16_t *out) {
    wr32(REG_EERD, ((uint32_t)addr << shift) | 1U);
    for (int i = 0; i < 10000; i++) {
        uint32_t v = rd32(REG_EERD);
        if (v & done) {
            *out = (uint16_t)(v >> 16);
            return 0;
        }
        delay_us(1);
    }
    return -1;
}

static int eeprom_read(uint8_t addr, uint16_t *out) {
    if (eeprom_layout != 2 && eerd_try(addr, 8, 1U << 4, out) == 0) {
        eeprom_layout = 1;
        return 0;
    }
    if (eeprom_layout != 1 && eerd_try(addr, 2, 1U << 1, out) == 0) {
        eeprom_layout = 2;
        return 0;
    }
    return -1;
}

static int mac_usable(const uint8_t m[6]) {
    int zero = 1, ones = 1;
    for (int i = 0; i < 6; i++) {
        if (m[i] != 0x00) zero = 0;
        if (m[i] != 0xFF) ones = 0;
    }
    return !zero && !ones && !(m[0] & 1);   /* not a group address */
}

static void read_mac(void) {
    uint16_t w[3];
    if (eeprom_read(0, &w[0]) == 0 && eeprom_read(1, &w[1]) == 0 &&
        eeprom_read(2, &w[2]) == 0) {
        for (int i = 0; i < 3; i++) {
            info.mac[i * 2] = (uint8_t)(w[i] & 0xFF);
            info.mac[i * 2 + 1] = (uint8_t)(w[i] >> 8);
        }
        if (mac_usable(info.mac)) {
            info.mac_from_eeprom = 1;
            return;
        }
    }
    /* No (or a blank) EEPROM: the address the firmware/BIOS left in the
     * first receive-address register. */
    uint32_t ral = rd32(REG_RAL0), rah = rd32(REG_RAH0);
    for (int i = 0; i < 4; i++)
        info.mac[i] = (uint8_t)(ral >> (i * 8));
    info.mac[4] = (uint8_t)rah;
    info.mac[5] = (uint8_t)(rah >> 8);
    info.mac_from_eeprom = 0;
}

static void rx_setup(void) {
    for (int i = 0; i < RX_DESCS; i++) {
        memset(&rx_ring[i], 0, sizeof(rx_ring[i]));
        rx_ring[i].addr = virt_to_phys(rx_bufs[i]);
    }
    wr32(REG_RDBAL, virt_to_phys(rx_ring));
    wr32(REG_RDBAH, 0);
    wr32(REG_RDLEN, sizeof(rx_ring));
    wr32(REG_RDH, 0);
    /* Head == tail means "no descriptors": hand all but one to the chip. */
    wr32(REG_RDT, RX_DESCS - 1);
    wr32(REG_RDTR, 0);              /* no receive-interrupt delay */
    info.rx_next = 0;
    wr32(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048);
}

static void tx_setup(void) {
    for (int i = 0; i < TX_DESCS; i++) {
        memset(&tx_ring[i], 0, sizeof(tx_ring[i]));
        tx_ring[i].addr = virt_to_phys(tx_bufs[i]);
    }
    wr32(REG_TDBAL, virt_to_phys(tx_ring));
    wr32(REG_TDBAH, 0);
    wr32(REG_TDLEN, sizeof(tx_ring));
    wr32(REG_TDH, 0);
    wr32(REG_TDT, 0);
    e1000_txq_init(&txq, TX_DESCS);
    /* Recommended values for full duplex copper (SDM 13.4.33/13.4.34). */
    wr32(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT(0x0F) | TCTL_COLD(0x40) |
                   TCTL_RTLC);
    wr32(REG_TIPG, 10U | (8U << 10) | (6U << 20));
}

static int tx_done(uint32_t idx, void *ctx) {
    (void)ctx;
    return (((volatile struct e1000_tx_desc *)&tx_ring[idx])->status &
            TXD_STAT_DD) != 0;
}

static uint32_t tx_reclaim(void) {
    return e1000_txq_reclaim(&txq, tx_done, 0);
}

/*
 * TX watchdog: posted frames that make no progress for 2 s mean the chip
 * stopped transmitting (link down on real hardware, a hung DMA engine).
 * Reset the transmit unit with an empty ring, as Linux's e1000 watchdog does
 * (dropping what was queued; TCP retransmits).  Called from send/poll, both
 * in process context under preempt_disable().
 */
static void tx_watchdog(void) {
    if (!e1000_txq_stalled(&txq, pit_ticks(), tx_done, 0))
        return;
    info.tx_resets++;
    printk_klog("[E1000] transmit stalled (%u frames queued, link %s); resetting TX\n",
                (unsigned)e1000_txq_pending(&txq),
                (rd32(REG_STATUS) & STATUS_LU) ? "up" : "down");
    wr32(REG_TCTL, 0);
    tx_setup();
}

int e1000_send(const void *data, uint32_t len) {
    if (!info.present)
        return -19;
    if (!data || len == 0 || len > BUF_SIZE)
        return -22;

    /* Ring full: give the chip a moment to finish a frame before giving up
     * (the caller drops this one; the watchdog deals with a dead chip). */
    tx_reclaim();
    for (int i = 0; i < 100000 && e1000_txq_full(&txq); i++) {
        __asm__ volatile("pause");
        tx_reclaim();
    }
    if (e1000_txq_full(&txq)) {
        tx_watchdog();
        if (e1000_txq_full(&txq))
            return -11;
    }

    uint32_t idx = e1000_txq_post(&txq);
    volatile struct e1000_tx_desc *d = &tx_ring[idx];
    memcpy(tx_bufs[idx], data, len);
    d->length = (uint16_t)len;
    d->cso = 0;
    d->css = 0;
    d->special = 0;
    d->cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    d->status = 0;

    __asm__ volatile("" ::: "memory");
    wr32(REG_TDT, txq.next);
    return (int)len;
}

int e1000_poll(void) {
    if (!info.present)
        return 0;

    tx_watchdog();

    int packets = 0;
    for (;;) {
        uint32_t idx = info.rx_next;
        volatile struct e1000_rx_desc *d = &rx_ring[idx];
        uint8_t status = d->status;
        if (!(status & RXD_STAT_DD))
            break;
        __asm__ volatile("" ::: "memory");   /* status before the data */

        uint16_t len = d->length;
        /* 2 KiB buffers and no long-packet mode: every frame fits in one
         * descriptor, so one without EOP is a jumbo the chip should have
         * dropped.  Any error bit (CRC, symbol, sequence, RX data) drops it
         * too. */
        if ((status & RXD_STAT_EOP) && !d->errors && len >= 14 &&
            len <= BUF_SIZE) {
            net_receive_ethernet(e1000_netif, rx_bufs[idx], len);
            packets++;
        } else if (e1000_netif) {
            e1000_netif->rx_dropped++;
        }

        d->status = 0;
        d->errors = 0;
        info.rx_next = (idx + 1) % RX_DESCS;
        /* Give the slot back: the tail trails our read position by one. */
        wr32(REG_RDT, idx);
    }
    return packets;
}

static int e1000_net_send(netif_t *iface, const void *data, uint32_t len) {
    (void)iface;
    return e1000_send(data, len);
}

static int e1000_net_poll(netif_t *iface) {
    (void)iface;
    return e1000_poll();
}

static int e1000_net_describe(netif_t *iface, char *buf, uint32_t cap) {
    (void)iface;
    return e1000_describe(buf, cap);
}

/* Reading ICR acknowledges every cause.  0 means the interrupt was some other
 * device's on a shared line. */
static void e1000_irq(registers_t *regs) {
    (void)regs;
    if (!info.present)
        return;
    uint32_t icr = rd32(REG_ICR);
    if (!icr)
        return;
    if (icr & INT_LSC)
        wr32(REG_CTRL, rd32(REG_CTRL) | CTRL_SLU);
    io_wake();
}

static const pci_device_t *find_device(void) {
    for (uint32_t i = 0; i < sizeof(e1000_ids) / sizeof(e1000_ids[0]); i++) {
        const pci_device_t *dev = pci_find_device(E1000_VENDOR_ID, e1000_ids[i]);
        if (dev)
            return dev;
    }
    return 0;
}

void e1000_init(void) {
    memset(&info, 0, sizeof(info));

    const pci_device_t *dev = find_device();
    if (!dev) {
        printk("[E1000] not present\n");
        return;
    }

    uint32_t bar0 = dev->bar[0];
    if (bar0 & 1U) {
        printk("[E1000] BAR0 is I/O space (0x%08x), unsupported\n",
               (unsigned)bar0);
        return;
    }
    if (((bar0 >> 1) & 3U) == 2U && dev->bar[1] != 0) {
        printk("[E1000] BAR0 above 4 GiB, unsupported\n");
        return;
    }
    uint32_t phys = bar0 & ~0xFU;
    if (!phys) {
        printk("[E1000] BAR0 not assigned\n");
        return;
    }

    /* Memory space + bus mastering on, INTx not disabled. */
    uint32_t cmd = pci_read_config32(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= 0x00000006U;
    cmd &= ~(1U << 10);
    pci_write_config32(dev->bus, dev->slot, dev->func, 0x04, cmd);

    for (uint32_t off = 0; off < E1000_MMIO_SIZE; off += PAGE_SIZE) {
        if (paging_map(E1000_MMIO_VIRT + off, phys + off,
                       PAGE_PRESENT | PAGE_WRITABLE | PAGE_NOCACHE |
                       PAGE_WRITETHRU) != 0) {
            printk("[E1000] cannot map the register window\n");
            return;
        }
    }
    mmio = (volatile uint8_t *)E1000_MMIO_VIRT;

    info.device_id = dev->device_id;
    info.mmio_phys = phys;
    info.irq = dev->irq_line;

    /* Quiesce, reset, quiesce again (the reset re-enables nothing, but a
     * cause latched before it must not fire once the IRQ is unmasked). */
    wr32(REG_IMC, 0xFFFFFFFFU);
    wr32(REG_RCTL, 0);
    wr32(REG_TCTL, 0);
    wr32(REG_CTRL, rd32(REG_CTRL) | CTRL_RST);
    delay_us(1000);
    for (int i = 0; i < 100000 && (rd32(REG_CTRL) & CTRL_RST); i++)
        delay_us(1);
    wr32(REG_IMC, 0xFFFFFFFFU);
    (void)rd32(REG_ICR);

    /* Let the MAC follow the PHY's autonegotiated speed/duplex and bring the
     * link up. */
    uint32_t ctrl = rd32(REG_CTRL);
    ctrl |= CTRL_SLU | CTRL_ASDE;
    ctrl &= ~(CTRL_LRST | CTRL_PHY_RST | CTRL_ILOS | CTRL_VME);
    wr32(REG_CTRL, ctrl);

    read_mac();
    wr32(REG_RAL0, (uint32_t)info.mac[0] | ((uint32_t)info.mac[1] << 8) |
                   ((uint32_t)info.mac[2] << 16) | ((uint32_t)info.mac[3] << 24));
    wr32(REG_RAH0, (uint32_t)info.mac[4] | ((uint32_t)info.mac[5] << 8) |
                   (1U << 31));        /* Address Valid */
    for (int i = 0; i < 128; i++)
        wr32(REG_MTA + (uint32_t)i * 4, 0);

    rx_setup();
    tx_setup();

    info.present = 1;
    /* The first NIC found is eth0 (lwIP's interface); a second one is still
     * registered so it shows up in /proc/netif. */
    e1000_netif = net_register(net_find_interface("eth0") ? "eth1" : "eth0",
                               info.mac, NET_MTU_ETHERNET, &info,
                               e1000_net_send, e1000_net_poll);
    if (e1000_netif) {
        e1000_netif->driver = "e1000";
        e1000_netif->describe = e1000_net_describe;
    }

    if (info.irq >= 1 && info.irq <= 15) {
        irq_install_handler(info.irq, e1000_irq);
        pic_unmask(info.irq);
    }
    wr32(REG_ITR, 0);
    wr32(REG_IMS, INT_RXT0 | INT_RXO | INT_RXDMT0 | INT_RXSEQ | INT_LSC);

    uint32_t status = rd32(REG_STATUS);
    printk("[E1000] 8086:%04x mmio=0x%08x irq=%u mac=%02x:%02x:%02x:%02x:%02x:%02x (%s) link=%s\n",
           (unsigned)info.device_id, (unsigned)phys, (unsigned)info.irq,
           info.mac[0], info.mac[1], info.mac[2],
           info.mac[3], info.mac[4], info.mac[5],
           info.mac_from_eeprom ? "eeprom" : "ral0",
           (status & STATUS_LU) ? "up" : "down");
}

int e1000_describe(char *buf, uint32_t cap) {
    if (!info.present || cap == 0)
        return 0;
    uint32_t status = rd32(REG_STATUS);
    return snprintf(buf, cap,
                    "mmio=0x%08x irq=%u mac=%02x:%02x:%02x:%02x:%02x:%02x link=%s%s rdh=%u rdt=%u tdh=%u tdt=%u rctl=0x%x txresets=%u",
                    (unsigned)info.mmio_phys, (unsigned)info.irq,
                    info.mac[0], info.mac[1], info.mac[2],
                    info.mac[3], info.mac[4], info.mac[5],
                    (status & STATUS_LU) ? "up" : "down",
                    (status & STATUS_LU) ? ((status & STATUS_FD) ? "/fd" : "/hd") : "",
                    (unsigned)rd32(REG_RDH), (unsigned)rd32(REG_RDT),
                    (unsigned)rd32(REG_TDH), (unsigned)rd32(REG_TDT),
                    (unsigned)rd32(REG_RCTL), (unsigned)info.tx_resets);
}

const e1000_info_t *e1000_get_info(void) {
    return &info;
}
