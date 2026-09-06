#ifndef ARCH_I686_SMP_H
#define ARCH_I686_SMP_H

#include <stdint.h>

/* Bring up the application processors (one INIT-SIPI-SIPI each).  Call once on
 * the BSP after apic_init(), heap, and the PIT are up (needs timed delays).
 * Returns the total number of online CPUs (BSP + APs that signalled alive). */
uint32_t smp_boot_aps(void);

/* Number of online CPUs (1 until smp_boot_aps reports more). */
uint32_t smp_cpu_count(void);

/* AP C entry point (called from the trampoline; not for direct use). */
void ap_entry(void);

/* Set by the BSP once the process table is ready and it is about to enter the
 * scheduler; APs spin on it before scanning ptable.  See ap_entry(). */
extern volatile int g_smp_go;

#endif /* ARCH_I686_SMP_H */
