#pragma once
/*
 * Realtek RTL8169/8168/8111/810x ("r8169") register programming, chip table
 * and descriptor rings, kept free of kernel dependencies so the host harness
 * (tools/test_r8169.c, run by tools/test_r8169.py) can drive it against a
 * simulated register file.  drivers/r8169.c supplies the register accessors
 * (MMIO or port I/O) and the memory.
 *
 * Written from the RTL8169 datasheet's register and descriptor layout, the
 * OSDev wiki "RTL8169" page, and FreeBSD's re(4) (sys/dev/re/if_re.c and
 * sys/dev/rl/if_rlreg.h, BSD licensed, used as the reference for register
 * values, the TxConfig XID table and the per-revision quirks; no code was
 * copied).  Linux's r8169 (GPL) was read only to cross-check facts.
 *
 * Chip model: one normal-priority TX ring and one RX ring of 16-byte
 * descriptors, each ring 256-byte aligned, the last descriptor marked EOR.
 * There is no head/tail register: ownership is the OWN bit.  The driver sets
 * OWN to hand a descriptor to the chip, the chip clears it when done.  A TX
 * frame is kicked by writing NPQ to TxPoll; RX descriptors are re-armed by
 * setting OWN again.
 */
#include <stdint.h>

/* ── Registers (byte offsets in BAR) ─────────────────────────────────────── */
#define R8169_IDR0        0x00   /* MAC address, 6 bytes; 32-bit access */
#define R8169_IDR4        0x04
#define R8169_MAR0        0x08   /* multicast hash, 8 bytes */
#define R8169_MAR4        0x0C
#define R8169_TNPDS_LO    0x20   /* TX normal-priority ring, 256-byte aligned */
#define R8169_TNPDS_HI    0x24
#define R8169_CMD         0x37   /* ChipCmd, 8 bit */
#define R8169_TPPOLL      0x38   /* TxPoll, 8 bit (gigabit parts) */
#define R8169_IMR         0x3C   /* 16 bit */
#define R8169_ISR         0x3E   /* 16 bit, write 1 to clear */
#define R8169_TXCFG       0x40
#define R8169_RXCFG       0x44
#define R8169_MPC         0x4C   /* missed packet counter */
#define R8169_EECMD       0x50   /* Cfg9346: config-register write enable */
#define R8169_CFG2        0x53
#define R8169_PHYAR       0x60   /* MDIO access */
#define R8169_PHYSTATUS   0x6C   /* 8 bit: link/speed/duplex as negotiated */
#define R8169_MACDBG      0x6D   /* 8168C spin 2 */
#define R8169_GPIO        0x6E   /* 8168C spin 2 */
#define R8169_PMCH        0x6F   /* PHY power (8168D/E, 810xE) */
#define R8169_8110SC_7C   0x7C   /* 8110SC/SCe PCI timing magic */
#define R8169_MACRESET    0x82   /* 8169/8110S/SB/SC: write 1 after reset */
#define R8169_D1          0xD1   /* 8401E PHY power bit */
#define R8169_MCU         0xD3   /* 8168G+: FIFO-empty flags */
#define R8169_RMS         0xDA   /* max RX frame length, 16 bit */
#define R8169_CPCMD       0xE0   /* C+ command, 16 bit */
#define R8169_INTRMOD     0xE2   /* interrupt moderation, 16 bit */
#define R8169_RDSAR_LO    0xE4   /* RX ring, 256-byte aligned */
#define R8169_RDSAR_HI    0xE8
#define R8169_ETTHR       0xEC   /* early TX threshold, 8 bit */
#define R8169_MISC        0xF0   /* 8168G+: RXDV gate */

#define R8169_CMD_TE      0x04
#define R8169_CMD_RE      0x08
#define R8169_CMD_RST     0x10
#define R8169_CMD_STOPREQ 0x80

#define R8169_TPPOLL_NPQ  0x40   /* poll the normal-priority TX ring */

#define R8169_EECMD_OFF   0x00
#define R8169_EECMD_WRCFG 0xC0   /* config registers + IDR writable */
#define R8169_CFG2_MSI    0x20

/* Interrupt status/mask bits. */
#define R8169_INT_ROK     0x0001
#define R8169_INT_RER     0x0002
#define R8169_INT_TOK     0x0004
#define R8169_INT_TER     0x0008
#define R8169_INT_RDU     0x0010   /* RX descriptor unavailable (ring full) */
#define R8169_INT_LINKCHG 0x0020
#define R8169_INT_FOVW    0x0040   /* RX FIFO overflow */
#define R8169_INT_TDU     0x0080
#define R8169_INT_SWINT   0x0100
#define R8169_INT_SERR    0x8000   /* PCI system error */
/* TOK is left masked: TX completions are reclaimed by the send path. */
#define R8169_INT_MASK (R8169_INT_ROK | R8169_INT_RER | R8169_INT_TER | \
                        R8169_INT_RDU | R8169_INT_LINKCHG | R8169_INT_FOVW | \
                        R8169_INT_SERR)

#define R8169_TXCFG_XID_MASK  0x7CC00000U
#define R8169_TXCFG_QEMPTY    0x00000800U   /* 8168E-VL and later */
#define R8169_TXCFG_IFG96     0x03000000U
#define R8169_TXCFG_DMA_2048  0x00000700U

#define R8169_RXCFG_AAP       0x00000001U   /* all physical (promiscuous) */
#define R8169_RXCFG_APM       0x00000002U   /* our address */
#define R8169_RXCFG_AM        0x00000004U   /* multicast (hash-filtered) */
#define R8169_RXCFG_AB        0x00000008U   /* broadcast */
#define R8169_RXCFG_ACCEPT    0x0000000FU
#define R8169_RXCFG_EARLYOFFV2 0x00000800U  /* 8168G+ */
#define R8169_RXCFG_EARLYOFF  0x00003800U   /* 8168E-VL/F */
/* No FIFO threshold, unlimited DMA burst; the 0x1800 bits are the RX
 * buffer-size field the 8139C+ heritage leaves in place (re(4) sets them on
 * every part; on 8168G+ they include EARLYOFFV2). */
#define R8169_RXCFG_BASE      (0x0000E000U | 0x00000700U | 0x00001800U)

#define R8169_MISC_RXDV_GATE  0x00080000U
#define R8169_MCU_FIFO_EMPTY  0x30          /* TX (bit 5) and RX (bit 4) */

#define R8169_CPCMD_TXENB     0x0001
#define R8169_CPCMD_RXENB     0x0002
#define R8169_CPCMD_PCI_MRW   0x0008
#define R8169_CPCMD_MACSTAT_DIS 0x0080

#define R8169_PHYAR_FLAG      0x80000000U

/* PHYstatus */
#define R8169_PHY_FDX         0x01
#define R8169_PHY_LINK        0x02
#define R8169_PHY_10M         0x04
#define R8169_PHY_100M        0x08
#define R8169_PHY_1000M       0x10

/* Standard MII registers on the internal PHY. */
#define MII_BMCR      0x00
#define MII_BMSR      0x01
#define MII_ANAR      0x04
#define MII_GTCR      0x09          /* 1000BASE-T control */
#define BMCR_RESET    0x8000
#define BMCR_ANEN     0x1000
#define BMCR_PDOWN    0x0800
#define BMCR_ISO      0x0400
#define BMCR_ANRESTART 0x0200
#define ANAR_CSMA     0x0001
#define ANAR_10       0x0020
#define ANAR_10FD     0x0040
#define ANAR_100      0x0080
#define ANAR_100FD    0x0100
#define ANAR_PAUSE    0x0400
#define ANAR_ASMDIR   0x0800
#define GTCR_1000HD   0x0100
#define GTCR_1000FD   0x0200

/* ── Descriptors ─────────────────────────────────────────────────────────── */
#define R8169_DESC_OWN    0x80000000U
#define R8169_DESC_EOR    0x40000000U
#define R8169_DESC_FS     0x20000000U
#define R8169_DESC_LS     0x10000000U
#define R8169_TX_LEN_MASK 0x0000FFFFU
#define R8169_RX_BUF_MASK 0x00001FFFU   /* buffer size field when armed */
#define R8169_RX_LEN_MASK 0x00003FFFU   /* frame length incl. CRC when done */
/* RX status, gigabit layout (one bit left of the 8139C+ one). */
#define R8169_RX_RES      0x00200000U   /* error summary */
#define R8169_RX_RUNT     0x00100000U
#define R8169_RX_CRC      0x00080000U

#define R8169_ETH_MIN     60            /* frame without CRC; we pad to it */
#define R8169_ETH_CRC     4

struct r8169_desc {
    uint32_t opts1;      /* OWN EOR FS LS ... length */
    uint32_t opts2;      /* VLAN tag (unused) */
    uint32_t addr_lo;
    uint32_t addr_hi;    /* always 0: rings and buffers are below 4 GiB */
};

#define R8169_RING_ALIGN 256

#define R8169_BARRIER() __asm__ volatile("" ::: "memory")

/* ── Chip table ─────────────────────────────────────────────────────────── */
/* Per-revision behaviour, after re(4)'s flags. */
#define R8169_F_PCIE        0x0001  /* PCIe part (8168/810x) */
#define R8169_F_FASTETHER   0x0002  /* 10/100 only: do not advertise 1000 */
#define R8169_F_MACSTAT     0x0004  /* CPlusCmd: MACSTAT_DIS|1, not TX/RXENB */
#define R8169_F_PHYWAKE     0x0008  /* PHY regs 0x1f/0x0e = 0 at attach */
#define R8169_F_PHYWAKE_PM  0x0010  /* PMCH bit 7 = PHY power */
#define R8169_F_MACRESET    0x0020  /* 0x82 = 1 after a reset */
#define R8169_F_CMDSTOP     0x0040  /* stop with ChipCmd STOPREQ */
#define R8169_F_WAIT_TXQ    0x0080  /* ... and wait for TxConfig QEMPTY */
#define R8169_F_WAIT_TXPOLL 0x0100  /* 8168DP: wait for TxPoll to clear */
#define R8169_F_EARLYOFF    0x0200  /* 8168E-VL/F: RxConfig early-off */
#define R8169_F_GPLUS       0x0400  /* 8168G and later: RXDV gate, MCU stop */
#define R8169_F_MACSLEEP    0x0800  /* 8168C/8103E deep sleep via GPIO */
#define R8169_F_8110SC      0x1000  /* 0x7C timing magic */
#define R8169_F_8110SCE     0x2000
#define R8169_F_8169S       0x4000  /* PHY reg 0x0b = 0 after reset */
#define R8169_F_8401E       0x8000  /* 0xD1 bit 3 clear with PHYWAKE_PM */
#define R8169_F_FOVW_RESET  0x10000 /* 8168B: RX FIFO overflow needs a reset */
#define R8169_F_MCBUG       0x20000 /* 8168F: multicast filter unusable */

struct r8169_chip {
    uint32_t xid;        /* TxConfig & R8169_TXCFG_XID_MASK (+bit 31) */
    const char *name;
    uint32_t flags;
};

#define R8169_F_8168B (R8169_F_PCIE | R8169_F_PHYWAKE | R8169_F_MACSTAT)
#define R8169_F_8168C (R8169_F_8168B | R8169_F_CMDSTOP)
#define R8169_F_810X  (R8169_F_PCIE | R8169_F_FASTETHER | R8169_F_PHYWAKE | \
                       R8169_F_MACSTAT | R8169_F_CMDSTOP)
#define R8169_F_8168EVL (R8169_F_8168C | R8169_F_WAIT_TXQ)
#define R8169_F_8168G (R8169_F_8168EVL | R8169_F_GPLUS)

static const struct r8169_chip r8169_chips[] = {
    /* Conventional PCI gigabit (10ec:8169, 10ec:8167 and OEM IDs). */
    { 0x00000000U, "RTL8169",          R8169_F_MACRESET },
    { 0x00800000U, "RTL8169S",         R8169_F_MACRESET | R8169_F_8169S },
    { 0x04000000U, "RTL8110S",         R8169_F_MACRESET },
    { 0x10000000U, "RTL8169SB/8110SB", R8169_F_MACRESET | R8169_F_PHYWAKE },
    { 0x18000000U, "RTL8169SC/8110SC", R8169_F_MACRESET | R8169_F_PHYWAKE |
                                       R8169_F_8110SC },
    { 0x7CC00000U, "RTL8169SBL",       R8169_F_MACRESET | R8169_F_PHYWAKE },
    { 0x98000000U, "RTL8110SCe",       R8169_F_MACRESET | R8169_F_PHYWAKE |
                                       R8169_F_8110SC | R8169_F_8110SCE },
    /* PCIe fast Ethernet (10ec:8136). */
    { 0x30800000U, "RTL8100E",  R8169_F_PCIE | R8169_F_FASTETHER | R8169_F_PHYWAKE },
    { 0x34000000U, "RTL8101E",  R8169_F_PCIE | R8169_F_FASTETHER | R8169_F_PHYWAKE },
    { 0x34800000U, "RTL8102E",  R8169_F_810X },
    { 0x24800000U, "RTL8102EL", R8169_F_810X },
    { 0x24C00000U, "RTL8102EL", R8169_F_810X },
    { 0x34C00000U, "RTL8103E",  R8169_F_810X | R8169_F_MACSLEEP },
    { 0x24000000U, "RTL8401E",  R8169_F_810X | R8169_F_PHYWAKE_PM | R8169_F_8401E },
    { 0x40800000U, "RTL8105E",  R8169_F_810X | R8169_F_PHYWAKE_PM },
    { 0x40C00000U, "RTL8105E",  R8169_F_810X | R8169_F_PHYWAKE_PM },
    { 0x44800000U, "RTL8106E",  R8169_F_810X | R8169_F_PHYWAKE_PM },
    { 0x44000000U, "RTL8402",   R8169_F_810X | R8169_F_PHYWAKE_PM | R8169_F_WAIT_TXQ },
    /* PCIe gigabit (10ec:8168, 8161, 2502, 2600). */
    { 0x30000000U, "RTL8168B/8111B",   R8169_F_8168B | R8169_F_FOVW_RESET },
    { 0x38000000U, "RTL8168B/8111B",   R8169_F_8168B },
    { 0x38400000U, "RTL8168B/8111B",   R8169_F_8168B },
    { 0x3C000000U, "RTL8168C/8111C",   R8169_F_8168C },   /* +MACSLEEP on macrev 2 */
    { 0x3C400000U, "RTL8168C/8111C",   R8169_F_8168C | R8169_F_MACSLEEP },
    { 0x3C800000U, "RTL8168CP/8111CP", R8169_F_8168C },
    { 0x28000000U, "RTL8168D/8111D",   R8169_F_8168C | R8169_F_PHYWAKE_PM },
    { 0x28800000U, "RTL8168DP/8111DP", R8169_F_8168B | R8169_F_WAIT_TXPOLL },
    { 0x2C000000U, "RTL8168E/8111E",   R8169_F_8168C | R8169_F_PHYWAKE_PM },
    { 0x2C800000U, "RTL8168E-VL/8111E-VL", R8169_F_8168EVL | R8169_F_EARLYOFF },
    { 0x48000000U, "RTL8168F/8111F",   R8169_F_8168EVL | R8169_F_EARLYOFF |
                                       R8169_F_MCBUG },
    { 0x48800000U, "RTL8411",          R8169_F_8168EVL },
    { 0x4C000000U, "RTL8168G/8111G",   R8169_F_8168G },
    { 0x50000000U, "RTL8168EP/8111EP", R8169_F_8168G },
    { 0x50800000U, "RTL8168GU/8111GU", R8169_F_8168G },
    { 0x54000000U, "RTL8168H/8111H",   R8169_F_8168G },
    { 0x54800000U, "RTL8168FP/8117",   R8169_F_8168G },
    { 0x5C800000U, "RTL8411B",         R8169_F_8168G },
};

/* The revision key of a TxConfig value: XID bits 30-26 and 23-22, plus bit
 * 31 for the conventional-PCI 8169 family (top nibble 0x0 or 0x1 there,
 * where the 8110SCe is told apart by bit 31). */
static inline uint32_t r8169_xid(uint32_t txcfg) {
    uint32_t top = txcfg & 0x70000000U;
    if (top == 0x00000000U || top == 0x10000000U)
        return txcfg & (R8169_TXCFG_XID_MASK | 0x80000000U);
    return txcfg & R8169_TXCFG_XID_MASK;
}

/* The chip a TxConfig value names, 0 for one this driver does not know
 * (left alone: an unknown revision may need init this driver lacks), or
 * all-ones (nothing decoding the BAR, which would otherwise read as the
 * 8169SBL's XID). */
static inline const struct r8169_chip *r8169_chip_lookup(uint32_t txcfg) {
    if (txcfg == 0xFFFFFFFFU)     /* no device answering (or in D3) */
        return 0;
    uint32_t xid = r8169_xid(txcfg);
    for (uint32_t i = 0; i < sizeof(r8169_chips) / sizeof(r8169_chips[0]); i++)
        if (r8169_chips[i].xid == xid)
            return &r8169_chips[i];
    return 0;
}

/* Flags for this device: the table's, plus what depends on the MAC revision
 * (TxConfig bits 22-20) or the PCI device ID. */
static inline uint32_t r8169_chip_flags(const struct r8169_chip *c,
                                        uint32_t txcfg, uint16_t device_id) {
    uint32_t f = c->flags;
    if (c->xid == 0x3C000000U && (txcfg & 0x00700000U) == 0x00200000U)
        f |= R8169_F_MACSLEEP;
    /* 8168GU/8168H silicon on a 10ec:8136 card is the 8106EUS/8107E. */
    if (device_id == 0x8136 && (f & R8169_F_GPLUS))
        f |= R8169_F_FASTETHER;
    return f;
}

/* ── Register access ─────────────────────────────────────────────────────── */
struct r8169_io {
    void *ctx;
    uint8_t  (*rd8)(void *ctx, uint32_t reg);
    uint16_t (*rd16)(void *ctx, uint32_t reg);
    uint32_t (*rd32)(void *ctx, uint32_t reg);
    void (*wr8)(void *ctx, uint32_t reg, uint8_t v);
    void (*wr16)(void *ctx, uint32_t reg, uint16_t v);
    void (*wr32)(void *ctx, uint32_t reg, uint32_t v);
    void (*udelay)(void *ctx, uint32_t us);
    /* PCI bus mastering on; optional.  Called by r8169_hw_start() only, so
     * the chip can DMA only once it is stopped, reset and has fresh rings:
     * a ring firmware left armed must never reach kernel memory. */
    void (*bus_master)(void *ctx);
};

#define R8169_RD8(io, r)     ((io)->rd8((io)->ctx, (r)))
#define R8169_RD16(io, r)    ((io)->rd16((io)->ctx, (r)))
#define R8169_RD32(io, r)    ((io)->rd32((io)->ctx, (r)))
#define R8169_WR8(io, r, v)  ((io)->wr8((io)->ctx, (r), (uint8_t)(v)))
#define R8169_WR16(io, r, v) ((io)->wr16((io)->ctx, (r), (uint16_t)(v)))
#define R8169_WR32(io, r, v) ((io)->wr32((io)->ctx, (r), (uint32_t)(v)))
#define R8169_DELAY(io, us)  ((io)->udelay((io)->ctx, (us)))

/* Poll until (reg & mask) == want, `tries` times `us` apart; 0 on success. */
static inline int r8169_wait8(const struct r8169_io *io, uint32_t reg,
                              uint8_t mask, uint8_t want, int tries, uint32_t us) {
    for (int i = 0; i < tries; i++) {
        if ((R8169_RD8(io, reg) & mask) == want)
            return 0;
        R8169_DELAY(io, us);
    }
    return -1;
}

static inline int r8169_wait32(const struct r8169_io *io, uint32_t reg,
                               uint32_t mask, uint32_t want, int tries,
                               uint32_t us) {
    for (int i = 0; i < tries; i++) {
        if ((R8169_RD32(io, reg) & mask) == want)
            return 0;
        R8169_DELAY(io, us);
    }
    return -1;
}

/* ── MDIO (PHYAR) ────────────────────────────────────────────────────────── */
/* A read starts with FLAG clear and completes when the chip sets FLAG; a
 * write starts with FLAG set and completes when the chip clears it.  The
 * chip wants ~20 us between transactions.  Up to 50 ms per access. */
static inline int r8169_mdio_read(const struct r8169_io *io, int reg) {
    R8169_WR32(io, R8169_PHYAR, ((uint32_t)reg & 0x1F) << 16);
    if (r8169_wait32(io, R8169_PHYAR, R8169_PHYAR_FLAG, R8169_PHYAR_FLAG,
                     2000, 25) != 0)
        return -1;
    uint32_t v = R8169_RD32(io, R8169_PHYAR);
    R8169_DELAY(io, 20);
    return (int)(v & 0xFFFF);
}

static inline int r8169_mdio_write(const struct r8169_io *io, int reg,
                                   uint16_t val) {
    R8169_WR32(io, R8169_PHYAR, R8169_PHYAR_FLAG |
                                (((uint32_t)reg & 0x1F) << 16) | val);
    int r = r8169_wait32(io, R8169_PHYAR, R8169_PHYAR_FLAG, 0, 2000, 25);
    R8169_DELAY(io, 20);
    return r;
}

/* ── TX ring ─────────────────────────────────────────────────────────────── */
/*
 * Slots clean .. next-1 (`pending` of them) are posted.  Unlike the e1000
 * there is no TDT == TDH ambiguity: the chip stops at the first descriptor
 * it does not own, so every slot is usable and `pending` tells full from
 * empty.  Stall detection as in drivers/e1000_txq.h.
 */
#define R8169_TXQ_STALL_TICKS 200   /* 2 s at the PIT's 100 Hz */

struct r8169_txq {
    uint32_t size, next, clean, pending, stall_tick;
};

static inline void r8169_tx_init(struct r8169_txq *q, struct r8169_desc *ring,
                                 uint32_t size) {
    for (uint32_t i = 0; i < size; i++) {
        ring[i].opts2 = 0;
        ring[i].addr_lo = 0;
        ring[i].addr_hi = 0;
        ring[i].opts1 = (i == size - 1) ? R8169_DESC_EOR : 0;
    }
    q->size = size;
    q->next = q->clean = q->pending = q->stall_tick = 0;
}

static inline int r8169_tx_full(const struct r8169_txq *q) {
    return q->pending == q->size;
}

/* Bytes the chip is told to send for a `len`-byte frame: short frames are
 * padded (by the caller, with zeros) to the 60-byte minimum, since only some
 * revisions pad by themselves. */
static inline uint32_t r8169_tx_wire_len(uint32_t len) {
    return len < R8169_ETH_MIN ? R8169_ETH_MIN : len;
}

/* Advance clean over descriptors the chip has given back; returns how many.
 * Stops at the first one still owned: completions are in order. */
static inline uint32_t r8169_tx_reclaim(struct r8169_txq *q,
                                        const struct r8169_desc *ring) {
    uint32_t freed = 0;
    while (q->pending &&
           !(((const volatile struct r8169_desc *)&ring[q->clean])->opts1 &
             R8169_DESC_OWN)) {
        q->clean = (q->clean + 1) % q->size;
        q->pending--;
        freed++;
    }
    if (freed)
        q->stall_tick = 0;
    return freed;
}

/* Post one single-descriptor frame (caller checked !full); returns the slot.
 * OWN is written last, after the address and length. */
static inline uint32_t r8169_tx_post(struct r8169_txq *q,
                                     struct r8169_desc *ring,
                                     uint32_t buf_phys, uint32_t len) {
    uint32_t idx = q->next;
    volatile struct r8169_desc *d = &ring[idx];
    d->addr_lo = buf_phys;
    d->addr_hi = 0;
    d->opts2 = 0;
    R8169_BARRIER();
    d->opts1 = R8169_DESC_OWN | R8169_DESC_FS | R8169_DESC_LS |
               (idx == q->size - 1 ? R8169_DESC_EOR : 0) |
               (len & R8169_TX_LEN_MASK);
    q->next = (idx + 1) % q->size;
    q->pending++;
    return idx;
}

/* 1 when frames have been posted with no completion for STALL_TICKS. */
static inline int r8169_tx_stalled(struct r8169_txq *q,
                                   const struct r8169_desc *ring, uint32_t now) {
    r8169_tx_reclaim(q, ring);
    if (!q->pending) {
        q->stall_tick = 0;
        return 0;
    }
    if (!q->stall_tick) {
        q->stall_tick = now ? now : 1;
        return 0;
    }
    return now - q->stall_tick >= R8169_TXQ_STALL_TICKS;
}

/* ── RX ring ─────────────────────────────────────────────────────────────── */
struct r8169_rxq {
    uint32_t size, next, buf_size;
    uint32_t errors;      /* descriptors with the error summary bit */
    uint32_t fragments;   /* pieces of frames spread over descriptors */
};

enum { R8169_RX_NONE = 0, R8169_RX_FRAME = 1, R8169_RX_DROP = 2 };

static inline void r8169_rx_arm(struct r8169_rxq *q, struct r8169_desc *ring,
                                uint32_t idx) {
    volatile struct r8169_desc *d = &ring[idx];
    d->opts2 = 0;
    R8169_BARRIER();
    d->opts1 = R8169_DESC_OWN | (idx == q->size - 1 ? R8169_DESC_EOR : 0) |
               (q->buf_size & R8169_RX_BUF_MASK);
}

/* buf_phys[i] is buffer i's bus address. */
static inline void r8169_rx_init(struct r8169_rxq *q, struct r8169_desc *ring,
                                 uint32_t size, const uint32_t *buf_phys,
                                 uint32_t buf_size) {
    q->size = size;
    q->next = 0;
    q->buf_size = buf_size;
    q->errors = q->fragments = 0;
    for (uint32_t i = 0; i < size; i++) {
        ring[i].addr_lo = buf_phys[i];
        ring[i].addr_hi = 0;
        r8169_rx_arm(q, ring, i);
    }
}

/*
 * Look at the next descriptor.  NONE: the chip still owns it.  Otherwise
 * *idx is the slot, which the caller hands back with r8169_rx_recycle()
 * after FRAME (*len bytes, CRC stripped, in buffer *idx) or DROP (an error,
 * or one piece of a frame larger than a buffer: RMS equals the buffer size,
 * so the chip should never split one, but a piece is dropped whole if it
 * does).
 */
static inline int r8169_rx_next(struct r8169_rxq *q,
                                const struct r8169_desc *ring,
                                uint32_t *idx, uint32_t *len) {
    uint32_t i = q->next;
    uint32_t o = ((const volatile struct r8169_desc *)&ring[i])->opts1;
    if (o & R8169_DESC_OWN)
        return R8169_RX_NONE;
    R8169_BARRIER();      /* status before the data */
    *idx = i;
    *len = 0;
    if ((o & (R8169_DESC_FS | R8169_DESC_LS)) !=
        (R8169_DESC_FS | R8169_DESC_LS)) {
        q->fragments++;
        return R8169_RX_DROP;
    }
    if (o & R8169_RX_RES) {
        q->errors++;
        return R8169_RX_DROP;
    }
    uint32_t n = o & R8169_RX_LEN_MASK;
    if (n < 14 + R8169_ETH_CRC || n > q->buf_size) {
        q->errors++;
        return R8169_RX_DROP;
    }
    *len = n - R8169_ETH_CRC;
    return R8169_RX_FRAME;
}

/* Give slot idx (the one r8169_rx_next returned) back to the chip. */
static inline void r8169_rx_recycle(struct r8169_rxq *q,
                                    struct r8169_desc *ring, uint32_t idx) {
    r8169_rx_arm(q, ring, idx);
    q->next = (idx + 1) % q->size;
}

/* ── Stop, reset, start ──────────────────────────────────────────────────── */
/*
 * Quiesce DMA before a reset: firmware (a PXE ROM, UEFI's network stack) can
 * leave the chip running on rings in memory we are about to reuse.  Stop
 * accepting frames first, then the per-generation stop handshake, then mask
 * and clear interrupts.
 */
static inline void r8169_hw_stop(const struct r8169_io *io, uint32_t flags) {
    R8169_WR32(io, R8169_RXCFG, R8169_RD32(io, R8169_RXCFG) & ~R8169_RXCFG_ACCEPT);
    if (flags & R8169_F_GPLUS) {
        /* 8168G+: STOPREQ is not defined; gate RXDV, let the TX queue and
         * both FIFOs drain, then clear TE/RE. */
        R8169_WR32(io, R8169_MISC, R8169_RD32(io, R8169_MISC) | R8169_MISC_RXDV_GATE);
        R8169_DELAY(io, 2000);
        r8169_wait32(io, R8169_TXCFG, R8169_TXCFG_QEMPTY, R8169_TXCFG_QEMPTY,
                     1000, 100);
        R8169_WR8(io, R8169_CMD, R8169_RD8(io, R8169_CMD) &
                                 ~(R8169_CMD_TE | R8169_CMD_RE));
        r8169_wait8(io, R8169_MCU, R8169_MCU_FIFO_EMPTY, R8169_MCU_FIFO_EMPTY,
                    3000, 20);
    } else if (flags & R8169_F_WAIT_TXPOLL) {
        r8169_wait8(io, R8169_TPPOLL, R8169_TPPOLL_NPQ, 0, 1000, 20);
        R8169_WR8(io, R8169_CMD, 0);
    } else if (flags & R8169_F_CMDSTOP) {
        R8169_WR8(io, R8169_CMD, R8169_CMD_STOPREQ | R8169_CMD_TE | R8169_CMD_RE);
        if (flags & R8169_F_WAIT_TXQ)
            r8169_wait32(io, R8169_TXCFG, R8169_TXCFG_QEMPTY,
                         R8169_TXCFG_QEMPTY, 1000, 100);
    } else {
        R8169_WR8(io, R8169_CMD, 0);
    }
    R8169_DELAY(io, 1000);
    R8169_WR16(io, R8169_IMR, 0);
    R8169_WR16(io, R8169_ISR, 0xFFFF);
}

/* Software reset; 0 on success (RST self-clears within ~10 ms). */
static inline int r8169_hw_reset(const struct r8169_io *io, uint32_t flags) {
    R8169_WR8(io, R8169_CMD, R8169_CMD_RST);
    int r = r8169_wait8(io, R8169_CMD, R8169_CMD_RST, 0, 1000, 10);
    if (flags & R8169_F_MACRESET)
        R8169_WR8(io, R8169_MACRESET, 1);
    if (flags & R8169_F_8169S)
        r8169_mdio_write(io, 0x0B, 0);
    return r;
}

/*
 * Program a freshly reset chip (rings already initialised): bus mastering
 * on, then C+ mode, ring addresses, TX/RX config, all
 * multicast accepted (the 8168F's hash filter is broken anyway, and lwIP
 * filters what it does not want), interrupts on.  The 8169..8168F want the
 * MAC enabled before TxConfig/RxConfig are written; the 8168G+ after, with
 * the RXDV gate opened first.  `rms` is the RX buffer size: no frame larger
 * than one buffer is accepted.
 */
static inline void r8169_hw_start(const struct r8169_io *io, uint32_t flags,
                                   uint32_t rx_ring_phys, uint32_t tx_ring_phys,
                                   uint16_t rms) {
    if (io->bus_master)
        io->bus_master(io->ctx);
    uint16_t cp = R8169_CPCMD_PCI_MRW;
    if (flags & R8169_F_MACSTAT)
        cp |= R8169_CPCMD_MACSTAT_DIS | 0x0001;   /* re(4): vendor magic */
    else
        cp |= R8169_CPCMD_RXENB | R8169_CPCMD_TXENB;
    R8169_WR16(io, R8169_CPCMD, cp);              /* before anything else */

    if (flags & R8169_F_8110SC) {
        uint32_t v = 0x000FFF00U;
        if (R8169_RD8(io, R8169_CFG2) & 0x01)      /* 66 MHz PCI */
            v |= 0x000000FFU;
        if (flags & R8169_F_8110SCE)
            v |= 0x00F00000U;
        R8169_WR32(io, R8169_8110SC_7C, v);
    }

    R8169_WR16(io, R8169_RMS, rms);

    R8169_WR32(io, R8169_RDSAR_HI, 0);
    R8169_WR32(io, R8169_RDSAR_LO, rx_ring_phys);
    R8169_WR32(io, R8169_TNPDS_HI, 0);
    R8169_WR32(io, R8169_TNPDS_LO, tx_ring_phys);

    if (flags & R8169_F_GPLUS)
        R8169_WR32(io, R8169_MISC, R8169_RD32(io, R8169_MISC) & ~R8169_MISC_RXDV_GATE);
    else
        R8169_WR8(io, R8169_CMD, R8169_CMD_TE | R8169_CMD_RE);

    R8169_WR32(io, R8169_TXCFG, R8169_TXCFG_IFG96 | R8169_TXCFG_DMA_2048);
    R8169_WR8(io, R8169_ETTHR, 16);

    uint32_t rx = R8169_RXCFG_BASE | R8169_RXCFG_APM | R8169_RXCFG_AB |
                  R8169_RXCFG_AM;
    if (flags & R8169_F_EARLYOFF)
        rx |= R8169_RXCFG_EARLYOFF;
    else if (flags & R8169_F_GPLUS)
        rx |= R8169_RXCFG_EARLYOFFV2;
    R8169_WR32(io, R8169_MAR0, 0xFFFFFFFFU);
    R8169_WR32(io, R8169_MAR4, 0xFFFFFFFFU);
    R8169_WR32(io, R8169_RXCFG, rx);

    R8169_WR16(io, R8169_INTRMOD, 0x5100);        /* re(4): vendor value */

    if (flags & R8169_F_GPLUS)
        R8169_WR8(io, R8169_CMD, R8169_CMD_TE | R8169_CMD_RE);

    R8169_WR16(io, R8169_ISR, 0xFFFF);
    R8169_WR16(io, R8169_IMR, R8169_INT_MASK);
    R8169_WR32(io, R8169_MPC, 0);
}

/*
 * One-time wake-up at attach: out of the 8168C/8103E deep sleep, PHY power
 * on (PMCH) where the chip has that switch, and the MSI enable in Config2
 * off since the interrupt is the legacy INTx line.
 */
static inline void r8169_hw_wake(const struct r8169_io *io, uint32_t flags) {
    if (flags & R8169_F_MACSLEEP) {
        if (R8169_RD8(io, R8169_MACDBG) & 0x80)
            R8169_WR8(io, R8169_GPIO, R8169_RD8(io, R8169_GPIO) | 0x01);
        else
            R8169_WR8(io, R8169_GPIO, R8169_RD8(io, R8169_GPIO) & ~0x01);
    }
    if (flags & R8169_F_PHYWAKE_PM) {
        R8169_WR8(io, R8169_PMCH, R8169_RD8(io, R8169_PMCH) | 0x80);
        if (flags & R8169_F_8401E)
            R8169_WR8(io, R8169_D1, R8169_RD8(io, R8169_D1) & ~0x08);
    }
    R8169_WR8(io, R8169_EECMD, R8169_EECMD_WRCFG);
    uint8_t cfg2 = R8169_RD8(io, R8169_CFG2);
    if (cfg2 & R8169_CFG2_MSI)
        R8169_WR8(io, R8169_CFG2, cfg2 & ~R8169_CFG2_MSI);
    R8169_WR8(io, R8169_EECMD, R8169_EECMD_OFF);
}

/*
 * One-time PHY bring-up: wake it (re(4)'s PHYWAKE: page 0, reg 0x0e = 0),
 * reset it through BMCR, advertise 10/100 (and 1000 on gigabit parts) with
 * pause, and restart autonegotiation.  The link comes up asynchronously
 * (LinkChg).  Returns -1 if the PHY does not answer on MDIO.
 */
static inline int r8169_phy_init(const struct r8169_io *io, uint32_t flags) {
    if (flags & R8169_F_PHYWAKE) {
        r8169_mdio_write(io, 0x1F, 0);
        r8169_mdio_write(io, 0x0E, 0);
    }
    if (r8169_mdio_read(io, MII_BMSR) < 0)
        return -1;
    r8169_mdio_write(io, 0x1F, 0);                /* standard register page */
    r8169_mdio_write(io, MII_BMCR, BMCR_RESET);
    for (int i = 0; i < 500; i++) {               /* up to 500 ms */
        int v = r8169_mdio_read(io, MII_BMCR);
        if (v >= 0 && !(v & BMCR_RESET))
            break;
        R8169_DELAY(io, 1000);
    }
    int anar = r8169_mdio_read(io, MII_ANAR);
    if (anar < 0)
        return -1;
    anar &= ~(ANAR_10 | ANAR_10FD | ANAR_100 | ANAR_100FD | ANAR_PAUSE |
              ANAR_ASMDIR | 0x001F);
    anar |= ANAR_CSMA | ANAR_10 | ANAR_10FD | ANAR_100 | ANAR_100FD |
            ANAR_PAUSE | ANAR_ASMDIR;
    r8169_mdio_write(io, MII_ANAR, (uint16_t)anar);
    if (!(flags & R8169_F_FASTETHER)) {
        int g = r8169_mdio_read(io, MII_GTCR);
        if (g >= 0)
            r8169_mdio_write(io, MII_GTCR,
                             (uint16_t)(g | GTCR_1000FD | GTCR_1000HD));
    }
    r8169_mdio_write(io, MII_BMCR, BMCR_ANEN | BMCR_ANRESTART);
    return 0;
}
