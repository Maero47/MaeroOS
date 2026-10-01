/*
 * Realtek RTL8169/8168/8111/810x gigabit Ethernet driver ("r8169", after the
 * family's first part).  The register programming, chip table and rings are
 * in r8169_hw.h (host-tested by tools/test_r8169.py); this file is the
 * kernel side: PCI probe, register accessors, memory, IRQ, and the net/
 * glue, modelled on drivers/e1000.c.
 *
 * QEMU emulates none of these parts, so this was written from documentation
 * (see r8169_hw.h and docs/r8169.md) and has to be tried on a real PC.  It
 * probes nothing but the PCI IDs below, so on a machine without one it
 * prints "not present" and touches no hardware.  A TxConfig revision it
 * does not know is left as found: PCI COMMAND and power state restored,
 * nothing written to the chip.  Bus mastering stays off from probe until
 * the chip has been stopped, reset and given fresh rings.
 *
 * Model, as for the e1000: rings and buffers in .bss (physical = virtual -
 * KERNEL_VMA, inside the low 16 MiB, so below 4 GiB with or without PAE and
 * every addr_hi is 0); the interrupt only acknowledges and wakes I/O
 * sleepers; frames are copied in and out in process context by
 * net_poll_all() and the send path, because lwIP is not reentrant.
 *
 * Per-chip notes (flags in r8169_hw.h's table, from re(4)):
 *   8169/8169S/8110S/SB/SC   conventional PCI.  0x82 = 1 after every reset
 *                            (MACRESET); 8169S: PHY reg 0x0b = 0 after reset;
 *                            8110SC/SCe: PCI timing magic at 0x7C.  C+ mode
 *                            via CPlusCmd TXENB|RXENB.
 *   8168B/8111B (XID 30/38)  MACSTAT C+ mode; the first spin needs a reset
 *                            after an RX FIFO overflow (FOVW_RESET).
 *   8168C/CP, D, DP          stop with ChipCmd STOPREQ (DP: wait for TxPoll
 *                            to drain instead); 8168C spin 2 / macrev 2:
 *                            out of deep sleep via GPIO; D: PHY power PMCH.
 *   8168E/8111E (2C0)        PMCH PHY power on at attach; STOPREQ stop.
 *   8168E-VL/8111E-VL (2C8)  STOPREQ + wait for TxConfig "queue empty";
 *                            RxConfig early-off bits (0x3800).
 *   8168F/8111F (480)        as E-VL; its multicast hash filter is broken,
 *                            moot here since all multicast is accepted.
 *   8168G/GU/EP/H/FP, 8411B  (4C0/508/500/540/548/5C8) no STOPREQ: stop by
 *                            gating RXDV (MISC bit 19), waiting for the TX
 *                            queue and the MCU's FIFO-empty flags, then
 *                            clearing TE/RE.  At start the gate is opened
 *                            and TE/RE are set *after* TxConfig/RxConfig
 *                            (earlier parts: before).  RxConfig early-off v2.
 *                            8168GU/H silicon on a 10ec:8136 card is the
 *                            10/100 8106EUS/8107E: no 1000BASE-T advert.
 *   8101E..8106E, 8401E/8402 PCIe 10/100 (10ec:8136): no 1000BASE-T advert;
 *                            8105E/8106E/8401E/8402: PMCH PHY power on.
 * Not done (works without on the chips re(4) drives, which does the same
 * subset): Linux's per-revision PCIe-PHY ("ephy") tables, PHY firmware
 * patches, EEE, jumbo frames, checksum offload, MSI.
 */
#include "r8169.h"
#include "r8169_hw.h"
#include "pci.h"
#include "../arch/i686/cpu/irq.h"
#include "../arch/i686/cpu/pit.h"
#include "../arch/i686/cpu/pic.h"
#include "../arch/i686/include/io.h"
#include "../mm/mmio.h"
#include "../include/kernel/config.h"
#include "../kernel/printk.h"
#include "../lib/printf.h"
#include "../lib/string.h"
#include "../net/net.h"
#include "../proc/scheduler.h"
#include <registers.h>
#include <stdint.h>

#define RX_DESCS   64
#define TX_DESCS   64
#define RX_BUF     2048      /* 1518 + VLAN fits; also the RMS value */
#define TX_BUF     1536      /* largest frame we send is 1514 */

/* Every ID re(4) drives as an RTL8169-family part, except 10ec:8139 (the
 * 8139C+ shares it with the plain 8139, which drivers/rtl8139.c owns). */
static const struct { uint16_t vendor, device; } r8169_ids[] = {
    { 0x10EC, 0x8169 }, { 0x10EC, 0x8167 },   /* 8169/8110, 8169SC/8110SC */
    { 0x10EC, 0x8168 }, { 0x10EC, 0x8161 },   /* 8168/8111 family */
    { 0x10EC, 0x8136 },                       /* 810xE fast Ethernet */
    { 0x10EC, 0x2502 }, { 0x10EC, 0x2600 },   /* Killer E2500/E2600 (8168) */
    { 0x1186, 0x4300 }, { 0x1186, 0x4302 },   /* D-Link DGE-528T/530T rev C */
    { 0x1259, 0xC107 },                       /* Corega CG-LAPCIGT */
    { 0x1737, 0x1032 },                       /* Linksys EG1032 rev 3 */
    { 0x16EC, 0x0116 },                       /* USR 997902 */
    { 0x10FF, 0x8168 },                       /* TP-Link TG-3468 v2 */
};

static r8169_info_t info;
static struct r8169_io io;
static volatile uint8_t *mmio;
static netif_t *r8169_netif;
static struct r8169_txq txq;
static struct r8169_rxq rxq;
static volatile int need_reset;      /* set by the IRQ, handled by poll */
static volatile uint16_t last_isr;

static struct r8169_desc rx_ring[RX_DESCS] __attribute__((aligned(R8169_RING_ALIGN)));
static struct r8169_desc tx_ring[TX_DESCS] __attribute__((aligned(R8169_RING_ALIGN)));
static uint8_t rx_bufs[RX_DESCS][RX_BUF] __attribute__((aligned(8)));
static uint8_t tx_bufs[TX_DESCS][TX_BUF] __attribute__((aligned(8)));

_Static_assert(sizeof(struct r8169_desc) == 16, "descriptor is 16 bytes");
_Static_assert(RX_DESCS <= 1024 && TX_DESCS <= 1024, "ring at most 1024");
_Static_assert(RX_BUF <= R8169_RX_BUF_MASK && (RX_BUF % 8) == 0, "RX buffer");

/* ── Register accessors: MMIO when there is a usable memory BAR, else the
 * I/O BAR every part also has. ─────────────────────────────────────────── */
static uint8_t m_rd8(void *c, uint32_t r)  { (void)c; return *(volatile uint8_t *)(mmio + r); }
static uint16_t m_rd16(void *c, uint32_t r) { (void)c; return *(volatile uint16_t *)(mmio + r); }
static uint32_t m_rd32(void *c, uint32_t r) { (void)c; return *(volatile uint32_t *)(mmio + r); }
static void m_wr8(void *c, uint32_t r, uint8_t v)   { (void)c; *(volatile uint8_t *)(mmio + r) = v; }
static void m_wr16(void *c, uint32_t r, uint16_t v) { (void)c; *(volatile uint16_t *)(mmio + r) = v; }
static void m_wr32(void *c, uint32_t r, uint32_t v) { (void)c; *(volatile uint32_t *)(mmio + r) = v; }

static uint8_t p_rd8(void *c, uint32_t r)  { (void)c; return inb((uint16_t)(info.io_base + r)); }
static uint16_t p_rd16(void *c, uint32_t r) { (void)c; return inw((uint16_t)(info.io_base + r)); }
static uint32_t p_rd32(void *c, uint32_t r) { (void)c; return inl((uint16_t)(info.io_base + r)); }
static void p_wr8(void *c, uint32_t r, uint8_t v)   { (void)c; outb((uint16_t)(info.io_base + r), v); }
static void p_wr16(void *c, uint32_t r, uint16_t v) { (void)c; outw((uint16_t)(info.io_base + r), v); }
static void p_wr32(void *c, uint32_t r, uint32_t v) { (void)c; outl((uint16_t)(info.io_base + r), v); }

/* ~1 us per port-0x80 write; the PIT may not be running at init time. */
static void udelay_io(void *c, uint32_t us) {
    (void)c;
    while (us--)
        io_wait();
}

static uint32_t virt_to_phys(const void *p) {
    return (uint32_t)((uintptr_t)p - KERNEL_VMA);
}

/* ── PCI helpers ─────────────────────────────────────────────────────────── */
static const pci_device_t *find_device(void) {
    for (int i = 0; i < pci_device_count(); i++) {
        const pci_device_t *d = pci_get_device(i);
        for (uint32_t j = 0; j < sizeof(r8169_ids) / sizeof(r8169_ids[0]); j++) {
            if (d->vendor_id != r8169_ids[j].vendor ||
                d->device_id != r8169_ids[j].device)
                continue;
            /* Only revision 3 of the Linksys EG1032 (subsystem 0x0024) is
             * an RTL8169S; earlier ones carry another vendor's chip. */
            if (d->vendor_id == 0x1737 &&
                (pci_read_config32(d->bus, d->slot, d->func, 0x2C) >> 16) != 0x0024)
                continue;
            return d;
        }
    }
    return 0;
}

/* Config-space offset of capability `id`, 0 if absent. */
static uint8_t pci_cap(const pci_device_t *d, uint8_t id) {
    uint32_t sts = pci_read_config32(d->bus, d->slot, d->func, 0x04) >> 16;
    if (!(sts & 0x10))
        return 0;
    uint8_t p = (uint8_t)(pci_read_config32(d->bus, d->slot, d->func, 0x34) & 0xFC);
    for (int n = 0; p && n < 48; n++) {
        uint32_t v = pci_read_config32(d->bus, d->slot, d->func, p);
        if ((v & 0xFF) == id)
            return p;
        p = (uint8_t)((v >> 8) & 0xFC);
    }
    return 0;
}

/* PCI state at probe, put back if the device turns out not to be ours to
 * drive (unknown revision, no usable BAR): COMMAND as firmware left it and
 * the power state.  pm_off is 0 when there is no PM capability. */
static const pci_device_t *pdev;
static uint32_t orig_cmd, orig_pmcsr;
static uint8_t pm_off;

/* Firmware may leave the NIC in D3hot; registers read all-ones there. */
static void pci_to_d0(const pci_device_t *d) {
    pm_off = pci_cap(d, 0x01);
    if (!pm_off)
        return;
    orig_pmcsr = pci_read_config32(d->bus, d->slot, d->func, (uint8_t)(pm_off + 4));
    if (orig_pmcsr & 3U) {
        pci_write_config32(d->bus, d->slot, d->func, (uint8_t)(pm_off + 4),
                           orig_pmcsr & ~3U);
        udelay_io(0, 10000);          /* D3hot -> D0: 10 ms */
    }
}

/* Give up on the device: decode and bus mastering as they were, back to the
 * power state it was found in. */
static void pci_restore(const pci_device_t *d) {
    pci_write_config32(d->bus, d->slot, d->func, 0x04, orig_cmd & 0xFFFFU);
    if (pm_off && (orig_pmcsr & 3U))
        pci_write_config32(d->bus, d->slot, d->func, (uint8_t)(pm_off + 4),
                           orig_pmcsr & 0xFFFFU & ~(1U << 15));   /* PME_Status RW1C */
}

/* io.bus_master: r8169_hw_start() calls it once the chip has been stopped,
 * reset and given fresh rings. */
static void bus_master_on(void *ctx) {
    (void)ctx;
    uint32_t cmd = pci_read_config32(pdev->bus, pdev->slot, pdev->func, 0x04);
    if (!(cmd & 0x4U))
        pci_write_config32(pdev->bus, pdev->slot, pdev->func, 0x04,
                           (cmd & 0xFFFFU) | 0x4U);
}

/* ASPM L0s/L1 and clock PM off (as re(4) does by default): implicated in TX
 * stalls on many 8168 revisions.  Link Status above is RW1C; write 0s. */
static int pcie_aspm_off(const pci_device_t *d) {
    uint8_t pe = pci_cap(d, 0x10);
    if (!pe)
        return 0;
    uint8_t off = (uint8_t)(pe + 0x10);
    uint32_t v = pci_read_config32(d->bus, d->slot, d->func, off);
    uint32_t ctl = v & 0xFFFFU;
    if (!(ctl & 0x0103U))
        return 0;
    pci_write_config32(d->bus, d->slot, d->func, off, ctl & ~0x0103U);
    return 1;
}

/* ── MAC address ─────────────────────────────────────────────────────────── */
static int mac_usable(const uint8_t m[6]) {
    int zero = 1, ones = 1;
    for (int i = 0; i < 6; i++) {
        if (m[i] != 0x00) zero = 0;
        if (m[i] != 0xFF) ones = 0;
    }
    return !zero && !ones && !(m[0] & 1);
}

/* IDR0-5, loaded by the chip from its EEPROM/eFuse at power-on.  If blank,
 * a locally administered address from the PCI location and the XID. */
static void read_mac(const pci_device_t *d) {
    uint32_t lo = R8169_RD32(&io, R8169_IDR0), hi = R8169_RD32(&io, R8169_IDR4);
    for (int i = 0; i < 4; i++)
        info.mac[i] = (uint8_t)(lo >> (i * 8));
    info.mac[4] = (uint8_t)hi;
    info.mac[5] = (uint8_t)(hi >> 8);
    if (mac_usable(info.mac))
        return;
    info.mac_generated = 1;
    info.mac[0] = 0x02;
    info.mac[1] = 0x10;
    info.mac[2] = 0xEC;
    info.mac[3] = d->bus;
    info.mac[4] = (uint8_t)((d->slot << 3) | d->func);
    info.mac[5] = (uint8_t)(info.txcfg >> 24);
}

/* IDR is only writable with Cfg9346 in config-write mode. */
static void write_mac(void) {
    R8169_WR8(&io, R8169_EECMD, R8169_EECMD_WRCFG);
    R8169_WR32(&io, R8169_IDR4, (uint32_t)info.mac[4] | ((uint32_t)info.mac[5] << 8));
    (void)R8169_RD32(&io, R8169_IDR4);
    R8169_WR32(&io, R8169_IDR0, (uint32_t)info.mac[0] | ((uint32_t)info.mac[1] << 8) |
                                ((uint32_t)info.mac[2] << 16) | ((uint32_t)info.mac[3] << 24));
    (void)R8169_RD32(&io, R8169_IDR0);
    R8169_WR8(&io, R8169_EECMD, R8169_EECMD_OFF);
}

/* ── Bring-up ────────────────────────────────────────────────────────────── */
/* Stop, reset, fresh rings, start (which turns bus mastering on).  Used at
 * attach and to recover. */
static int hw_restart(void) {
    r8169_hw_stop(&io, info.flags);
    int r = r8169_hw_reset(&io, info.flags);

    static uint32_t rx_phys[RX_DESCS];
    for (int i = 0; i < RX_DESCS; i++)
        rx_phys[i] = virt_to_phys(rx_bufs[i]);
    r8169_rx_init(&rxq, rx_ring, RX_DESCS, rx_phys, RX_BUF);
    r8169_tx_init(&txq, tx_ring, TX_DESCS);

    write_mac();
    r8169_hw_start(&io, info.flags, virt_to_phys(rx_ring),
                   virt_to_phys(tx_ring), RX_BUF);
    need_reset = 0;
    return r;
}

static void recover(const char *why) {
    info.resets++;
    printk_klog("[R8169] %s; resetting the chip (%u frames queued, link %s)\n",
                why, (unsigned)txq.pending,
                (R8169_RD8(&io, R8169_PHYSTATUS) & R8169_PHY_LINK) ? "up" : "down");
    hw_restart();
}

/* ── Data path ───────────────────────────────────────────────────────────── */
int r8169_send(const void *data, uint32_t len) {
    if (!info.present)
        return -19;
    if (!data || len == 0 || len > TX_BUF)
        return -22;

    r8169_tx_reclaim(&txq, tx_ring);
    for (int i = 0; i < 100000 && r8169_tx_full(&txq); i++) {
        __asm__ volatile("pause");
        r8169_tx_reclaim(&txq, tx_ring);
    }
    if (r8169_tx_full(&txq)) {
        if (r8169_tx_stalled(&txq, tx_ring, pit_ticks()))
            recover("transmit stalled");
        if (r8169_tx_full(&txq))
            return -11;
    }

    uint32_t idx = txq.next;
    uint32_t wire = r8169_tx_wire_len(len);
    memcpy(tx_bufs[idx], data, len);
    if (wire > len)
        memset(tx_bufs[idx] + len, 0, wire - len);
    r8169_tx_post(&txq, tx_ring, virt_to_phys(tx_bufs[idx]), wire);
    R8169_BARRIER();
    R8169_WR8(&io, R8169_TPPOLL, R8169_TPPOLL_NPQ);
    return (int)len;
}

int r8169_poll(void) {
    if (!info.present)
        return 0;

    if (need_reset)
        recover((last_isr & R8169_INT_SERR) ? "PCI system error"
                                            : "RX FIFO overflow");
    else if (r8169_tx_stalled(&txq, tx_ring, pit_ticks()))
        recover("transmit stalled");

    int packets = 0;
    for (int n = 0; n < RX_DESCS; n++) {
        uint32_t idx, len;
        int r = r8169_rx_next(&rxq, rx_ring, &idx, &len);
        if (r == R8169_RX_NONE)
            break;
        if (r == R8169_RX_FRAME) {
            net_receive_ethernet(r8169_netif, rx_bufs[idx], len);
            packets++;
        } else if (r8169_netif) {
            r8169_netif->rx_dropped++;
        }
        r8169_rx_recycle(&rxq, rx_ring, idx);
    }
    return packets;
}

static int r8169_net_send(netif_t *iface, const void *data, uint32_t len) {
    (void)iface;
    return r8169_send(data, len);
}

static int r8169_net_poll(netif_t *iface) {
    (void)iface;
    return r8169_poll();
}

static int r8169_net_describe(netif_t *iface, char *buf, uint32_t cap) {
    (void)iface;
    return r8169_describe(buf, cap);
}

/* ISR is write-1-to-clear.  0: another device's interrupt on a shared line;
 * 0xFFFF: the device is gone (surprise removal, or D3). */
static void r8169_irq(registers_t *regs) {
    (void)regs;
    if (!info.present)
        return;
    uint16_t isr = R8169_RD16(&io, R8169_ISR);
    if (!isr || isr == 0xFFFF)
        return;
    R8169_WR16(&io, R8169_ISR, isr);
    last_isr = isr;
    if (isr & R8169_INT_RDU)
        info.rdu++;
    if (isr & R8169_INT_FOVW) {
        info.fovw++;
        if (info.flags & R8169_F_FOVW_RESET)
            need_reset = 1;
    }
    if (isr & R8169_INT_SERR)
        need_reset = 1;
    io_wake();
}

static void link_str(char *buf, uint32_t cap) {
    uint8_t ps = R8169_RD8(&io, R8169_PHYSTATUS);
    if (!(ps & R8169_PHY_LINK)) {
        snprintf(buf, cap, "down");
        return;
    }
    snprintf(buf, cap, "up/%s/%s",
             (ps & R8169_PHY_1000M) ? "1000" : (ps & R8169_PHY_100M) ? "100" :
             (ps & R8169_PHY_10M) ? "10" : "?",
             (ps & R8169_PHY_FDX) ? "fd" : "hd");
}

void r8169_init(void) {
    memset(&info, 0, sizeof(info));

    const pci_device_t *dev = find_device();
    if (!dev) {
        printk("[R8169] not present\n");
        return;
    }
    info.vendor_id = dev->vendor_id;
    info.device_id = dev->device_id;
    info.irq = dev->irq_line;

    /* Prefer a memory BAR (BAR1 on the 8169, BAR2/3 64-bit on PCIe parts);
     * one above 4 GiB is unusable here, so fall back to the I/O BAR. */
    uint32_t mem = 0;
    uint16_t port = 0;
    for (int i = 0; i < 6; i++) {
        uint32_t b = dev->bar[i];
        if (b & 1U) {
            if (!port)
                port = (uint16_t)(b & ~3U);
            continue;
        }
        int is64 = ((b >> 1) & 3U) == 2U;
        uint32_t hi = (is64 && i < 5) ? dev->bar[i + 1] : 0;
        if (!mem && (b & ~0xFU) && hi == 0)
            mem = b & ~0xFU;
        if (is64)
            i++;
    }

    /* Nothing is written to the device before the BAR is known usable. */
    if (mem)
        mmio = mmio_map(mem, 0x100);
    if (mmio) {
        info.bar_phys = mem;
        io = (struct r8169_io){ 0, m_rd8, m_rd16, m_rd32, m_wr8, m_wr16, m_wr32,
                                udelay_io, bus_master_on };
    } else if (port) {
        info.io_base = port;
        io = (struct r8169_io){ 0, p_rd8, p_rd16, p_rd32, p_wr8, p_wr16, p_wr32,
                                udelay_io, bus_master_on };
    } else {
        printk("[R8169] %04x:%04x has no usable BAR\n",
               (unsigned)dev->vendor_id, (unsigned)dev->device_id);
        return;
    }

    /* Register decode on, bus mastering OFF: firmware (a PXE ROM, UEFI's
     * network stack) can leave RX DMA armed on rings in memory the kernel
     * now owns.  With BME clear the chip cannot reach memory; it comes back
     * on in r8169_hw_start(), after stop, reset and fresh rings.  INTx stays
     * as firmware left it until the device is ours. */
    pdev = dev;
    orig_cmd = pci_read_config32(dev->bus, dev->slot, dev->func, 0x04);
    pci_write_config32(dev->bus, dev->slot, dev->func, 0x04,
                       ((orig_cmd & 0xFFFFU) | 0x3U) & ~0x4U);
    pci_to_d0(dev);

    info.txcfg = R8169_RD32(&io, R8169_TXCFG);
    const struct r8169_chip *chip = r8169_chip_lookup(info.txcfg);
    if (!chip) {
        printk("[R8169] %04x:%04x TxConfig=0x%08x: unknown chip revision, not used\n",
               (unsigned)dev->vendor_id, (unsigned)dev->device_id,
               (unsigned)info.txcfg);
        pci_restore(dev);
        return;
    }
    info.chip = chip->name;
    info.flags = r8169_chip_flags(chip, info.txcfg, dev->device_id);

    int aspm = (info.flags & R8169_F_PCIE) ? pcie_aspm_off(dev) : 0;

    r8169_hw_stop(&io, info.flags);   /* firmware may have left DMA running */
    r8169_hw_wake(&io, info.flags);
    uint32_t cmd = pci_read_config32(dev->bus, dev->slot, dev->func, 0x04);
    pci_write_config32(dev->bus, dev->slot, dev->func, 0x04,
                       (cmd & 0xFFFFU) & ~(1U << 10));     /* INTx enabled */
    read_mac(dev);
    int rst = hw_restart();
    info.phy_ok = r8169_phy_init(&io, info.flags) == 0;

    info.present = 1;
    char name[8] = "eth0";
    for (int i = 0; i < 4 && net_find_interface(name); i++)
        name[3] = (char)('1' + i);
    r8169_netif = net_register(name, info.mac, NET_MTU_ETHERNET, &info,
                               r8169_net_send, r8169_net_poll);
    if (r8169_netif) {
        r8169_netif->driver = "r8169";
        r8169_netif->describe = r8169_net_describe;
    }

    if (info.irq >= 1 && info.irq <= 15) {
        irq_install_handler(info.irq, r8169_irq);
        pic_unmask(info.irq);
    }

    char link[24];
    link_str(link, sizeof(link));
    printk("[R8169] %04x:%04x %s (TxConfig 0x%08x) %s=0x%x irq=%u mac=%02x:%02x:%02x:%02x:%02x:%02x%s phy=%s%s%s link=%s\n",
           (unsigned)dev->vendor_id, (unsigned)dev->device_id, info.chip,
           (unsigned)info.txcfg, info.bar_phys ? "mmio" : "io",
           info.bar_phys ? (unsigned)info.bar_phys : (unsigned)info.io_base,
           (unsigned)info.irq, info.mac[0], info.mac[1], info.mac[2],
           info.mac[3], info.mac[4], info.mac[5],
           info.mac_generated ? " (generated)" : "",
           info.phy_ok ? "ok" : "no-answer", aspm ? " aspm=off" : "",
           rst ? " reset-timeout" : "", link);
}

int r8169_describe(char *buf, uint32_t cap) {
    if (!info.present || cap == 0)
        return 0;
    char link[24];
    link_str(link, sizeof(link));
    return snprintf(buf, cap,
                    "chip=%s %s=0x%x irq=%u mac=%02x:%02x:%02x:%02x:%02x:%02x link=%s rxnext=%u txpend=%u rxerr=%u rdu=%u fovw=%u missed=%u resets=%u",
                    info.chip,
                    info.bar_phys ? "mmio" : "io",
                    info.bar_phys ? (unsigned)info.bar_phys : (unsigned)info.io_base,
                    (unsigned)info.irq, info.mac[0], info.mac[1], info.mac[2],
                    info.mac[3], info.mac[4], info.mac[5], link,
                    (unsigned)rxq.next, (unsigned)txq.pending,
                    (unsigned)(rxq.errors + rxq.fragments), (unsigned)info.rdu,
                    (unsigned)info.fovw,
                    (unsigned)R8169_RD32(&io, R8169_MPC), (unsigned)info.resets);
}

const r8169_info_t *r8169_get_info(void) {
    return &info;
}
