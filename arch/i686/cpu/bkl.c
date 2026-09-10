#include "percpu.h"
#include "apic.h"
#include "spinlock.h"
#include <stdint.h>

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

/*
 * The lock is inactive until the LAPIC is enabled (single-CPU early boot, before
 * apic_init).  After that it is active on every CPU — uncontended on a single
 * CPU, real on SMP.
 *
 * Both halves run with interrupts off, and that is not an optimisation: the
 * lock word and this CPU's depth are two separate objects, and the moment
 * between changing one and the other is a hole an interrupt on THIS CPU can
 * fall into.  bkl_enter takes the lock while the depth still reads 0, so an
 * interrupt landing in those few instructions runs its own bkl_enter, sees
 * depth 0, finds the lock held by itself, and spins for it forever with
 * interrupts already off; the interrupted half can never run the increment
 * that would have told it the lock was its own.  bkl_leave has the mirror
 * hole between the decrement to 0 and the unlock.
 *
 * Interrupts are not reliably off here.  CPU exceptions use TRAP gates, which
 * preserve IF, so a page fault taken from user code enters the stub with
 * interrupts ENABLED and runs straight into bkl_enter's hole.  And every
 * syscall that slept returns with IF set — sleep_on() and yield() sti after
 * the switch back — so the stub reaches bkl_leave's hole with interrupts on
 * even though int 0x80 is an interrupt gate.  Between them that is tens of
 * thousands of exposures per Firefox startup, which is what turned a
 * few-instruction window into a hang about one boot in sixty: silent, because
 * the spinner holds interrupts off forever and the 8259 keeps IRQ0 in service
 * with no EOI ever sent.
 *
 * Saving and restoring the caller's IF (rather than a bare cli/sti) keeps the
 * exception path's interrupts-enabled behaviour intact everywhere else.
 */
void bkl_enter(void) {
    if (!apic_available()) return;
    uint32_t fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    struct cpu *c = &cpus[this_cpu_id()];
    if (c->bkl_depth == 0) {
        /* Spin for the lock, but keep servicing TLB-shootdown requests while we
         * wait: a CPU spinning here has interrupts off, so it can't take the
         * shootdown IPI — without this it could never flush and the sender
         * would wait forever (deadlock). */
        while (!spin_trylock(&g_bkl)) {
            tlb_serve_pending();
            __asm__ volatile("pause");
        }
    }
    c->bkl_depth++;
    if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
}

void bkl_leave(void) {
    if (!apic_available()) return;
    uint32_t fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    struct cpu *c = &cpus[this_cpu_id()];
    if (c->bkl_depth > 0) {                 /* never underflow */
        c->bkl_depth--;
        if (c->bkl_depth == 0) spin_unlock(&g_bkl);
    }
    if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
}

/* Diagnostic: the lock word and this CPU's nesting depth.  A wedge with the
 * lock held and the depth at 0 is a CPU deadlocked against itself. */
void bkl_state(int *locked, int *depth) {
    if (locked) *locked = g_bkl.locked ? 1 : 0;
    if (depth)  *depth  = apic_available() ? cpus[this_cpu_id()].bkl_depth : 0;
}
