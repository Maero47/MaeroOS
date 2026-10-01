#include "ahci.h"
#include "ahci_handoff.h"
#include "pci.h"
#include "../kernel/printk.h"
#include "../lib/string.h"
#include "../arch/i686/mm/paging.h"
#include "../mm/mmio.h"
#include "../arch/i686/cpu/spinlock.h"
#include "../arch/i686/cpu/tsc.h"
#include <kernel/config.h>
#include <io.h>
#include <stdint.h>

/*
 * AHCI 1.3.1 host bus adapter driver (Serial ATA AHCI specification, Intel,
 * rev 1.3.1; register and structure names below follow it).
 *
 * The HBA is a bus master that walks, per port, a command list of 32 command
 * headers; each header points at a command table holding the command FIS
 * (a Host-to-Device Register FIS) and a physical region descriptor table
 * (PRDT) saying where the data goes.  The host builds slot 0, sets bit 0 of
 * PxCI and the HBA clears it once the device has answered; errors show up in
 * PxIS (task-file error, host bus errors) and PxTFD (the ATA status/error).
 *
 * This driver uses that machinery the simplest way that is still correct:
 *   - one command at a time (slot 0, no NCQ), polled for completion with
 *     interrupts off, exactly like the IDE driver's DMA path (drivers/ata.c):
 *     a transaction cannot be preempted, and the BKL plus `ahci_lock` keep
 *     another CPU out.  GHC.IE stays clear, so the HBA never raises an IRQ;
 *   - data moves through a physically contiguous bounce buffer in .bss, big
 *     enough for the largest request (256 sectors), so one PRD entry covers
 *     any transfer and callers may pass heap buffers that are not contiguous;
 *   - the command list, received-FIS area and command table of each port live
 *     in .bss too.  The kernel image is physically contiguous and below 4 GiB,
 *     so kphys() is a subtraction and the upper address registers are 0.
 *
 * Error recovery follows section 6.2.2.2 of the spec ("non-queued error
 * recovery"): stop the port (PxCMD.ST), clear PxSERR/PxIS, and if the device
 * is still busy use command list override (PxCMD.CLO) or, failing that, a
 * COMRESET through PxSCTL before starting the port again.  The failed request
 * is retried once after a recovery.
 */

/* ── HBA (generic host control) registers, offsets from ABAR ───────────── */
#define HBA_CAP     0x00
#define HBA_GHC     0x04
#define HBA_IS      0x08
#define HBA_PI      0x0C
#define HBA_VS      0x10
#define HBA_CAP2    0x24
#define HBA_BOHC    0x28

#define CAP_NP_MASK   0x1Fu         /* number of ports - 1 */
#define CAP_SCLO      (1u << 24)    /* supports command list override */
#define CAP_SSS       (1u << 27)    /* supports staggered spin-up */
#define CAP_S64A      (1u << 31)

#define GHC_HR        (1u << 0)
#define GHC_IE        (1u << 1)
#define GHC_AE        (1u << 31)

#define CAP2_BOH      (1u << 0)     /* BIOS/OS handoff supported */

/* ── Port registers, offsets from ABAR + 0x100 + port * 0x80 ───────────── */
#define PX_CLB      0x00
#define PX_CLBU     0x04
#define PX_FB       0x08
#define PX_FBU      0x0C
#define PX_IS       0x10
#define PX_IE       0x14
#define PX_CMD      0x18
#define PX_TFD      0x20
#define PX_SIG      0x24
#define PX_SSTS     0x28
#define PX_SCTL     0x2C
#define PX_SERR     0x30
#define PX_SACT     0x34
#define PX_CI       0x38

#define PXCMD_ST      (1u << 0)
#define PXCMD_SUD     (1u << 1)
#define PXCMD_POD     (1u << 2)
#define PXCMD_CLO     (1u << 3)
#define PXCMD_FRE     (1u << 4)
#define PXCMD_FR      (1u << 14)
#define PXCMD_CR      (1u << 15)

/* PxIS bits that end a command with an error. */
#define PXIS_IFS      (1u << 27)    /* interface fatal error */
#define PXIS_HBDS     (1u << 28)    /* host bus data error */
#define PXIS_HBFS     (1u << 29)    /* host bus fatal error */
#define PXIS_TFES     (1u << 30)    /* task file error (ATA status ERR) */
#define PXIS_ERRORS   (PXIS_IFS | PXIS_HBDS | PXIS_HBFS | PXIS_TFES)

#define TFD_ERR       0x01u
#define TFD_DRQ       0x08u
#define TFD_BSY       0x80u

#define SSTS_DET_MASK 0x0Fu
#define SSTS_DET_OK   0x03u         /* device present, phy communication up */

#define SIG_ATA       0x00000101u
#define SIG_ATAPI     0xEB140101u
#define SIG_SEMB      0xC33C0101u
#define SIG_PM        0x96690101u

/* ── ATA commands ──────────────────────────────────────────────────────── */
#define ATA_READ_DMA_EXT   0x25
#define ATA_WRITE_DMA_EXT  0x35
#define ATA_FLUSH_EXT      0xEA
#define ATA_IDENTIFY       0xEC

#define FIS_TYPE_H2D       0x27

/* ── In-memory structures (spec section 4.2) ───────────────────────────── */
typedef struct {
    uint32_t dw0;          /* CFL[4:0] A[5] W[6] P[7] R[8] B[9] C[10] PMP PRDTL[31:16] */
    uint32_t prdbc;        /* bytes transferred, written by the HBA */
    uint32_t ctba;         /* command table base, 128-byte aligned */
    uint32_t ctbau;
    uint32_t rsv[4];
} ahci_cmdhdr_t;

typedef struct {
    uint32_t dba;          /* data base, word aligned */
    uint32_t dbau;
    uint32_t rsv;
    uint32_t dbc;          /* byte count - 1 (bit 0 set: even count), I[31] */
} ahci_prd_t;

#define AHCI_PRDS 1
typedef struct {
    uint8_t    cfis[64];
    uint8_t    acmd[16];
    uint8_t    rsv[48];
    ahci_prd_t prdt[AHCI_PRDS];
} ahci_cmdtbl_t;

/* Everything one port needs, laid out to meet each piece's alignment: the
 * command list (1 KiB aligned), the received-FIS area (256), the command
 * table (128). */
typedef struct {
    ahci_cmdhdr_t cl[32];
    uint8_t       rfis[256];
    ahci_cmdtbl_t ct;
} __attribute__((aligned(1024))) ahci_port_mem_t;

_Static_assert(sizeof(ahci_cmdhdr_t) == 32, "command header is 32 bytes");
_Static_assert(sizeof(ahci_prd_t) == 16, "PRD entry is 16 bytes");
_Static_assert(__builtin_offsetof(ahci_port_mem_t, rfis) == 1024, "FIS area alignment");
_Static_assert(__builtin_offsetof(ahci_port_mem_t, ct) % 128 == 0, "command table alignment");

#define AHCI_BOUNCE_BYTES (256u * 512u)

static ahci_port_mem_t port_mem[AHCI_MAX_DISKS];
static uint8_t         ahci_bounce[AHCI_BOUNCE_BYTES] __attribute__((aligned(4096)));

typedef struct {
    volatile uint8_t *abar;        /* controller register window */
    volatile uint8_t *port;        /* this port's registers */
    ahci_port_mem_t  *mem;
    uint32_t          cap;         /* controller CAP, for CLO */
    uint64_t          sectors;
    uint8_t           ctrl, portno;
} ahci_disk_t;

static ahci_disk_t disks[AHCI_MAX_DISKS];
static int         ndisks;
static spinlock_t  ahci_lock;

/*
 * Each controller's ABAR (at most 0x1100 bytes: the HBA registers and 32
 * ports) is mapped uncached through mmio_map() during boot (see the kernel
 * virtual map in include/kernel/config.h).
 */
#define AHCI_ABAR_SIZE   0x1100u
#define AHCI_MAX_CTRL    4

static uint32_t kphys(const void *p) {
    return (uint32_t)(uintptr_t)p - (uint32_t)KERNEL_VMA;
}

static inline uint32_t rd(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}

static inline void wr(volatile uint8_t *base, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(base + off) = v;
}

static inline uint32_t ahci_irq_save(void) {
    uint32_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}

static inline void ahci_irq_restore(uint32_t f) {
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
}

/*
 * Wall-clock timeouts, as in drivers/ata.c: transactions run with interrupts
 * off, so the deadline is on the TSC once it is calibrated.  ahci_init runs
 * before that, and then each poll also writes the POST port 0x80, which takes
 * about a microsecond on real chipsets, so a budget of `ms * 1000` polls lasts
 * at least `ms` there (and passes quicker under emulation, where nothing
 * needs that long).
 */
typedef struct {
    uint64_t end;
    uint32_t spins, max_spins;
} ahci_timer_t;

static inline uint64_t ahci_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void timer_start(ahci_timer_t *t, uint32_t ms) {
    uint32_t cpt = tsc_cycles_per_tick();          /* cycles per 10 ms */
    t->spins = 0;
    if (cpt) {
        t->end = ahci_rdtsc() + (uint64_t)cpt * ms / 10u + 1u;
        t->max_spins = 0;
    } else {
        t->end = 0;
        t->max_spins = ms * 1000u;
    }
}

static int timer_expired(ahci_timer_t *t) {
    if (t->end)
        return ahci_rdtsc() >= t->end;
    outb(0x80, 0);
    return ++t->spins > t->max_spins;
}

static void delay_ms(uint32_t ms) {
    ahci_timer_t t;
    timer_start(&t, ms);
    while (!timer_expired(&t))
        __asm__ volatile("pause");
}

/* Wait until (reg & mask) == want; 0 on success, -1 on timeout. */
static int wait_reg(volatile uint8_t *base, uint32_t off, uint32_t mask,
                    uint32_t want, uint32_t ms) {
    ahci_timer_t t;
    timer_start(&t, ms);
    do {
        if ((rd(base, off) & mask) == want)
            return 0;
    } while (!timer_expired(&t));
    return (rd(base, off) & mask) == want ? 0 : -1;
}

/* ── Port control ──────────────────────────────────────────────────────── */

/* Stop command processing and FIS reception (spec 10.1.2). */
static int port_stop(volatile uint8_t *p) {
    uint32_t cmd = rd(p, PX_CMD);
    if (cmd & PXCMD_ST)
        wr(p, PX_CMD, cmd & ~PXCMD_ST);
    if (wait_reg(p, PX_CMD, PXCMD_CR, 0, 500) < 0)
        return -1;
    cmd = rd(p, PX_CMD);
    if (cmd & PXCMD_FRE)
        wr(p, PX_CMD, cmd & ~PXCMD_FRE);
    return wait_reg(p, PX_CMD, PXCMD_FR, 0, 500);
}

static void port_start(volatile uint8_t *p) {
    (void)wait_reg(p, PX_CMD, PXCMD_CR, 0, 500);
    wr(p, PX_CMD, rd(p, PX_CMD) | PXCMD_FRE);
    wr(p, PX_CMD, rd(p, PX_CMD) | PXCMD_ST);
}

/* COMRESET: hold PxSCTL.DET at 1 for at least 1 ms, release it and wait for
 * the link to come back.  The device then answers with a D2H Register FIS
 * that clears BSY.  0 when the link is up again. */
static int port_comreset(volatile uint8_t *p) {
    uint32_t sctl = rd(p, PX_SCTL);
    wr(p, PX_SCTL, (sctl & ~0xFu) | 1u);
    delay_ms(2);
    wr(p, PX_SCTL, sctl & ~0xFu);
    if (wait_reg(p, PX_SSTS, SSTS_DET_MASK, SSTS_DET_OK, 1000) < 0)
        return -1;
    wr(p, PX_SERR, 0xFFFFFFFFu);
    return 0;
}

/* Wait for the device to drop BSY and DRQ. */
static int port_wait_idle(volatile uint8_t *p, uint32_t ms) {
    return wait_reg(p, PX_TFD, TFD_BSY | TFD_DRQ, 0, ms);
}

/* Non-queued error recovery (spec 6.2.2.2).  0 when the port runs again. */
static int port_recover(ahci_disk_t *d) {
    volatile uint8_t *p = d->port;
    wr(p, PX_CMD, rd(p, PX_CMD) & ~PXCMD_ST);
    (void)wait_reg(p, PX_CMD, PXCMD_CR, 0, 500);
    wr(p, PX_SERR, 0xFFFFFFFFu);
    wr(p, PX_IS, 0xFFFFFFFFu);
    wr(d->abar, HBA_IS, 1u << d->portno);

    if (rd(p, PX_TFD) & (TFD_BSY | TFD_DRQ)) {
        int cleared = 0;
        if (d->cap & CAP_SCLO) {
            wr(p, PX_CMD, rd(p, PX_CMD) | PXCMD_CLO);
            cleared = wait_reg(p, PX_CMD, PXCMD_CLO, 0, 500) == 0 &&
                      !(rd(p, PX_TFD) & (TFD_BSY | TFD_DRQ));
        }
        if (!cleared) {
            (void)port_stop(p);
            if (port_comreset(p) < 0 || port_wait_idle(p, 5000) < 0)
                return -1;
            wr(p, PX_SERR, 0xFFFFFFFFu);
            wr(p, PX_IS, 0xFFFFFFFFu);
        }
    }
    port_start(p);
    return 0;
}

/*
 * Run one non-queued command in slot 0.  `bytes` (a multiple of 512, at most
 * the bounce buffer) move through ahci_bounce, towards the device when
 * `write`.  0 on success, -1 on a timeout or any error the HBA or the device
 * reported.  Interrupts are off and ahci_lock is held.
 */
static int port_exec(ahci_disk_t *d, uint8_t cmd, uint64_t lba, uint32_t nsect,
                     uint32_t bytes, int write, uint32_t timeout_ms) {
    volatile uint8_t *p = d->port;
    ahci_port_mem_t *m = d->mem;

    if (port_wait_idle(p, timeout_ms) < 0)
        return -1;

    ahci_cmdhdr_t *h = &m->cl[0];
    h->dw0   = 5u                              /* CFL: 5 dwords */
             | (write ? (1u << 6) : 0u)
             | ((bytes ? 1u : 0u) << 16);      /* PRDTL */
    h->prdbc = 0;
    h->ctba  = kphys(&m->ct);
    h->ctbau = 0;

    uint8_t *f = m->ct.cfis;
    memset(f, 0, 20);
    f[0]  = FIS_TYPE_H2D;
    f[1]  = 0x80;                              /* C: this is a command */
    f[2]  = cmd;
    f[4]  = (uint8_t)lba;
    f[5]  = (uint8_t)(lba >> 8);
    f[6]  = (uint8_t)(lba >> 16);
    f[7]  = 0x40;                              /* device: LBA mode */
    f[8]  = (uint8_t)(lba >> 24);
    f[9]  = (uint8_t)(lba >> 32);
    f[10] = (uint8_t)(lba >> 40);
    f[12] = (uint8_t)nsect;
    f[13] = (uint8_t)(nsect >> 8);

    if (bytes) {
        m->ct.prdt[0].dba  = kphys(ahci_bounce);
        m->ct.prdt[0].dbau = 0;
        m->ct.prdt[0].rsv  = 0;
        m->ct.prdt[0].dbc  = bytes - 1u;
    }

    wr(p, PX_IS, 0xFFFFFFFFu);
    __asm__ volatile("" ::: "memory");         /* structures before CI */
    wr(p, PX_CI, 1u);

    ahci_timer_t t;
    timer_start(&t, timeout_ms);
    int rc = -1;
    for (;;) {
        uint32_t is = rd(p, PX_IS);
        if (is & PXIS_ERRORS)
            break;
        if (!(rd(p, PX_CI) & 1u)) {
            rc = 0;
            break;
        }
        if (timer_expired(&t))
            break;
    }
    uint32_t is  = rd(p, PX_IS);
    uint32_t tfd = rd(p, PX_TFD);
    wr(p, PX_IS, is);
    wr(d->abar, HBA_IS, 1u << d->portno);
    if (rc == 0 && ((is & PXIS_ERRORS) || (tfd & TFD_ERR)))
        rc = -1;
    if (rc == 0 && bytes && h->prdbc != bytes)
        rc = -1;
    if (rc < 0) {
        printk("[AHCI] port %u: command 0x%02x LBA %u failed (IS=0x%08x TFD=0x%04x "
               "SERR=0x%08x)\n", (unsigned)d->portno, (unsigned)cmd, (unsigned)lba,
               (unsigned)is, (unsigned)(tfd & 0xFFFF), (unsigned)rd(p, PX_SERR));
        if (port_recover(d) < 0)
            printk("[AHCI] port %u: recovery failed\n", (unsigned)d->portno);
    }
    return rc;
}

/* ── Bring-up ──────────────────────────────────────────────────────────── */

static void ident_string(const uint16_t *id, int first, int words, char *out) {
    int n = 0;
    for (int i = 0; i < words; i++) {
        out[n++] = (char)(id[first + i] >> 8);
        out[n++] = (char)(id[first + i] & 0xFF);
    }
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = '\0';
}

/* Bring up one port with something attached and register an ATA disk on it. */
static void port_probe(volatile uint8_t *abar, uint32_t cap, int ctrl, int portno) {
    volatile uint8_t *p = abar + 0x100u + (uint32_t)portno * 0x80u;

    if (ndisks >= AHCI_MAX_DISKS) {
        printk("[AHCI] port %d: disk table full, ignoring\n", portno);
        return;
    }
    if (port_stop(p) < 0) {
        printk("[AHCI] port %d: does not stop, ignoring\n", portno);
        return;
    }

    ahci_disk_t *d = &disks[ndisks];
    ahci_port_mem_t *m = &port_mem[ndisks];
    memset(m, 0, sizeof(*m));
    d->abar = abar;
    d->port = p;
    d->mem = m;
    d->cap = cap;
    d->ctrl = (uint8_t)ctrl;
    d->portno = (uint8_t)portno;

    wr(p, PX_CLB, kphys(m->cl));
    wr(p, PX_CLBU, 0);
    wr(p, PX_FB, kphys(m->rfis));
    wr(p, PX_FBU, 0);
    wr(p, PX_IE, 0);
    wr(p, PX_SERR, 0xFFFFFFFFu);
    wr(p, PX_IS, 0xFFFFFFFFu);
    wr(p, PX_CMD, rd(p, PX_CMD) | PXCMD_FRE);

    /* A disk spinning up keeps BSY for seconds; a link that is up but whose
     * device never reports ready gets one COMRESET. */
    if (port_wait_idle(p, 10000) < 0 &&
        (port_comreset(p) < 0 || port_wait_idle(p, 10000) < 0)) {
        printk("[AHCI] port %d: device stays busy (TFD=0x%02x), ignoring\n",
               portno, (unsigned)(rd(p, PX_TFD) & 0xFF));
        (void)port_stop(p);
        return;
    }

    uint32_t sig = rd(p, PX_SIG);
    if (sig == SIG_ATAPI || sig == SIG_SEMB || sig == SIG_PM) {
        printk("[AHCI] port %d: %s device, not supported\n", portno,
               sig == SIG_ATAPI ? "ATAPI" : sig == SIG_PM ? "port multiplier" : "SEMB");
        (void)port_stop(p);
        return;
    }

    wr(p, PX_SERR, 0xFFFFFFFFu);
    wr(p, PX_IS, 0xFFFFFFFFu);
    port_start(p);

    if (port_exec(d, ATA_IDENTIFY, 0, 0, 512, 0, 5000) < 0) {
        printk("[AHCI] port %d: IDENTIFY failed (signature 0x%08x)\n",
               portno, (unsigned)sig);
        (void)port_stop(p);
        return;
    }
    uint16_t id[256];
    memcpy(id, ahci_bounce, sizeof(id));

    if (!(id[49] & (1u << 8)) || !(id[49] & (1u << 9))) {
        printk("[AHCI] port %d: disk lacks LBA/DMA, ignoring\n", portno);
        (void)port_stop(p);
        return;
    }
    uint64_t sectors;
    if (id[83] & (1u << 10))
        sectors = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                  ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    else
        sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    /* Word 106 valid (bits 15:14 = 01) with bit 12: logical sectors are
     * longer than 256 words; words 117-118 say how long. */
    if ((id[106] & 0xC000u) == 0x4000u && (id[106] & (1u << 12))) {
        uint32_t words = (uint32_t)id[117] | ((uint32_t)id[118] << 16);
        if (words != 256) {
            printk("[AHCI] port %d: %u-byte logical sectors, not supported\n",
                   portno, (unsigned)(words * 2u));
            (void)port_stop(p);
            return;
        }
    }
    if (!sectors) {
        printk("[AHCI] port %d: disk reports no capacity, ignoring\n", portno);
        (void)port_stop(p);
        return;
    }
    d->sectors = sectors;

    char model[41];
    ident_string(id, 27, 20, model);
    printk("[AHCI] disk %d: controller %d port %d, \"%s\", %u MiB%s\n",
           ndisks, ctrl, portno, model, (unsigned)(sectors >> 11),
           (id[83] & (1u << 10)) ? ", LBA48" : "");
    ndisks++;
}

/* BOHC through the register window, for ahci_bios_handoff(). */
typedef struct {
    ahci_bohc_io_t    io;
    volatile uint8_t *abar;
} ahci_bohc_hw_t;

static uint32_t bohc_read(ahci_bohc_io_t *io) {
    return rd(((ahci_bohc_hw_t *)io)->abar, HBA_BOHC);
}

static void bohc_write(ahci_bohc_io_t *io, uint32_t v) {
    wr(((ahci_bohc_hw_t *)io)->abar, HBA_BOHC, v);
}

static void bohc_sleep(ahci_bohc_io_t *io, uint32_t ms) {
    (void)io;
    delay_ms(ms);
}

/* Take the controller from the firmware (spec 10.6.3, drivers/ahci_handoff.h),
 * reset it and enable AHCI mode.  0 on success; on failure the reason is
 * logged and the controller is left alone.  A controller whose BIOS never
 * releases BOS is not reset: resetting it under a firmware that is still
 * driving it is what the handoff exists to prevent. */
static int hba_reset(volatile uint8_t *abar) {
    if (rd(abar, HBA_CAP2) & CAP2_BOH) {
        ahci_bohc_hw_t hw = { { bohc_read, bohc_write, bohc_sleep }, abar };
        int busy = 0;
        if (ahci_bios_handoff(&hw.io, &busy) < 0) {
            printk("[AHCI] BIOS did not release the HBA (BOHC=0x%08x%s); "
                   "leaving it alone\n", (unsigned)rd(abar, HBA_BOHC),
                   busy ? ", BIOS busy" : "");
            return -1;
        }
        printk("[AHCI] BIOS/OS handoff done%s\n", busy ? " (waited for busy BIOS)" : "");
    }
    wr(abar, HBA_GHC, rd(abar, HBA_GHC) | GHC_AE);
    wr(abar, HBA_GHC, rd(abar, HBA_GHC) | GHC_HR);
    if (wait_reg(abar, HBA_GHC, GHC_HR, 0, 1000) < 0) {
        printk("[AHCI] HBA reset timed out\n");
        return -1;
    }
    wr(abar, HBA_GHC, (rd(abar, HBA_GHC) | GHC_AE) & ~GHC_IE);
    return 0;
}

static void ahci_init_ctrl(const pci_device_t *pd, int ctrl) {
    uint32_t bar5 = pd->bar[5];
    if ((bar5 & 1u) || !(bar5 & ~0xFu)) {
        printk("[AHCI] %02x:%02x.%u: no memory BAR5, skipping\n",
               (unsigned)pd->bus, (unsigned)pd->slot, (unsigned)pd->func);
        return;
    }
    uint32_t phys = bar5 & ~0xFu;

    uint32_t cmd = pci_read_config32(pd->bus, pd->slot, pd->func, 0x04);
    /* Memory space + bus master; INTx disabled (the driver polls). */
    pci_write_config32(pd->bus, pd->slot, pd->func, 0x04,
                       (cmd & 0xFFFFu) | 0x0006u | 0x0400u);

    volatile uint8_t *abar = mmio_map(phys, AHCI_ABAR_SIZE);
    if (!abar) {
        printk("[AHCI] cannot map the register window\n");
        return;
    }

    uint32_t vs = rd(abar, HBA_VS);
    if (hba_reset(abar) < 0) {
        printk("[AHCI] %02x:%02x.%u: controller skipped\n",
               (unsigned)pd->bus, (unsigned)pd->slot, (unsigned)pd->func);
        return;
    }
    uint32_t cap = rd(abar, HBA_CAP);
    uint32_t pi  = rd(abar, HBA_PI);
    if (!pi)                                   /* PI must be set; be lenient */
        pi = (uint32_t)((1ull << ((cap & CAP_NP_MASK) + 1u)) - 1u);

    printk("[AHCI] controller %d at %02x:%02x.%u (%04x:%04x), AHCI %u.%u%s, "
           "%u ports, %u slots, PI=0x%08x\n",
           ctrl, (unsigned)pd->bus, (unsigned)pd->slot, (unsigned)pd->func,
           (unsigned)pd->vendor_id, (unsigned)pd->device_id,
           (unsigned)(vs >> 16), (unsigned)((vs >> 8) & 0xFF),
           (cap & CAP_S64A) ? ", 64-bit" : "",
           (unsigned)((cap & CAP_NP_MASK) + 1u), (unsigned)(((cap >> 8) & 0x1Fu) + 1u),
           (unsigned)pi);

    /* With staggered spin-up the ports wait for PxCMD.SUD before they send
     * COMRESET; without it the reset above already started link bring-up. */
    for (int i = 0; i < 32; i++) {
        if (!(pi & (1u << i)))
            continue;
        volatile uint8_t *p = abar + 0x100u + (uint32_t)i * 0x80u;
        if (cap & CAP_SSS)
            wr(p, PX_CMD, rd(p, PX_CMD) | PXCMD_SUD);
    }

    /* Links come up within ~10 ms of COMRESET.  Wait for all implemented
     * ports together, so empty ports cost one wait, not one each. */
    ahci_timer_t t;
    timer_start(&t, 50);
    for (;;) {
        int pending = 0;
        for (int i = 0; i < 32; i++) {
            if (!(pi & (1u << i)))
                continue;
            volatile uint8_t *p = abar + 0x100u + (uint32_t)i * 0x80u;
            if ((rd(p, PX_SSTS) & SSTS_DET_MASK) != SSTS_DET_OK)
                pending = 1;
        }
        if (!pending || timer_expired(&t))
            break;
    }

    for (int i = 0; i < 32; i++) {
        if (!(pi & (1u << i)))
            continue;
        volatile uint8_t *p = abar + 0x100u + (uint32_t)i * 0x80u;
        uint32_t det = rd(p, PX_SSTS) & SSTS_DET_MASK;
        /* DET 1: a device is there but the phy never came up; one COMRESET. */
        if (det == 1 && port_comreset(p) == 0)
            det = SSTS_DET_OK;
        if (det == SSTS_DET_OK)
            port_probe(abar, cap, ctrl, i);
    }
}

void ahci_init(void) {
    spin_init(&ahci_lock);
    int nctrl = 0;
    for (int i = 0; i < pci_device_count(); i++) {
        const pci_device_t *pd = pci_get_device(i);
        if (pd->class_code != 0x01 || pd->subclass != 0x06 || pd->prog_if != 0x01)
            continue;
        if (nctrl >= AHCI_MAX_CTRL) {
            printk("[AHCI] more than %d controllers, ignoring the rest\n", AHCI_MAX_CTRL);
            break;
        }
        ahci_init_ctrl(pd, nctrl++);
    }
    if (nctrl)
        printk("[AHCI] %d disk(s) on %d controller(s)\n", ndisks, nctrl);
}

int ahci_disk_count(void) {
    return ndisks;
}

uint32_t ahci_disk_sectors(int disk) {
    if (disk < 0 || disk >= ndisks)
        return 0;
    return disks[disk].sectors > 0xFFFFFFFFull ? 0xFFFFFFFFu
                                               : (uint32_t)disks[disk].sectors;
}

static int ahci_rw(int disk, uint32_t lba, uint32_t count, void *buf, int write) {
    if (disk < 0 || disk >= ndisks || count == 0 || count > 256)
        return -1;
    ahci_disk_t *d = &disks[disk];
    if ((uint64_t)lba + count > d->sectors)
        return -1;

    uint32_t bytes = count * 512u;
    uint32_t irq = ahci_irq_save();
    spin_lock(&ahci_lock);
    if (write)
        memcpy(ahci_bounce, buf, bytes);
    int rc = -1;
    for (int attempt = 0; attempt < 2 && rc < 0; attempt++) {
        rc = port_exec(d, write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT,
                       lba, count, bytes, write, 10000);
        /* Flush the write cache, and fail the write if the flush did not
         * land (as the IDE driver does). */
        if (rc == 0 && write)
            rc = port_exec(d, ATA_FLUSH_EXT, 0, 0, 0, 0, 30000);
    }
    if (rc == 0 && !write)
        memcpy(buf, ahci_bounce, bytes);
    spin_unlock(&ahci_lock);
    ahci_irq_restore(irq);
    return rc;
}

int ahci_read(int disk, uint32_t lba, uint32_t count, void *buf) {
    return ahci_rw(disk, lba, count, buf, 0);
}

int ahci_write(int disk, uint32_t lba, uint32_t count, const void *buf) {
    return ahci_rw(disk, lba, count, (void *)buf, 1);
}
