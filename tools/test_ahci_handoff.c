/*
 * Host test for the AHCI BIOS/OS handoff (drivers/ahci_handoff.h) against a
 * simulated firmware: QEMU's AHCI has no CAP2.BOH, so the boot smoke cannot
 * reach this path.  Built and run by tools/smoke_ahci.py:
 *   cc -I drivers -o test_ahci_handoff tools/test_ahci_handoff.c && ./test_ahci_handoff
 *
 * The fake clock advances only through sleep_ms; the firmware sees OOS when
 * it is written and releases BOS (and drops BB) at a scripted time.
 */
#include <stdio.h>
#include <stdint.h>
#include "ahci_handoff.h"

#define NEVER 0xFFFFFFFFu

typedef struct {
    ahci_bohc_io_t io;
    uint32_t now;          /* ms since the start of the handoff */
    uint32_t bohc;
    uint32_t oos_at;       /* when OOS was written, NEVER if not yet */
    uint32_t bb_for;       /* BB stays set this long after OOS */
    uint32_t release_after;/* BOS clears this long after OOS, or NEVER */
} fake_t;

static void fw_step(fake_t *f) {
    if (f->oos_at == NEVER)
        return;
    uint32_t since = f->now - f->oos_at;
    if (since >= f->bb_for)
        f->bohc &= ~BOHC_BB;
    if (f->release_after != NEVER && since >= f->release_after)
        f->bohc &= ~BOHC_BOS;
}

static uint32_t fake_read(ahci_bohc_io_t *io) {
    fake_t *f = (fake_t *)io;
    fw_step(f);
    return f->bohc;
}

static void fake_write(ahci_bohc_io_t *io, uint32_t v) {
    fake_t *f = (fake_t *)io;
    /* Only OOS (and the OOC/SOOE bits, unused here) are OS-writable. */
    if ((v & BOHC_OOS) && f->oos_at == NEVER)
        f->oos_at = f->now;
    f->bohc = (f->bohc & ~BOHC_OOS) | (v & BOHC_OOS);
}

static void fake_sleep(ahci_bohc_io_t *io, uint32_t ms) {
    ((fake_t *)io)->now += ms;
}

static int failures;

static void scenario(const char *name, uint32_t bb_for, uint32_t release_after,
                     int want_rc, int want_busy) {
    fake_t f = { { fake_read, fake_write, fake_sleep }, 0,
                 BOHC_BOS | (bb_for ? BOHC_BB : 0), NEVER, bb_for, release_after };
    int busy = -1;
    int rc = ahci_bios_handoff(&f.io, &busy);
    int ok = rc == want_rc && busy == want_busy && f.oos_at == 0;
    if (rc == 0) {
        /* Done only once the BIOS has really let go, and never before. */
        ok = ok && !(f.bohc & BOHC_BOS) && f.now >= release_after &&
             f.now >= AHCI_HANDOFF_SETTLE_MS;
    } else {
        /* Gave up only after the full budget the spec asks for. */
        uint32_t budget = AHCI_HANDOFF_SETTLE_MS +
                          (want_busy ? AHCI_HANDOFF_BUSY_MS : AHCI_HANDOFF_IDLE_MS);
        ok = ok && (f.bohc & BOHC_BOS) && f.now >= budget && f.now <= budget + 1;
    }
    printf("%s %-46s rc=%d busy=%d t=%ums\n", ok ? "ok  " : "FAIL", name, rc, busy,
           (unsigned)f.now);
    if (!ok)
        failures++;
}

int main(void) {
    /*            name                                      BB for  BOS after  rc busy */
    scenario("BIOS releases at once",                          0,      0,      0, 0);
    scenario("BIOS releases within 25 ms",                     0,      10,     0, 0);
    scenario("idle BIOS slow to release (300 ms)",             0,      300,    0, 0);
    scenario("busy BIOS, BOS held past 25 ms (1500 ms)",       1400,   1500,   0, 1);
    scenario("busy BIOS releases just inside 2 s (2020 ms)",   2000,   2020,   0, 1);
    scenario("busy BIOS never releases",                       NEVER,  NEVER,  -1, 1);
    scenario("idle BIOS never releases",                       0,      NEVER,  -1, 0);
    if (failures) {
        printf("[TEST-AHCI-HANDOFF] FAILED (%d)\n", failures);
        return 1;
    }
    printf("[TEST-AHCI-HANDOFF] passed\n");
    return 0;
}
