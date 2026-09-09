#include "percpu.h"
#include "apic.h"
#include "spinlock.h"

struct cpu cpus[MAX_CPUS];

static spinlock_t g_bkl = { 0 };

/*
 * `current_proc` is `cpus[this_cpu_id()].proc`, so this function is on the path
 * of every single reference to the running thread -- and it read the Local APIC
 * ID register to answer.  That register lives in the LAPIC's MMIO page, and a
 * guest access to it exits to the hypervisor: measured on this kernel, one
 * `current_proc` dereference cost about 1.6 us.  `preempt_disable()` names it
 * twice and `preempt_enable()` three times, so guarding one block-cache lookup
 * cost 8 us against 0.2 us of actual work, and the exits added up to roughly a
 * third of a Firefox startup.
 *
 * While only one CPU is executing, the answer cannot change, so it is read once
 * and remembered.  The moment an AP is about to be started, smp_percpu_go_multi
 * turns the cache off and every call reads the register again, exactly as
 * before -- the fast path is not an assumption about the machine, it is a fact
 * about how many CPUs are running.
 */
static uint32_t g_solo_id;
static int      g_solo_valid;
static volatile int g_multi_cpu;

void smp_percpu_go_multi(void) { g_multi_cpu = 1; }

uint32_t this_cpu_id(void) {
    if (!apic_available()) return 0;
    if (!g_multi_cpu) {
        if (!g_solo_valid) {
            uint32_t boot = apic_id();
            g_solo_id    = boot < MAX_CPUS ? boot : 0;
            g_solo_valid = 1;
        }
        return g_solo_id;
    }
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
