#include "percpu.h"
#include "apic.h"
#include "spinlock.h"

struct cpu cpus[MAX_CPUS];

static spinlock_t g_bkl = { 0 };

uint32_t this_cpu_id(void) {
    if (!apic_available()) return 0;
    uint32_t id = apic_id();
    return id < MAX_CPUS ? id : 0;
}

/* The lock is inactive until the LAPIC is enabled (single-CPU early boot, before
 * apic_init).  After that it is active on every CPU — uncontended on a single
 * CPU, real on SMP. */
void bkl_enter(void) {
    if (!apic_available()) return;
    struct cpu *c = &cpus[this_cpu_id()];
    if (c->bkl_depth == 0) {
        /* Spin for the lock, but keep servicing TLB-shootdown requests while we
         * wait: a CPU spinning here has interrupts off (trap gate), so it can't
         * take the shootdown IPI — without this it could never flush and the
         * sender would wait forever (deadlock). */
        while (!spin_trylock(&g_bkl)) {
            tlb_serve_pending();
            __asm__ volatile("pause");
        }
    }
    c->bkl_depth++;
}

void bkl_leave(void) {
    if (!apic_available()) return;
    struct cpu *c = &cpus[this_cpu_id()];
    if (c->bkl_depth <= 0) return;          /* never underflow */
    c->bkl_depth--;
    if (c->bkl_depth == 0) spin_unlock(&g_bkl);
}
