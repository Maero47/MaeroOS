#pragma once
#include <stdint.h>

/*
 * AHCI BIOS/OS handoff (AHCI 1.3.1 section 10.6.3), kept apart from the
 * register window so the host test tools/test_ahci_handoff.c can run it
 * against a simulated firmware.  The caller only uses it when CAP2.BOH is set.
 *
 * The sequence the spec gives the OS:
 *   1. set BOHC.OOS (OS ownership requested);
 *   2. wait 25 ms for the BIOS to release BOHC.BOS;
 *   3. if BOHC.BB (BIOS busy) is set then, the BIOS is finishing outstanding
 *      commands: wait at least 2 s more for BOS to clear;
 *   4. the HBA is the OS's once BOS reads 0.
 * The HBA reset must not be issued while BOS is still set.  So BOS is polled
 * in every case, with a bound so firmware that never lets go cannot hang
 * boot: the 25 ms, then up to 2 s more when BB was seen, else up to 1 s
 * (lenient towards firmware that is slow without saying it is busy).
 *
 * Time is counted in sleep_ms(1) steps, so the elapsed time is at least the
 * count whatever a register read costs, and the test can drive a fake clock.
 */

#define BOHC_BOS      (1u << 0)     /* BIOS owned semaphore */
#define BOHC_OOS      (1u << 1)     /* OS owned semaphore */
#define BOHC_BB       (1u << 4)     /* BIOS busy */

#define AHCI_HANDOFF_SETTLE_MS  25u
#define AHCI_HANDOFF_BUSY_MS    2000u
#define AHCI_HANDOFF_IDLE_MS    1000u

typedef struct ahci_bohc_io {
    uint32_t (*read)(struct ahci_bohc_io *io);
    void     (*write)(struct ahci_bohc_io *io, uint32_t v);
    void     (*sleep_ms)(struct ahci_bohc_io *io, uint32_t ms);
} ahci_bohc_io_t;

/* 0 when the OS owns the HBA (BOS clear), -1 when the BIOS still held it
 * when the time ran out.  *busy_seen reports whether BB was set at 25 ms. */
static inline int ahci_bios_handoff(ahci_bohc_io_t *io, int *busy_seen) {
    io->write(io, io->read(io) | BOHC_OOS);
    io->sleep_ms(io, AHCI_HANDOFF_SETTLE_MS);

    uint32_t bohc = io->read(io);
    int busy = (bohc & BOHC_BB) != 0;
    if (busy_seen)
        *busy_seen = busy;
    uint32_t budget = busy ? AHCI_HANDOFF_BUSY_MS : AHCI_HANDOFF_IDLE_MS;
    for (uint32_t waited = 0; bohc & BOHC_BOS; waited++) {
        if (waited >= budget)
            return -1;
        io->sleep_ms(io, 1);
        bohc = io->read(io);
    }
    return 0;
}
