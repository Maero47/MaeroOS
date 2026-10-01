/*
 * Host test for drivers/r8169_hw.h; run by tools/test_r8169.py.
 *
 * No emulator models these chips, so this drives the driver's register
 * programming and ring logic against a simulated chip: a 256-byte register
 * file with the side effects the code relies on (self-clearing reset, PHYAR
 * handshake, write-1-to-clear ISR, TxPoll kicking the TX DMA, the RXDV gate,
 * FIFO-empty flags), an internal PHY, and DMA engines that walk the rings by
 * OWN/EOR exactly as the datasheet describes.  Bus addresses are cookies the
 * simulated DMA maps back to host memory.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "r8169_hw.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* ── Simulated chip ──────────────────────────────────────────────────────── */
#define NDESC 16
#define BUFSZ 2048
#define RX_RING_PHYS 0x00100000U
#define TX_RING_PHYS 0x00100100U
#define RX_BUF_PHYS  0x00200000U
#define TX_BUF_PHYS  0x00400000U

struct wlog { uint32_t reg, val; int size; };

static struct chip {
    uint8_t r[256];
    uint32_t xid;            /* TxConfig XID bits */
    uint16_t phy[32];
    int phy_present;
    int bmcr_reset_reads;    /* BMCR reset clears after this many reads */
    int rst_reads;           /* ChipCmd RST clears after this many reads */
    int phyar_pending_read;
    int tx_hold;             /* DMA engine stalled (link down) */
    uint32_t tx_idx, rx_idx; /* the chip's own ring positions */
    struct r8169_desc *rx_ring, *tx_ring;
    uint8_t (*rx_bufs)[BUFSZ];
    uint8_t (*tx_bufs)[BUFSZ];
    uint32_t rx_size, tx_size;
    /* frames the TX DMA took, in order */
    uint32_t sent, sent_len[4096];
    uint8_t sent_first[4096];
    struct wlog log[4096];
    int nlog;
    uint64_t delay_us;
    int resets;
    int eor_violation;
    int bme;                 /* PCI bus mastering: no DMA without it */
    int bme_calls, bme_at_log;   /* log position when it was turned on */
    int reset_with_bme;      /* resets issued while BME was on */
    int dma_blocked;         /* DMA attempts refused for lack of BME */
} C;

static uint32_t get32(uint32_t reg) {
    uint32_t v;
    memcpy(&v, &C.r[reg], 4);
    return v;
}
static void put32(uint32_t reg, uint32_t v) { memcpy(&C.r[reg], &v, 4); }

static void logw(uint32_t reg, uint32_t val, int size) {
    if (C.nlog < 4096)
        C.log[C.nlog++] = (struct wlog){ reg, val, size };
}

static void chip_reset_state(uint32_t xid) {
    memset(&C, 0, sizeof(C));
    C.xid = xid;
    C.phy_present = 1;
    C.bmcr_reset_reads = 2;
    C.rst_reads = 3;
    put32(R8169_TXCFG, xid);
    C.r[R8169_MCU] = R8169_MCU_FIFO_EMPTY;
    const uint8_t mac[6] = { 0x00, 0xE0, 0x4C, 0x68, 0x01, 0x02 };
    memcpy(&C.r[R8169_IDR0], mac, 6);
    C.phy[MII_BMSR] = 0x7949;
    C.phy[MII_ANAR] = 0x0001;
}

/* TX DMA: from its own position, take every descriptor it owns; stop at the
 * first it does not.  Wraps at EOR (the only way it knows the ring size). */
static void chip_tx_run(void) {
    if (C.tx_hold || !(C.r[R8169_CMD] & R8169_CMD_TE))
        return;
    if (!C.bme) {
        C.dma_blocked++;
        return;
    }
    CHECK(get32(R8169_TNPDS_LO) == TX_RING_PHYS, "TX ring address 0x%x",
          get32(R8169_TNPDS_LO));
    for (uint32_t n = 0; n < C.tx_size; n++) {
        struct r8169_desc *d = &C.tx_ring[C.tx_idx];
        if (!(d->opts1 & R8169_DESC_OWN))
            break;
        CHECK((d->opts1 & (R8169_DESC_FS | R8169_DESC_LS)) ==
              (R8169_DESC_FS | R8169_DESC_LS), "TX desc %u not FS|LS", C.tx_idx);
        int eor = (d->opts1 & R8169_DESC_EOR) != 0;
        if (eor != (C.tx_idx == C.tx_size - 1))
            C.eor_violation++;
        uint32_t b = (d->addr_lo - TX_BUF_PHYS) / BUFSZ;
        CHECK(b < C.tx_size && d->addr_hi == 0, "TX buffer address 0x%x", d->addr_lo);
        if (C.sent < 4096) {
            C.sent_len[C.sent] = d->opts1 & R8169_TX_LEN_MASK;
            C.sent_first[C.sent] = C.tx_bufs[b][0];
        }
        C.sent++;
        d->opts1 &= ~R8169_DESC_OWN;          /* status: done, no error */
        C.tx_idx = eor ? 0 : C.tx_idx + 1;
    }
    C.r[R8169_ISR] |= R8169_INT_TOK;
}

/* RX DMA: one frame of `len` bytes (CRC included) with first byte `tag`.
 * A frame larger than a buffer is spread over descriptors (FS..LS), as the
 * chip does when RMS exceeds the buffer.  `err` adds RES|CRC.  Returns 0, or
 * -1 for "descriptor unavailable" (RDU, frame lost). */
static int chip_rx_frame(uint32_t len, uint8_t tag, int err) {
    uint32_t left = len;
    int first = 1;
    if (!C.bme) {
        C.dma_blocked++;
        return -1;
    }
    while (left) {
        struct r8169_desc *d = &C.rx_ring[C.rx_idx];
        if (!(d->opts1 & R8169_DESC_OWN) || !(C.r[R8169_CMD] & R8169_CMD_RE) ||
            (get32(R8169_MISC) & R8169_MISC_RXDV_GATE)) {
            C.r[R8169_ISR] |= R8169_INT_RDU;
            return -1;
        }
        uint32_t cap = d->opts1 & R8169_RX_BUF_MASK;
        int eor = (d->opts1 & R8169_DESC_EOR) != 0;
        if (eor != (C.rx_idx == C.rx_size - 1))
            C.eor_violation++;
        uint32_t b = (d->addr_lo - RX_BUF_PHYS) / BUFSZ;
        uint32_t chunk = left < cap ? left : cap;
        memset(C.rx_bufs[b], tag, chunk);
        left -= chunk;
        uint32_t o = (eor ? R8169_DESC_EOR : 0) | (first ? R8169_DESC_FS : 0) |
                     (left ? 0 : R8169_DESC_LS) | (len & R8169_RX_LEN_MASK);
        if (err)
            o |= R8169_RX_RES | R8169_RX_CRC;
        d->opts2 = 0x1234;                    /* garbage the driver must clear */
        d->opts1 = o;                         /* OWN cleared */
        first = 0;
        C.rx_idx = eor ? 0 : C.rx_idx + 1;
    }
    C.r[R8169_ISR] |= R8169_INT_ROK;
    return 0;
}

static uint8_t s_rd8(void *c, uint32_t reg) {
    (void)c;
    if (reg == R8169_CMD && (C.r[reg] & R8169_CMD_RST)) {
        if (--C.rst_reads <= 0)
            C.r[reg] &= ~R8169_CMD_RST;
    }
    return C.r[reg];
}
static uint16_t s_rd16(void *c, uint32_t reg) {
    (void)c;
    uint16_t v;
    memcpy(&v, &C.r[reg], 2);
    return v;
}
static uint32_t s_rd32(void *c, uint32_t reg) {
    (void)c;
    if (reg == R8169_PHYAR && C.phyar_pending_read && C.phy_present) {
        uint32_t pr = (get32(R8169_PHYAR) >> 16) & 0x1F;
        uint16_t v = C.phy[pr];
        if (pr == MII_BMCR && (v & BMCR_RESET) && --C.bmcr_reset_reads <= 0)
            C.phy[pr] = v = (uint16_t)(v & ~BMCR_RESET);
        put32(R8169_PHYAR, R8169_PHYAR_FLAG | (pr << 16) | v);
        C.phyar_pending_read = 0;
    }
    if (reg == R8169_TXCFG)
        return get32(reg) | R8169_TXCFG_QEMPTY;
    return get32(reg);
}
static void s_wr8(void *c, uint32_t reg, uint8_t v) {
    (void)c;
    logw(reg, v, 1);
    if (reg == R8169_CMD) {
        if (v & R8169_CMD_RST) {
            C.resets++;
            if (C.bme)
                C.reset_with_bme++;
            C.rst_reads = 3;
            C.r[reg] = R8169_CMD_RST;
            put32(R8169_RXCFG, 0);
            C.tx_idx = C.rx_idx = 0;
            return;
        }
        C.r[reg] = v & (R8169_CMD_TE | R8169_CMD_RE);
        return;
    }
    if (reg == R8169_TPPOLL) {
        if (v & R8169_TPPOLL_NPQ)
            chip_tx_run();
        return;
    }
    C.r[reg] = v;
}
static void s_wr16(void *c, uint32_t reg, uint16_t v) {
    (void)c;
    logw(reg, v, 2);
    if (reg == R8169_ISR) {
        uint16_t cur;
        memcpy(&cur, &C.r[reg], 2);
        cur &= (uint16_t)~v;
        memcpy(&C.r[reg], &cur, 2);
        return;
    }
    memcpy(&C.r[reg], &v, 2);
}
static void s_wr32(void *c, uint32_t reg, uint32_t v) {
    (void)c;
    logw(reg, v, 4);
    if (reg == R8169_PHYAR) {
        if (!C.phy_present) {
            put32(reg, v);                    /* never completes */
            return;
        }
        uint32_t pr = (v >> 16) & 0x1F;
        if (v & R8169_PHYAR_FLAG) {           /* write: done at once */
            C.phy[pr] = (uint16_t)v;
            put32(reg, v & ~R8169_PHYAR_FLAG);
        } else {                              /* read: done on next poll */
            put32(reg, v);
            C.phyar_pending_read = 1;
        }
        return;
    }
    if (reg == R8169_TXCFG) {                 /* XID bits are read-only */
        put32(reg, (v & ~R8169_TXCFG_XID_MASK & ~0x80000000U) | C.xid);
        return;
    }
    put32(reg, v);
}
static void s_udelay(void *c, uint32_t us) { (void)c; C.delay_us += us; }
static void s_bus_master(void *c) {
    (void)c;
    C.bme = 1;
    C.bme_calls++;
    C.bme_at_log = C.nlog;
}

static const struct r8169_io sim = {
    0, s_rd8, s_rd16, s_rd32, s_wr8, s_wr16, s_wr32, s_udelay, s_bus_master
};

/* Index of the first logged write of `val` (masked) to reg, -1 if none. */
static int find_write(uint32_t reg, uint32_t mask, uint32_t val, int from) {
    for (int i = from; i < C.nlog; i++)
        if (C.log[i].reg == reg && (C.log[i].val & mask) == val)
            return i;
    return -1;
}
static int last_write(uint32_t reg) {
    for (int i = C.nlog - 1; i >= 0; i--)
        if (C.log[i].reg == reg)
            return i;
    return -1;
}

/* ── Driver-side memory ──────────────────────────────────────────────────── */
static struct r8169_desc rx_ring[NDESC] __attribute__((aligned(256)));
static struct r8169_desc tx_ring[NDESC] __attribute__((aligned(256)));
static uint8_t rx_bufs[NDESC][BUFSZ];
static uint8_t tx_bufs[NDESC][BUFSZ];
static struct r8169_rxq rxq;
static struct r8169_txq txq;

static void attach(uint32_t txcfg, uint16_t device_id, uint32_t *flags_out) {
    chip_reset_state(txcfg & (R8169_TXCFG_XID_MASK | 0x80000000U | 0x00700000U));
    const struct r8169_chip *chip = r8169_chip_lookup(txcfg);
    CHECK(chip != 0, "no chip for 0x%08x", txcfg);
    uint32_t flags = chip ? r8169_chip_flags(chip, txcfg, device_id) : 0;
    C.rx_ring = rx_ring; C.tx_ring = tx_ring;
    C.rx_bufs = rx_bufs; C.tx_bufs = tx_bufs;
    C.rx_size = C.tx_size = NDESC;
    /* Firmware left the MAC running with frames accepted and its RX ring
     * armed; the driver has turned bus mastering off (r8169_init), so a
     * frame arriving now must not be DMA'd anywhere. */
    C.r[R8169_CMD] = R8169_CMD_TE | R8169_CMD_RE;
    put32(R8169_RXCFG, 0xE70F);
    for (int i = 0; i < NDESC; i++)
        rx_ring[i].opts1 = R8169_DESC_OWN | BUFSZ;
    CHECK(chip_rx_frame(100, 0x99, 0) < 0 && C.dma_blocked == 1,
          "DMA with bus mastering off");

    r8169_hw_stop(&sim, flags);
    r8169_hw_wake(&sim, flags);
    CHECK(r8169_hw_reset(&sim, flags) == 0, "reset timed out");
    uint32_t phys[NDESC];
    for (int i = 0; i < NDESC; i++)
        phys[i] = RX_BUF_PHYS + (uint32_t)i * BUFSZ;
    r8169_rx_init(&rxq, rx_ring, NDESC, phys, BUFSZ);
    r8169_tx_init(&txq, tx_ring, NDESC);
    CHECK(!C.bme && C.reset_with_bme == 0, "bus mastering on before start");
    C.nlog = 0;                       /* check the start sequence alone */
    r8169_hw_start(&sim, flags, RX_RING_PHYS, TX_RING_PHYS, BUFSZ);
    CHECK(C.bme && C.bme_calls == 1 && C.bme_at_log == 0,
          "bus mastering not turned on first in start (calls %d, at write %d)",
          C.bme_calls, C.bme_at_log);
    *flags_out = flags;
}

static uint32_t send(uint32_t len, uint8_t tag) {
    r8169_tx_reclaim(&txq, tx_ring);
    if (r8169_tx_full(&txq))
        return (uint32_t)-1;
    uint32_t idx = txq.next;
    uint32_t wire = r8169_tx_wire_len(len);
    memset(tx_bufs[idx], tag, len);
    memset(tx_bufs[idx] + len, 0, wire - len);
    r8169_tx_post(&txq, tx_ring, TX_BUF_PHYS + idx * BUFSZ, wire);
    s_wr8(0, R8169_TPPOLL, R8169_TPPOLL_NPQ);
    return idx;
}

/* Drain the RX ring like r8169_poll(); returns frames delivered. */
static uint32_t got_len[4096];
static uint8_t got_tag[4096];
static int ngot, ndrop;
static int poll_rx(int max) {
    int n = 0;
    for (int k = 0; k < max; k++) {
        uint32_t idx, len;
        int r = r8169_rx_next(&rxq, rx_ring, &idx, &len);
        if (r == R8169_RX_NONE)
            break;
        if (r == R8169_RX_FRAME) {
            if (ngot < 4096) {
                got_len[ngot] = len;
                got_tag[ngot] = rx_bufs[idx][0];
            }
            ngot++;
            n++;
        } else {
            ndrop++;
        }
        r8169_rx_recycle(&rxq, rx_ring, idx);
    }
    return n;
}

/* ── Tests ───────────────────────────────────────────────────────────────── */
static void test_chip_table(void) {
    struct { uint32_t txcfg; const char *name; uint32_t want, nowant; } t[] = {
        { 0x00000000U, "RTL8169",          R8169_F_MACRESET, R8169_F_PCIE },
        { 0x98000000U, "RTL8110SCe",       R8169_F_8110SCE, R8169_F_PCIE },
        { 0x18000000U, "RTL8169SC/8110SC", R8169_F_8110SC, R8169_F_8110SCE },
        { 0x7CC00000U, "RTL8169SBL",       R8169_F_MACRESET, 0 },
        { 0x38000000U, "RTL8168B/8111B",   R8169_F_MACSTAT, R8169_F_CMDSTOP },
        { 0x3C000000U, "RTL8168C/8111C",   R8169_F_CMDSTOP, R8169_F_MACSLEEP },
        { 0x3C200000U, "RTL8168C/8111C",   R8169_F_MACSLEEP, 0 },
        { 0x28100000U, "RTL8168D/8111D",   R8169_F_PHYWAKE_PM, R8169_F_GPLUS },
        { 0x2C100000U, "RTL8168E/8111E",   R8169_F_PHYWAKE_PM | R8169_F_CMDSTOP,
                                           R8169_F_WAIT_TXQ | R8169_F_EARLYOFF },
        { 0x2C800000U, "RTL8168E-VL/8111E-VL", R8169_F_EARLYOFF | R8169_F_WAIT_TXQ,
                                           R8169_F_GPLUS },
        { 0x48100000U, "RTL8168F/8111F",   R8169_F_EARLYOFF | R8169_F_MCBUG,
                                           R8169_F_GPLUS },
        { 0x4C000000U, "RTL8168G/8111G",   R8169_F_GPLUS, R8169_F_EARLYOFF },
        { 0x4C100000U, "RTL8168G/8111G",   R8169_F_GPLUS, R8169_F_FASTETHER },
        { 0x50900000U, "RTL8168GU/8111GU", R8169_F_GPLUS, 0 },
        { 0x54000000U, "RTL8168H/8111H",   R8169_F_GPLUS, R8169_F_FASTETHER },
        { 0x54100000U, "RTL8168H/8111H",   R8169_F_GPLUS, 0 },
        { 0x50000000U, "RTL8168EP/8111EP", R8169_F_GPLUS, 0 },
        { 0x5C800000U, "RTL8411B",         R8169_F_GPLUS, 0 },
        { 0x44800000U, "RTL8106E",         R8169_F_FASTETHER | R8169_F_PHYWAKE_PM, 0 },
        { 0x34000000U, "RTL8101E",         R8169_F_FASTETHER, R8169_F_MACSTAT },
        { 0x24000000U, "RTL8401E",         R8169_F_8401E, 0 },
        /* TxConfig's non-XID bits (IFG, DMA, loopback) do not matter. */
        { 0x54100000U | 0x03000700U, "RTL8168H/8111H", R8169_F_GPLUS, 0 },
    };
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        const struct r8169_chip *c = r8169_chip_lookup(t[i].txcfg);
        CHECK(c && !strcmp(c->name, t[i].name), "0x%08x -> %s, want %s",
              t[i].txcfg, c ? c->name : "(none)", t[i].name);
        if (!c)
            continue;
        uint32_t f = r8169_chip_flags(c, t[i].txcfg, 0x8168);
        CHECK((f & t[i].want) == t[i].want, "0x%08x lacks flags 0x%x",
              t[i].txcfg, t[i].want & ~f);
        CHECK(!(f & t[i].nowant), "0x%08x has flags 0x%x", t[i].txcfg,
              f & t[i].nowant);
    }
    /* 8139 family, the 8125 (2.5G), garbage and all-ones are not ours. */
    uint32_t bad[] = { 0x74800000U, 0x60000000U, 0x64000000U, 0x60800000U,
                       0xFFFFFFFFU, 0x7C800000U };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(!r8169_chip_lookup(bad[i]), "0x%08x matched %s", bad[i],
              r8169_chip_lookup(bad[i])->name);
    /* 8168H silicon on an 8136 card is 10/100. */
    const struct r8169_chip *h = r8169_chip_lookup(0x54100000U);
    CHECK(h && (r8169_chip_flags(h, 0x54100000U, 0x8136) & R8169_F_FASTETHER),
          "8107E not fast Ethernet");
    /* Every table entry is found by its own XID (no shadowing). */
    for (unsigned i = 0; i < sizeof(r8169_chips) / sizeof(r8169_chips[0]); i++)
        CHECK(r8169_chip_lookup(r8169_chips[i].xid) == &r8169_chips[i],
              "entry %u (%s) shadowed", i, r8169_chips[i].name);
}

static void test_start_pre_g(void) {
    uint32_t f;
    attach(0x2C100000U, 0x8168, &f);                   /* 8168E */
    CHECK(C.resets == 1, "%d resets", C.resets);
    CHECK(C.log[0].reg == R8169_CPCMD, "CPlusCmd not first (0x%x)", C.log[0].reg);
    CHECK(C.log[0].val == (R8169_CPCMD_PCI_MRW | R8169_CPCMD_MACSTAT_DIS | 1),
          "8168E CPlusCmd 0x%x", C.log[0].val);
    int rd = find_write(R8169_RDSAR_LO, ~0U, RX_RING_PHYS, 0);
    int td = find_write(R8169_TNPDS_LO, ~0U, TX_RING_PHYS, 0);
    int en = find_write(R8169_CMD, 0xFF, R8169_CMD_TE | R8169_CMD_RE, 0);
    int tc = find_write(R8169_TXCFG, ~0U, R8169_TXCFG_IFG96 | R8169_TXCFG_DMA_2048, 0);
    int rc = last_write(R8169_RXCFG);
    CHECK(rd >= 0 && td >= 0 && en > rd && en > td, "rings after enable");
    CHECK(tc > en && rc > en, "8168E: TxConfig/RxConfig before TE|RE");
    CHECK(find_write(R8169_MISC, 0, 0, 0) < 0, "8168E touched MISC");
    uint32_t rx = get32(R8169_RXCFG);
    CHECK((rx & R8169_RXCFG_ACCEPT) == (R8169_RXCFG_APM | R8169_RXCFG_AB | R8169_RXCFG_AM),
          "accept bits 0x%x", rx);
    CHECK((rx & 0xE700U) == 0xE700U, "FIFO/DMA bits 0x%x", rx);
    CHECK(get32(R8169_MAR0) == 0xFFFFFFFFU && get32(R8169_MAR4) == 0xFFFFFFFFU,
          "multicast not open");
    uint16_t imr; memcpy(&imr, &C.r[R8169_IMR], 2);
    CHECK(imr == R8169_INT_MASK, "IMR 0x%x", imr);
    CHECK(last_write(R8169_IMR) > last_write(R8169_RXCFG), "IRQs before RX on");
    uint16_t rms; memcpy(&rms, &C.r[R8169_RMS], 2);
    CHECK(rms == BUFSZ, "RMS %u", rms);
    CHECK(C.r[R8169_PMCH] & 0x80, "8168E PHY power (PMCH) not on");
}

static void test_start_g_plus(void) {
    uint32_t f;
    /* 8168H with the RXDV gate closed by firmware, as after a PXE boot. */
    chip_reset_state(0);
    attach(0x54100000U, 0x8168, &f);
    int gate_open = find_write(R8169_MISC, R8169_MISC_RXDV_GATE, 0, 0);
    int en = find_write(R8169_CMD, 0xFF, R8169_CMD_TE | R8169_CMD_RE, 0);
    int tc = last_write(R8169_TXCFG), rc = last_write(R8169_RXCFG);
    CHECK(gate_open >= 0, "8168H: RXDV gate never opened");
    CHECK(en > tc && en > rc, "8168H: TE|RE before TxConfig/RxConfig");
    CHECK(en > gate_open, "8168H: TE|RE before the RXDV gate opened");
    CHECK(!(get32(R8169_MISC) & R8169_MISC_RXDV_GATE), "gate left closed");
    CHECK(find_write(R8169_CMD, R8169_CMD_STOPREQ, R8169_CMD_STOPREQ, 0) < 0,
          "STOPREQ on an 8168G+");
    uint32_t rx = get32(R8169_RXCFG);
    CHECK(rx & R8169_RXCFG_EARLYOFFV2, "8168H RxConfig 0x%x lacks early-off v2", rx);
    CHECK(C.r[R8169_CMD] == (R8169_CMD_TE | R8169_CMD_RE), "MAC not enabled");

    /* Stop: accept bits off, gate closed, TE/RE cleared, IRQs masked. */
    C.nlog = 0;
    r8169_hw_stop(&sim, f);
    CHECK(!(get32(R8169_RXCFG) & R8169_RXCFG_ACCEPT), "stop left RX accepting");
    CHECK(get32(R8169_MISC) & R8169_MISC_RXDV_GATE, "stop left gate open");
    CHECK(!(C.r[R8169_CMD] & (R8169_CMD_TE | R8169_CMD_RE)), "stop left MAC on");
    int g = find_write(R8169_MISC, R8169_MISC_RXDV_GATE, R8169_MISC_RXDV_GATE, 0);
    int off = find_write(R8169_CMD, 0xFF, 0, 0);
    CHECK(g >= 0 && off > g, "8168H: TE/RE cleared before the gate closed");
    uint16_t imr; memcpy(&imr, &C.r[R8169_IMR], 2);
    CHECK(imr == 0, "stop left IMR 0x%x", imr);
    /* A frame arriving while gated is not DMA'd. */
    C.r[R8169_CMD] |= R8169_CMD_RE;
    CHECK(chip_rx_frame(100, 1, 0) < 0, "frame DMA'd through a closed gate");
}

static void test_start_8169_and_evl(void) {
    uint32_t f;
    attach(0x00000000U, 0x8169, &f);
    CHECK(C.log[0].val == (R8169_CPCMD_PCI_MRW | R8169_CPCMD_RXENB | R8169_CPCMD_TXENB),
          "8169 CPlusCmd 0x%x", C.log[0].val);
    CHECK(C.r[R8169_MACRESET] == 1, "8169: 0x82 not set after reset");
    attach(0x2C800000U, 0x8168, &f);
    CHECK((get32(R8169_RXCFG) & R8169_RXCFG_EARLYOFF) == R8169_RXCFG_EARLYOFF,
          "8168E-VL RxConfig 0x%x lacks early-off", get32(R8169_RXCFG));
    attach(0x18000000U, 0x8167, &f);
    CHECK(get32(R8169_8110SC_7C) == 0x000FFF00U, "8110SC 0x7C = 0x%x",
          get32(R8169_8110SC_7C));
    /* Config2 MSI enable cleared at wake, and Cfg9346 locked again. */
    chip_reset_state(0x4C000000U);
    C.r[R8169_CFG2] = R8169_CFG2_MSI | 0x01;
    r8169_hw_wake(&sim, R8169_F_8168G);
    CHECK(C.r[R8169_CFG2] == 0x01, "CFG2 0x%x", C.r[R8169_CFG2]);
    CHECK(C.r[R8169_EECMD] == R8169_EECMD_OFF, "Cfg9346 left unlocked");
}

static void test_phy(void) {
    chip_reset_state(0x54100000U);
    CHECK(r8169_mdio_read(&sim, MII_BMSR) == 0x7949, "BMSR read");
    CHECK(r8169_phy_init(&sim, R8169_F_8168G) == 0, "phy init failed");
    CHECK((C.phy[MII_ANAR] & 0x0DE1) == 0x0DE1, "ANAR 0x%x", C.phy[MII_ANAR]);
    CHECK((C.phy[MII_GTCR] & (GTCR_1000FD | GTCR_1000HD)) ==
          (GTCR_1000FD | GTCR_1000HD), "GTCR 0x%x", C.phy[MII_GTCR]);
    CHECK(C.phy[MII_BMCR] == (BMCR_ANEN | BMCR_ANRESTART), "BMCR 0x%x",
          C.phy[MII_BMCR]);
    int rst = find_write(R8169_PHYAR, 0x801FFFFFU,
                         R8169_PHYAR_FLAG | (MII_BMCR << 16) | BMCR_RESET, 0);
    int an = find_write(R8169_PHYAR, 0x801FFFFFU, R8169_PHYAR_FLAG | (MII_BMCR << 16) |
                        BMCR_ANEN | BMCR_ANRESTART, 0);
    CHECK(rst >= 0 && an > rst, "BMCR reset/restart order");

    chip_reset_state(0x44800000U);                     /* 8106E */
    CHECK(r8169_phy_init(&sim, r8169_chip_lookup(0x44800000U)->flags) == 0, "8106E phy");
    CHECK(C.phy[MII_GTCR] == 0, "10/100 part advertised 1000: 0x%x", C.phy[MII_GTCR]);

    /* No PHY answering: fails, and in bounded time (no hang at boot). */
    chip_reset_state(0x54100000U);
    C.phy_present = 0;
    CHECK(r8169_phy_init(&sim, R8169_F_8168G) == -1, "absent PHY not reported");
    CHECK(C.delay_us < 1000000, "absent PHY took %llu us",
          (unsigned long long)C.delay_us);
}

static void test_tx(void) {
    uint32_t f;
    attach(0x54100000U, 0x8168, &f);
    for (int i = 0; i < NDESC; i++)
        CHECK(!!(tx_ring[i].opts1 & R8169_DESC_EOR) == (i == NDESC - 1),
              "TX EOR on %d", i);

    /* 1. Chip stalled: all NDESC slots usable (OWN delimits, no reserved
     * slot), then full; nothing reclaimed while the chip owns them. */
    C.tx_hold = 1;
    int posted = 0;
    while (posted <= NDESC && send(100, (uint8_t)posted) != (uint32_t)-1)
        posted++;
    CHECK(posted == NDESC, "posted %d into %d slots", posted, NDESC);
    CHECK(r8169_tx_reclaim(&txq, tx_ring) == 0, "reclaimed owned slots");

    /* 2. Stall detection: 200 ticks after first seen, not before. */
    CHECK(!r8169_tx_stalled(&txq, tx_ring, 5000), "stalled at first sight");
    CHECK(!r8169_tx_stalled(&txq, tx_ring, 5199), "stalled after 199");
    CHECK(r8169_tx_stalled(&txq, tx_ring, 5200), "no stall after 200");

    /* 3. Chip resumes: everything goes out in order and is reclaimed. */
    C.tx_hold = 0;
    chip_tx_run();
    CHECK(C.sent == (uint32_t)NDESC, "chip sent %u", C.sent);
    for (int i = 0; i < NDESC; i++)
        CHECK(C.sent_first[i] == (uint8_t)i, "frame %d out of order", i);
    CHECK(r8169_tx_reclaim(&txq, tx_ring) == NDESC && txq.pending == 0, "reclaim");
    CHECK(!r8169_tx_stalled(&txq, tx_ring, 9999) && txq.stall_tick == 0,
          "idle ring stalled");

    /* 4. Wraparound: 10 rings' worth, lengths varying, padding to 60. */
    C.sent = 0;
    for (int i = 0; i < NDESC * 10; i++) {
        uint32_t len = 14 + (uint32_t)(i * 37) % 1500;
        CHECK(send(len, (uint8_t)(i * 7)) != (uint32_t)-1, "send %d failed", i);
        CHECK(C.sent_len[i] == (len < 60 ? 60 : len), "frame %d len %u for %u",
              i, C.sent_len[i], len);
        CHECK(C.sent_first[i] == (uint8_t)(i * 7), "frame %d content", i);
    }
    CHECK(C.sent == (uint32_t)NDESC * 10, "sent %u", C.sent);
    CHECK(C.eor_violation == 0, "EOR misplaced %d times", C.eor_violation);
    for (int i = 0; i < NDESC; i++)
        CHECK(!!(tx_ring[i].opts1 & R8169_DESC_EOR) == (i == NDESC - 1),
              "TX EOR lost on %d", i);

    /* 5. Partial completion: reclaim stops at the first owned slot even if
     * a later one reads done. */
    r8169_tx_reclaim(&txq, tx_ring);
    C.tx_hold = 1;
    uint32_t a = send(70, 1), b = send(70, 2);
    tx_ring[b].opts1 &= ~R8169_DESC_OWN;   /* out of order: must not count */
    CHECK(r8169_tx_reclaim(&txq, tx_ring) == 0, "reclaimed past an owned slot");
    tx_ring[a].opts1 &= ~R8169_DESC_OWN;
    CHECK(r8169_tx_reclaim(&txq, tx_ring) == 2, "in-order reclaim");
    C.tx_hold = 0;
    C.tx_idx = txq.next;

    /* 6. A steady upload whose send reclaims completions before the
     * watchdog looks: no false stall. */
    int bogus = 0;
    for (uint32_t t = 1; t <= 1000; t++) {
        send(1514, 3);
        if (r8169_tx_stalled(&txq, tx_ring, t))
            bogus++;
    }
    CHECK(bogus == 0, "%d bogus stalls", bogus);

    /* 7. Tick 0 and wrap still arm/fire the timer. */
    C.tx_hold = 1;
    r8169_tx_init(&txq, tx_ring, NDESC);
    C.tx_idx = 0;
    send(60, 0);
    CHECK(!r8169_tx_stalled(&txq, tx_ring, 0xFFFFFF80U), "stalled before wrap");
    CHECK(r8169_tx_stalled(&txq, tx_ring, 0x48U), "no stall across wrap");
    C.tx_hold = 0;
}

static void test_rx(void) {
    uint32_t f;
    attach(0x4C000000U, 0x8168, &f);
    for (int i = 0; i < NDESC; i++) {
        CHECK(rx_ring[i].opts1 == (R8169_DESC_OWN | BUFSZ |
                                   (i == NDESC - 1 ? R8169_DESC_EOR : 0)),
              "RX desc %d armed as 0x%08x", i, rx_ring[i].opts1);
        CHECK(rx_ring[i].addr_lo == RX_BUF_PHYS + (uint32_t)i * BUFSZ &&
              rx_ring[i].addr_hi == 0, "RX desc %d address", i);
    }

    /* 1. Nothing received: nothing returned. */
    ngot = ndrop = 0;
    CHECK(poll_rx(100) == 0, "frames from an idle ring");

    /* 2. Wraparound: 20 rings' worth in bursts of 5, delivered in order,
     * CRC stripped, buffers recycled. */
    int sent = 0;
    for (int burst = 0; burst < NDESC * 4; burst++) {
        for (int k = 0; k < 5; k++, sent++)
            CHECK(chip_rx_frame(64 + (uint32_t)(sent % 1455), (uint8_t)sent, 0) == 0,
                  "frame %d: RDU with a drained ring", sent);
        poll_rx(1000);
    }
    CHECK(ngot == sent && ndrop == 0, "got %d of %d (%d dropped)", ngot, sent, ndrop);
    for (int i = 0; i < sent && i < 4096; i++) {
        CHECK(got_len[i] == 60 + (uint32_t)(i % 1455), "frame %d len %u", i, got_len[i]);
        CHECK(got_tag[i] == (uint8_t)i, "frame %d out of order", i);
    }
    for (int i = 0; i < NDESC; i++)
        CHECK(rx_ring[i].opts2 == 0 && (rx_ring[i].opts1 & R8169_DESC_OWN),
              "desc %d not re-armed cleanly", i);
    CHECK(C.eor_violation == 0, "RX EOR misplaced");

    /* 3. Ring full (the driver is slow): exactly NDESC frames land, the
     * next one is RDU; after a drain reception resumes in order. */
    ngot = 0;
    for (int i = 0; i < NDESC; i++)
        CHECK(chip_rx_frame(100, (uint8_t)(0x40 + i), 0) == 0, "fill %d", i);
    C.r[R8169_ISR] = 0;
    CHECK(chip_rx_frame(100, 0xEE, 0) < 0, "frame into a full ring");
    CHECK(C.r[R8169_ISR] & R8169_INT_RDU, "no RDU");
    CHECK(poll_rx(1000) == NDESC, "drain after RDU got %d", ngot);
    CHECK(got_tag[0] == 0x40 && got_tag[NDESC - 1] == (uint8_t)(0x40 + NDESC - 1),
          "order after RDU");
    CHECK(chip_rx_frame(100, 0x77, 0) == 0 && poll_rx(10) == 1 &&
          got_tag[NDESC] == 0x77, "no resume after RDU");

    /* 4. Partially drained: the poll budget stops mid-ring and the next
     * poll continues where it left off. */
    ngot = 0;
    for (int i = 0; i < 10; i++)
        chip_rx_frame(200, (uint8_t)(0x10 + i), 0);
    CHECK(poll_rx(3) == 3 && poll_rx(100) == 7, "budgeted polls");
    CHECK(got_tag[3] == 0x13 && got_tag[9] == 0x19, "budgeted order");

    /* 5. Errors: CRC/RES dropped; a frame spread over 3 descriptors dropped
     * whole; the good frames around them still arrive. */
    ngot = ndrop = 0;
    chip_rx_frame(300, 0xA1, 0);
    chip_rx_frame(300, 0xA2, 1);                       /* CRC error */
    chip_rx_frame(BUFSZ * 2 + 100, 0xA3, 0);           /* 3 pieces */
    chip_rx_frame(300, 0xA4, 0);
    poll_rx(100);
    CHECK(ngot == 2 && got_tag[0] == 0xA1 && got_tag[1] == 0xA4,
          "errors: got %d frames", ngot);
    CHECK(ndrop == 4, "dropped %d descriptors, want 4", ndrop);
    CHECK(rxq.fragments == 3 && rxq.errors >= 1, "fragments %u errors %u",
          rxq.fragments, rxq.errors);

    /* 6. Runt / impossible lengths from a confused chip are dropped. */
    ngot = ndrop = 0;
    uint32_t i0 = rxq.next;
    rx_ring[i0].opts1 = R8169_DESC_FS | R8169_DESC_LS | 10 |
                        (i0 == NDESC - 1 ? R8169_DESC_EOR : 0);
    uint32_t i1 = (i0 + 1) % NDESC;
    rx_ring[i1].opts1 = R8169_DESC_FS | R8169_DESC_LS | (BUFSZ + 8) |
                        (i1 == NDESC - 1 ? R8169_DESC_EOR : 0);
    poll_rx(2);
    CHECK(ngot == 0 && ndrop == 2, "bad lengths: %d delivered", ngot);
    C.rx_idx = rxq.next;

    /* 7. A reset re-arms the whole ring and the chip starts at slot 0. */
    r8169_hw_stop(&sim, f);
    r8169_hw_reset(&sim, f);
    uint32_t phys[NDESC];
    for (int i = 0; i < NDESC; i++)
        phys[i] = RX_BUF_PHYS + (uint32_t)i * BUFSZ;
    r8169_rx_init(&rxq, rx_ring, NDESC, phys, BUFSZ);
    r8169_hw_start(&sim, f, RX_RING_PHYS, TX_RING_PHYS, BUFSZ);
    ngot = 0;
    CHECK(chip_rx_frame(100, 0x55, 0) == 0 && poll_rx(10) == 1 &&
          got_tag[0] == 0x55, "RX after reset");
}

int main(void) {
    CHECK(sizeof(struct r8169_desc) == 16, "descriptor size %zu",
          sizeof(struct r8169_desc));
    CHECK(((uintptr_t)rx_ring & 255) == 0 && ((uintptr_t)tx_ring & 255) == 0,
          "ring alignment");
    test_chip_table();
    test_start_pre_g();
    test_start_g_plus();
    test_start_8169_and_evl();
    test_phy();
    test_tx();
    test_rx();
    if (fails) {
        printf("test_r8169: %d failure(s)\n", fails);
        return 1;
    }
    printf("test_r8169: ok\n");
    return 0;
}
