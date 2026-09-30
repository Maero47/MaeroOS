#include "ata.h"
#include "../kernel/printk.h"
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
#define ATA_CMD_WRITE      0x30
#define ATA_CMD_FLUSH      0xE7
#define ATA_CMD_IDENT      0xEC
#define ATA_CMD_SET_MULT   0xC6

static int drive_present = 0;

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

void ata_init(void) {
    /* Read the 256-word identify data; word 47 low byte is the largest block
     * READ/WRITE MULTIPLE may use (0 means the drive does not support it). */
    uint16_t ident[256];
    int r = -1;
    for (int attempt = 0; attempt < 3 && r < 0; attempt++)
        r = ata_probe(ident);
    if (r <= 0) {
        printk("[ATA]  Drive not present.\n");
        return;
    }

    drive_present = 1;

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

    if (ata_multi)
        printk("[ATA]  Primary master ready (READ MULTIPLE, %u sectors/block).\n",
               (unsigned)ata_multi);
    else
        printk("[ATA]  Primary master ready.\n");
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
    uint32_t irq = ata_irq_save();   /* serialize the whole PIO transaction */
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
