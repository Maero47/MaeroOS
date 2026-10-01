#include "ata.h"
#include "pci.h"
#include "../kernel/printk.h"
#include "../lib/string.h"
#include <kernel/config.h>
#include <kernel/kprof.h>
#include "../arch/i686/cpu/tsc.h"
#include <io.h>
#include <stdint.h>

/*
 * ATA PIO is a stateful transaction (issue command → poll DRQ → insw sectors).
 * With preemptible multithreaded processes (Firefox), two threads doing
 * concurrent file-backed mmaps hit ext2 → ata_read concurrently; if one is
 * preempted mid-transfer and another issues a command, the controller state is
 * corrupted and the first thread reads garbage → non-deterministic crashes.
 * ext2 reads are small (1 block = 2 sectors), so serialize each transaction
 * with a saved-IF cli/sti (single CPU).
 */
static inline uint32_t ata_irq_save(void) {
    uint32_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void ata_irq_restore(uint32_t f) {
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
}

/* Primary channel I/O registers */
#define ATA_DATA     0x1F0   /* 16-bit data port */
#define ATA_FEAT     0x1F1   /* features / error (read) */
#define ATA_NSECT    0x1F2   /* sector count */
#define ATA_LBAL     0x1F3   /* LBA bits 0-7 */
#define ATA_LBAM     0x1F4   /* LBA bits 8-15 */
#define ATA_LBAH     0x1F5   /* LBA bits 16-23 */
#define ATA_DRIVE    0x1F6   /* drive/head, LBA bits 24-27 */
#define ATA_STATUS   0x1F7   /* status (read) / command (write) */
#define ATA_CMD      0x1F7
#define ATA_ALT      0x3F6   /* alternate status / device control */

/* Status register bits */
#define ATA_SR_BSY  0x80   /* busy */
#define ATA_SR_DRDY 0x40   /* drive ready */
#define ATA_SR_DF   0x20   /* drive fault */
#define ATA_SR_DRQ  0x08   /* data request (ready to transfer) */
#define ATA_SR_ERR  0x01   /* error */

/* Commands */
#define ATA_CMD_READ       0x20
#define ATA_CMD_READ_MUL   0xC4
#define ATA_CMD_READ_DMA   0xC8
#define ATA_CMD_WRITE      0x30
#define ATA_CMD_FLUSH      0xE7
#define ATA_CMD_IDENT      0xEC
#define ATA_CMD_SET_MULT   0xC6

static int drive_present = 0;
static uint32_t master_sectors = 0;   /* LBA28 capacity, IDENTIFY words 60-61 */
static uint64_t master_capacity = 0;  /* whole drive, LBA48 words 100-103 */

/* The drive's real size: the LBA48 count (words 100-103) when word 83 bit 10
 * says the feature set is there, else the LBA28 count.  I/O here is LBA28
 * only, so a drive past 128 GiB is used up to its first 2^28 - 1 sectors;
 * this tells BLKGETSIZE64 (and so maeros-install) how big it really is. */
static uint64_t ident_capacity(const uint16_t *ident) {
    uint64_t lba28 = (uint32_t)ident[60] | ((uint32_t)ident[61] << 16);
    if (!(ident[83] & (1u << 10))) return lba28;
    uint64_t lba48 = (uint64_t)ident[100] | ((uint64_t)ident[101] << 16) |
                     ((uint64_t)ident[102] << 32) | ((uint64_t)ident[103] << 48);
    return lba48 > lba28 ? lba48 : lba28;
}

static void note_lba28_limit(const char *name, uint32_t sectors, uint64_t capacity) {
    if (capacity > sectors)
        printk("[ATA]  %s: %u MiB, but LBA28 reaches only the first %u MiB\n", name,
               (unsigned)(capacity / 2048u), (unsigned)(sectors / 2048u));
}

/*
 * Sectors the drive transfers per DRQ assertion (READ MULTIPLE block size), or
 * 0 when multiple mode is not in use and every sector needs its own handshake.
 *
 * Under KVM each port access in a PIO transaction is an exit into QEMU, and
 * single-sector PIO spends three of them per 512 bytes: poll the status for
 * DRQ, `rep insw` the data, read the alternate status to let the drive settle.
 * Two of those three are handshake, not data.  READ MULTIPLE is the ATA
 * feature for exactly this: the drive asserts DRQ once per block of N sectors
 * and the host transfers the whole block, so the handshake is paid once per
 * block instead of once per sector.  Measured on this workload a sector cost
 * ~46 us with the per-sector handshake.
 *
 * The drive states its maximum block in IDENTIFY word 47 and the host selects
 * one with SET MULTIPLE MODE; both are checked, and anything unexpected leaves
 * multi = 0 and the original single-sector path, which is still correct.
 */
static uint32_t ata_multi = 0;
#define ATA_MULTI_WANT 16      /* 8 KiB per handshake */

/* Read alternate status 4 times (~400 ns delay).  Required after a write to the
 * command or drive-select register, before the status register is meaningful. */
static void ata_delay(void) {
    inb(ATA_ALT); inb(ATA_ALT); inb(ATA_ALT); inb(ATA_ALT);
}

/* The same settle between the sectors of one multi-sector transfer, where the
 * drive has just gone busy on its own: a single alternate-status read is what
 * the interface needs there.  Each of these reads is a port access, and under
 * KVM every port access is a VM exit into QEMU (~2 us), so the other three were
 * pure cost on every 512 bytes the guest reads. */
static inline void ata_settle(void) {
    inb(ATA_ALT);
}

/*
 * Command timeouts are wall-clock, not a fixed number of status reads.  A read
 * costs anything from ~1 us on hardware to far more under emulation, and some
 * commands are slow when nothing is wrong: FLUSH CACHE on QEMU's default
 * writeback cache is a host fdatasync, which a busy host can hold for seconds,
 * and a real drive may spin up or flush for tens of seconds.  Failing those
 * turned a slow but successful write into an ext2 I/O error.
 *
 * The transactions run with interrupts off (see ata_irq_save), so the PIT tick
 * cannot advance inside them; the deadline is measured on the TSC instead,
 * which keeps counting.  Before the TSC is calibrated (ata_init runs before
 * the PIT is even programmed) a status-read budget stands in, sized for at
 * least the same time on hardware.
 */
#define ATA_TIMEOUT_MS        10000   /* BSY / DRQ for ordinary commands */
#define ATA_FLUSH_TIMEOUT_MS  30000   /* FLUSH CACHE */

typedef struct {
    uint64_t end;          /* TSC deadline, or 0 when uncalibrated */
    uint32_t spins, max_spins;
} ata_timer_t;

static inline uint64_t ata_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void ata_timer_start(ata_timer_t *t, uint32_t ms) {
    uint32_t cpt = tsc_cycles_per_tick();          /* cycles per 10 ms */
    t->spins = 0;
    if (cpt) {
        t->end = ata_rdtsc() + (uint64_t)cpt * (ms / 10u);
        t->max_spins = 0;
    } else {
        t->end = 0;
        t->max_spins = ms * 1000u;                 /* >= 1 us per status read */
    }
}

static int ata_timer_expired(ata_timer_t *t) {
    if (t->end)
        return ata_rdtsc() >= t->end;
    return ++t->spins > t->max_spins;
}

/* Wait until BSY clears; return -1 on timeout */
static int ata_wait_bsy_ms(uint32_t ms) {
    ata_timer_t t;
    ata_timer_start(&t, ms);
    do {
        if (!(inb(ATA_STATUS) & ATA_SR_BSY))
            return 0;
    } while (!ata_timer_expired(&t));
    return -1;  /* timeout */
}

static int ata_wait_bsy(void) {
    return ata_wait_bsy_ms(ATA_TIMEOUT_MS);
}

/* Wait for the end of a non-data command (or the last sector of a write) and
 * report its outcome: -1 on timeout, or when the drive set ERR or DF. */
static int ata_wait_done_ms(uint32_t ms) {
    if (ata_wait_bsy_ms(ms) < 0)
        return -1;
    return (inb(ATA_STATUS) & (ATA_SR_ERR | ATA_SR_DF)) ? -1 : 0;
}

static int ata_wait_done(void) {
    return ata_wait_done_ms(ATA_TIMEOUT_MS);
}

/* Wait until DRQ is set (data ready); return -1 on error.  ERR, DF and DRQ are
 * only meaningful once BSY has cleared, so a busy status is just polled past. */
static int ata_wait_drq(void) {
    ata_timer_t t;
    ata_timer_start(&t, ATA_TIMEOUT_MS);
    do {
        uint8_t s = inb(ATA_STATUS);
        if (s & ATA_SR_BSY)
            continue;
        if (s & (ATA_SR_ERR | ATA_SR_DF))
            return -1;
        if (s & ATA_SR_DRQ)
            return 0;
    } while (!ata_timer_expired(&t));
    return -1;  /* timeout */
}

/*
 * Select the primary master once the channel has come out of a software
 * reset.  Returns 1 when it answers ready, 0 when nothing is on the channel,
 * -1 when something is there but never became ready.
 *
 * The drive-select register cannot be written while the selected device is
 * busy, and a reset leaves both devices busy until it completes.  QEMU finishes
 * the reset asynchronously, in its main loop, and the BIOS may have left the
 * (absent) slave selected: a select written before the reset lands is dropped,
 * status then reads 0 from the missing slave, and a single-shot probe decides
 * the master is not there.  How long the reset takes depends on how busy the
 * host is.  So wait out BSY and re-issue the select until the master answers
 * with the select bit reading back clear.
 */
#define ATA_DETECT_POLLS 0x40000

static int ata_select_master(void) {
    for (int i = 0; i < ATA_DETECT_POLLS; i++) {
        uint8_t s = inb(ATA_ALT);
        if (s == 0xFF)
            return 0;               /* floating bus: no controller or drive */
        if (s & ATA_SR_BSY)
            continue;
        outb(ATA_DRIVE, 0xA0);
        ata_delay();
        s = inb(ATA_STATUS);
        uint8_t sel = inb(ATA_DRIVE);
        if (s == 0 && sel == 0)
            return 0;               /* no device on either position */
        if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRDY) && !(sel & 0x10))
            return 1;
    }
    return -1;
}

/*
 * Reset the channel and IDENTIFY the primary master into `ident`.  Returns 1
 * on success, 0 when there is no (ATA) drive to find, -1 on a failure worth
 * retrying.
 */
static int ata_probe(uint16_t *ident) {
    outb(ATA_ALT, 0x04);            /* software reset */
    ata_delay();
    outb(ATA_ALT, 0x00);
    ata_delay();

    int r = ata_select_master();
    if (r <= 0)
        return r;

    outb(ATA_NSECT, 0);
    outb(ATA_LBAL,  0);
    outb(ATA_LBAM,  0);
    outb(ATA_LBAH,  0);
    outb(ATA_CMD,   ATA_CMD_IDENT);
    ata_delay();

    if (inb(ATA_STATUS) == 0)
        return -1;
    if (ata_wait_bsy() < 0)
        return -1;
    /* The signature is only valid once BSY has cleared.  A packet device
     * aborts IDENTIFY (ERR) and leaves 0x14/0xEB (or SATA's 0x69/0x96) here. */
    uint8_t lm = inb(ATA_LBAM), lh = inb(ATA_LBAH);
    if (lm || lh) {
        printk("[ATA]  Non-ATA device detected (signature %02x/%02x) — skipping.\n",
               (unsigned)lm, (unsigned)lh);
        return 0;
    }
    if (ata_wait_drq() < 0)
        return -1;

    for (int i = 0; i < 256; i++)
        ident[i] = inw(ATA_DATA);
    return 1;
}

/*
 * Bus-master DMA for reads (PCI IDE, SFF-8038i).  PIO moves every word with
 * `rep insw`, and under KVM that is a trip into QEMU per chunk of the string
 * plus QEMU's per-sector PIO state machine: ~17 us a sector, 4.5 s of the
 * ~6 s from firefox-bin's exec to its first paint.  With DMA the controller
 * (QEMU) writes the sectors straight into guest memory in one request.
 *
 * The transfer goes through a bounce buffer in .bss, physically contiguous
 * because the kernel image is, and big enough for the largest read (count 0 =
 * 256 sectors = 128 KiB); callers pass heap or cache buffers, which are not.
 * A PRD entry may not cross a 64 KiB boundary, so the table splits the buffer
 * there (three entries at most).  The buffer is only page aligned on purpose:
 * a 64 KiB alignment raises the ELF data segment's alignment, the linker then
 * pads its file offset, and the multiboot loader - which copies the file
 * linearly (load_end_addr 0 in boot.asm) - put .data at the wrong address
 * (init came up as pid 0).  Completion is
 * polled with interrupts off, like the PIO path (IRQ 14 stays masked).
 * Writes stay PIO: they are rare and small here.  A controller that is absent,
 * not bus-master capable, or that reports an error leaves DMA off and every
 * read on the PIO path.
 */
#define BM_CMD     0      /* bus-master registers, primary channel */
#define BM_STATUS  2
#define BM_PRDT    4
#define BM_CMD_START 0x01
#define BM_CMD_READ  0x08 /* device -> memory */
#define BM_ST_ACTIVE 0x01
#define BM_ST_ERR    0x02
#define BM_ST_IRQ    0x04

typedef struct { uint32_t addr; uint16_t bytes; uint16_t flags; } __attribute__((packed)) ata_prd_t;
#define ATA_DMA_BYTES (128u * 1024u)
/* The bus-master PRD table must not cross a 64 KiB boundary (it is fetched
 * with a 16-bit offset): aligning the 24-byte table to 32, which divides
 * 64 KiB, makes that hold wherever .bss puts it. */
static ata_prd_t ata_prdt[3] __attribute__((aligned(32)));
_Static_assert(sizeof(ata_prdt) <= 32, "PRD table must fit its alignment");
static uint8_t   ata_dma_buf[ATA_DMA_BYTES] __attribute__((aligned(4096)));
static uint16_t  ata_bm;          /* bus-master I/O base, 0 = no DMA */

static uint32_t kphys(const void *p) {
    return (uint32_t)(uintptr_t)p - (uint32_t)KERNEL_VMA;
}

static void ata_dma_init(const uint16_t *ident) {
    if (!(ident[49] & 0x0100))                     /* DMA supported */
        return;
    const pci_device_t *d = pci_find_class(0x01, 0x01);   /* IDE controller */
    if (!d || !(d->prog_if & 0x80))                /* bus-master capable */
        return;
    uint32_t bar4 = d->bar[4];
    if (!(bar4 & 1) || !(bar4 & ~3u))              /* must be an I/O BAR */
        return;
    uint32_t cmd = pci_read_config32(d->bus, d->slot, d->func, 0x04);
    if (!(cmd & 0x4))                              /* enable bus mastering */
        pci_write_config32(d->bus, d->slot, d->func, 0x04, (cmd & 0xFFFF) | 0x5);
    ata_bm = (uint16_t)(bar4 & ~3u);
    outb(ata_bm + BM_CMD, 0);
    outb(ata_bm + BM_STATUS, BM_ST_ERR | BM_ST_IRQ);
}

/* One READ DMA of nsect (1..256) sectors into the bounce buffer.  0 on
 * success, -1 if the transfer failed (the caller then falls back to PIO).
 * Runs with interrupts off. */
static int ata_read_dma(uint32_t lba, uint8_t count, uint32_t nsect) {
    uint32_t left = nsect * 512u, pa = kphys(ata_dma_buf);
    int n = 0;
    while (left) {
        uint32_t run = 0x10000u - (pa & 0xFFFFu);  /* to the next 64 KiB line */
        if (run > left) run = left;
        ata_prdt[n].addr  = pa;
        ata_prdt[n].bytes = (uint16_t)run;         /* 65536 wraps to 0 = 64 KiB */
        ata_prdt[n].flags = 0;
        pa += run; left -= run; n++;
    }
    ata_prdt[n - 1].flags = 0x8000;                /* end of table */

    if (ata_wait_bsy() < 0)
        return -1;
    outb(ata_bm + BM_CMD, 0);
    outl(ata_bm + BM_PRDT, kphys(ata_prdt));
    outb(ata_bm + BM_CMD, BM_CMD_READ);
    outb(ata_bm + BM_STATUS, BM_ST_ERR | BM_ST_IRQ);

    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_FEAT,  0x00);
    outb(ATA_NSECT, count);
    outb(ATA_LBAL,  (uint8_t)(lba));
    outb(ATA_LBAM,  (uint8_t)(lba >> 8));
    outb(ATA_LBAH,  (uint8_t)(lba >> 16));
    outb(ATA_CMD,   ATA_CMD_READ_DMA);
    outb(ata_bm + BM_CMD, BM_CMD_READ | BM_CMD_START);

    /* Done when the controller raises its interrupt bit (or stops with an
     * error) and the drive has dropped BSY. */
    ata_timer_t t;
    ata_timer_start(&t, ATA_TIMEOUT_MS);
    uint8_t bst;
    int ok = 0;
    do {
        bst = inb(ata_bm + BM_STATUS);
        if (bst & BM_ST_ERR)
            break;
        if ((bst & BM_ST_IRQ) && !(inb(ATA_ALT) & ATA_SR_BSY)) {
            ok = 1;
            break;
        }
    } while (!ata_timer_expired(&t));
    outb(ata_bm + BM_CMD, 0);
    uint8_t st = inb(ATA_STATUS);                  /* also acks the drive's INTRQ */
    outb(ata_bm + BM_STATUS, BM_ST_ERR | BM_ST_IRQ);
    if (!ok || (bst & BM_ST_ERR) || (st & (ATA_SR_ERR | ATA_SR_DF | ATA_SR_BSY)))
        return -1;
    return 0;
}

static void ata_probe_others(void);

void ata_init(void) {
    /* Read the 256-word identify data; word 47 low byte is the largest block
     * READ/WRITE MULTIPLE may use (0 means the drive does not support it). */
    uint16_t ident[256];
    int r = -1;
    for (int attempt = 0; attempt < 3 && r < 0; attempt++)
        r = ata_probe(ident);
    if (r <= 0) {
        printk("[ATA]  Drive not present.\n");
        ata_probe_others();
        return;
    }

    drive_present = 1;
    master_sectors = (uint32_t)ident[60] | ((uint32_t)ident[61] << 16);
    master_capacity = ident_capacity(ident);
    note_lba28_limit("hda", master_sectors, master_capacity);

    uint32_t max_multi = ident[47] & 0xFFu;
    if (max_multi) {
        uint32_t want = max_multi < ATA_MULTI_WANT ? max_multi : ATA_MULTI_WANT;
        if (ata_wait_bsy() == 0) {
            outb(ATA_DRIVE, 0xA0);
            outb(ATA_NSECT, (uint8_t)want);
            outb(ATA_CMD,   ATA_CMD_SET_MULT);
            ata_delay();
            /* The drive rejects a block size it cannot do by setting ERR;
             * anything short of a clean ready-without-error leaves multiple
             * mode off. */
            if (ata_wait_done() == 0 && (inb(ATA_STATUS) & ATA_SR_DRDY))
                ata_multi = want;
        }
    }

    ata_dma_init(ident);

    if (ata_multi)
        printk("[ATA]  Primary master ready (READ MULTIPLE, %u sectors/block%s).\n",
               (unsigned)ata_multi, ata_bm ? ", bus-master DMA" : "");
    else
        printk("[ATA]  Primary master ready%s.\n", ata_bm ? " (bus-master DMA)" : "");

    ata_probe_others();
}

int ata_present(void) {
    return drive_present;
}

int ata_read(uint32_t lba, uint8_t count, void *buf) {
    if (!drive_present) return -1;

    kprof_count(KPE_ATA_RD);
    kprof_add(KPE_ATA_RD_SECT, count ? count : 256);
    int kp_old = kprof_switch(KPB_ATA);
    /* Split the cost by transaction size so the fixed per-command cost and
     * the per-sector cost can be separated from the totals alone. */
    int kp_id = (count && count <= 4) ? KPP_ATA_SMALL : KPP_ATA_BIG;
    uint64_t kp_t0 = kprof_probe_begin();
    uint32_t irq = ata_irq_save();   /* serialize the whole transaction */
    if (ata_bm) {
        uint32_t n = count ? count : 256u;
        if (ata_read_dma(lba, count, n) == 0) {
            memcpy(buf, ata_dma_buf, n * 512u);
            ata_irq_restore(irq);
            kprof_probe_end(kp_id, kp_t0);
            kprof_switch(kp_old);
            return 0;
        }
        printk("[ATA]  DMA read of LBA %u failed; using PIO from now on.\n",
               (unsigned)lba);
        ata_bm = 0;
    }
    if (ata_wait_bsy() < 0) {
        ata_irq_restore(irq);
        kprof_probe_end(kp_id, kp_t0);
        kprof_switch(kp_old);
        return -1;
    }

    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));  /* LBA mode, master */
    outb(ATA_FEAT,  0x00);
    outb(ATA_NSECT, count);
    outb(ATA_LBAL,  (uint8_t)(lba));
    outb(ATA_LBAM,  (uint8_t)(lba >> 8));
    outb(ATA_LBAH,  (uint8_t)(lba >> 16));
    outb(ATA_CMD,   ata_multi ? ATA_CMD_READ_MUL : ATA_CMD_READ);
    ata_delay();

    uint32_t nsect = (count == 0) ? 256u : (uint32_t)count;
    uint32_t blk   = ata_multi ? ata_multi : 1u;
    uint16_t *ptr  = (uint16_t *)buf;

    /* One DRQ handshake per block; the last block is short when the transfer
     * is not a whole number of blocks, exactly as the standard specifies. */
    for (uint32_t done = 0; done < nsect; ) {
        uint32_t n = nsect - done;
        if (n > blk) n = blk;
        if (ata_wait_drq() < 0) {
            ata_irq_restore(irq);
            kprof_probe_end(kp_id, kp_t0);
            kprof_switch(kp_old);
            return -1;
        }
        insw(ATA_DATA, ptr, n * 256u);   /* 256 words = 512 bytes per sector */
        ptr  += n * 256u;
        done += n;
        ata_settle();
    }
    ata_irq_restore(irq);
    kprof_probe_end(kp_id, kp_t0);
    kprof_switch(kp_old);
    return 0;
}

int ata_write(uint32_t lba, uint8_t count, const void *buf) {
    if (!drive_present) return -1;

    kprof_count(KPE_ATA_WR);
    kprof_add(KPE_ATA_WR_SECT, count ? count : 256);
    int kp_old = kprof_switch(KPB_ATA);
    uint32_t irq = ata_irq_save();   /* serialize the whole PIO transaction */
    int rc = -1;
    if (ata_wait_bsy() < 0)
        goto out;

    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_FEAT,  0x00);
    outb(ATA_NSECT, count);
    outb(ATA_LBAL,  (uint8_t)(lba));
    outb(ATA_LBAM,  (uint8_t)(lba >> 8));
    outb(ATA_LBAH,  (uint8_t)(lba >> 16));
    outb(ATA_CMD,   ATA_CMD_WRITE);
    ata_delay();

    int nsect = (count == 0) ? 256 : (int)count;
    const uint16_t *ptr = (const uint16_t *)buf;

    for (int s = 0; s < nsect; s++) {
        if (ata_wait_drq() < 0)
            goto out;
        outsw(ATA_DATA, ptr, 256);
        ptr += 256;
        ata_settle();
    }

    /* The drive goes BSY committing the last sector and reports a failed
     * write with ERR/DF once it is done; a command written while BSY is set
     * is ignored, so the flush has to wait for that anyway. */
    if (ata_wait_done() < 0)
        goto out;

    /* Flush the write cache, and fail the write if the flush did not land. */
    outb(ATA_CMD, ATA_CMD_FLUSH);
    ata_delay();
    if (ata_wait_done_ms(ATA_FLUSH_TIMEOUT_MS) < 0)
        goto out;
    rc = 0;
out:
    ata_irq_restore(irq);
    kprof_switch(kp_old);
    return rc;
}

/* ── The other three drive positions (hdb, hdc, hdd) ─────────────────────────
 *
 * Everything above serves the primary master, the boot disk, and is tuned for
 * it (READ MULTIPLE, bus-master DMA).  The other positions get a plain LBA28
 * PIO path: they hold disks that are mounted later with mount(2), and are read
 * far less.  Each transaction runs with interrupts off like the master's, and
 * selects its drive explicitly; the master's own commands select the master
 * again before they start, so the two never confuse the shared channel.
 *
 * Primary slave is probed without a channel reset (a reset would drop the
 * master's READ MULTIPLE setting); the secondary channel is reset once.  A
 * packet (ATAPI) device, such as QEMU's CD-ROM, answers IDENTIFY with an abort
 * and the packet signature and is skipped. */
typedef struct {
    uint16_t io, ctl;     /* command block base, control/alt-status port */
    uint8_t  slave;       /* 0 master, 1 slave */
    uint8_t  present;
    uint32_t sectors;
    uint64_t capacity;    /* ident_capacity() */
} ata_xdev_t;

static ata_xdev_t ata_xdev[ATA_MAX_DEVS] = {
    { 0x1F0, 0x3F6, 0, 0, 0, 0 },     /* hda: served by the code above */
    { 0x1F0, 0x3F6, 1, 0, 0, 0 },     /* hdb */
    { 0x170, 0x376, 0, 0, 0, 0 },     /* hdc */
    { 0x170, 0x376, 1, 0, 0, 0 },     /* hdd */
};

static int xdev_wait_bsy(const ata_xdev_t *d, uint32_t ms) {
    ata_timer_t t;
    ata_timer_start(&t, ms);
    do {
        if (!(inb(d->io + 7) & ATA_SR_BSY))
            return 0;
    } while (!ata_timer_expired(&t));
    return -1;
}

static int xdev_wait_drq(const ata_xdev_t *d) {
    ata_timer_t t;
    ata_timer_start(&t, ATA_TIMEOUT_MS);
    do {
        uint8_t s = inb(d->io + 7);
        if (s & ATA_SR_BSY)
            continue;
        if (s & (ATA_SR_ERR | ATA_SR_DF))
            return -1;
        if (s & ATA_SR_DRQ)
            return 0;
    } while (!ata_timer_expired(&t));
    return -1;
}

static void xdev_delay(const ata_xdev_t *d) {
    inb(d->ctl); inb(d->ctl); inb(d->ctl); inb(d->ctl);
}

/* IDENTIFY one position; 1 when an ATA disk answered. */
static int xdev_probe(ata_xdev_t *d, uint16_t *ident) {
    uint8_t s = inb(d->ctl);
    if (s == 0xFF) return 0;                       /* floating bus */
    if (xdev_wait_bsy(d, 1000) < 0) return 0;
    outb(d->io + 6, d->slave ? 0xB0 : 0xA0);
    xdev_delay(d);
    s = inb(d->io + 7);
    if (s == 0 || s == 0xFF) return 0;             /* nothing at this position */
    outb(d->io + 2, 0);
    outb(d->io + 3, 0);
    outb(d->io + 4, 0);
    outb(d->io + 5, 0);
    outb(d->io + 7, ATA_CMD_IDENT);
    xdev_delay(d);
    if (inb(d->io + 7) == 0) return 0;
    if (xdev_wait_bsy(d, 2000) < 0) return 0;
    if (inb(d->io + 4) || inb(d->io + 5)) return 0;   /* ATAPI / SATA packet */
    if (xdev_wait_drq(d) < 0) return 0;
    for (int i = 0; i < 256; i++)
        ident[i] = inw(d->io);
    return 1;
}

static void ata_probe_others(void) {
    static const char *names[ATA_MAX_DEVS] = { "hda", "hdb", "hdc", "hdd" };
    uint16_t ident[256];
    ata_xdev[0].present = (uint8_t)drive_present;
    ata_xdev[0].sectors = master_sectors;

    /* Reset the secondary channel once, if there is one. */
    if (inb(0x376) != 0xFF) {
        outb(0x376, 0x04);
        xdev_delay(&ata_xdev[2]);
        outb(0x376, 0x00);
        xdev_delay(&ata_xdev[2]);
        xdev_wait_bsy(&ata_xdev[2], 2000);
    }
    for (int i = 1; i < ATA_MAX_DEVS; i++) {
        ata_xdev_t *d = &ata_xdev[i];
        uint32_t irq = ata_irq_save();
        int ok = xdev_probe(d, ident);
        ata_irq_restore(irq);
        if (!ok) continue;
        d->sectors = (uint32_t)ident[60] | ((uint32_t)ident[61] << 16);
        if (!d->sectors) continue;
        d->capacity = ident_capacity(ident);
        d->present = 1;
        printk("[ATA]  %s: %u sectors (%u MiB), PIO.\n", names[i],
               (unsigned)d->sectors, (unsigned)(d->sectors / 2048u));
        note_lba28_limit(names[i], d->sectors, d->capacity);
    }
    /* Leave the primary channel pointing at the master, as before. */
    if (drive_present) {
        outb(ATA_DRIVE, 0xA0);
        ata_delay();
    }
}

int ata_dev_present(int dev) {
    if (dev < 0 || dev >= ATA_MAX_DEVS) return 0;
    return dev == 0 ? drive_present : ata_xdev[dev].present;
}

uint32_t ata_dev_sectors(int dev) {
    if (!ata_dev_present(dev)) return 0;
    return dev == 0 ? master_sectors : ata_xdev[dev].sectors;
}

uint64_t ata_dev_capacity(int dev) {
    if (!ata_dev_present(dev)) return 0;
    return dev == 0 ? master_capacity : ata_xdev[dev].capacity;
}

static int xdev_rw(int dev, uint32_t lba, uint8_t count, void *buf, int write) {
    ata_xdev_t *d = &ata_xdev[dev];
    uint32_t nsect = count ? count : 256u;
    if (lba >= d->sectors || nsect > d->sectors - lba || lba >= (1u << 28))
        return -1;
    kprof_count(write ? KPE_ATA_WR : KPE_ATA_RD);
    uint32_t irq = ata_irq_save();
    int rc = -1;
    if (xdev_wait_bsy(d, ATA_TIMEOUT_MS) < 0)
        goto out;
    outb(d->io + 6, (uint8_t)((d->slave ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F)));
    xdev_delay(d);
    outb(d->io + 1, 0);
    outb(d->io + 2, count);
    outb(d->io + 3, (uint8_t)lba);
    outb(d->io + 4, (uint8_t)(lba >> 8));
    outb(d->io + 5, (uint8_t)(lba >> 16));
    outb(d->io + 7, write ? ATA_CMD_WRITE : ATA_CMD_READ);
    xdev_delay(d);
    uint16_t *p = (uint16_t *)buf;
    for (uint32_t s = 0; s < nsect; s++) {
        if (xdev_wait_drq(d) < 0)
            goto out;
        if (write) outsw(d->io, p, 256);
        else       insw(d->io, p, 256);
        p += 256;
        inb(d->ctl);
    }
    if (write) {
        if (xdev_wait_bsy(d, ATA_TIMEOUT_MS) < 0 ||
            (inb(d->io + 7) & (ATA_SR_ERR | ATA_SR_DF)))
            goto out;
        outb(d->io + 7, ATA_CMD_FLUSH);
        xdev_delay(d);
        if (xdev_wait_bsy(d, ATA_FLUSH_TIMEOUT_MS) < 0 ||
            (inb(d->io + 7) & (ATA_SR_ERR | ATA_SR_DF)))
            goto out;
    }
    rc = 0;
out:
    ata_irq_restore(irq);
    return rc;
}

int ata_dev_read(int dev, uint32_t lba, uint8_t count, void *buf) {
    if (!ata_dev_present(dev)) return -1;
    if (dev == 0) return ata_read(lba, count, buf);
    return xdev_rw(dev, lba, count, buf, 0);
}

int ata_dev_write(int dev, uint32_t lba, uint8_t count, const void *buf) {
    if (!ata_dev_present(dev)) return -1;
    if (dev == 0) return ata_write(lba, count, buf);
    return xdev_rw(dev, lba, count, (void *)(uintptr_t)buf, 1);
}
