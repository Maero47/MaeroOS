#include "printk.h"
#include "klog.h"
#include "../drivers/serial.h"
#include "../drivers/vga.h"
#include "../lib/printf.h"
#include "../arch/i686/cpu/percpu.h"
#include "../arch/i686/cpu/apic.h"
#include "../arch/i686/cpu/spinlock.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Console lock (docs/smp-plan.md stage 1g).  printk runs on every CPU, from
 * interrupt handlers and from threads, with or without the BKL (the APs print
 * during bring-up without it, the lock torture's threads run outside it), so
 * lines used to interleave character by character — once mid-way through the
 * getty prompt a test harness was waiting for — and the klog ring's index
 * update raced.  One message is now emitted whole: klog, serial and VGA under
 * this lock, interrupts off.
 *
 * Never a deadlock source:
 *  - nothing taken under it (the UART and VGA are plain port/memory writes);
 *  - a CPU that already holds it (an NMI dump or a fault inside printk on the
 *    same CPU) writes through without it;
 *  - a waiter serves TLB shootdowns while it spins, like every IF=0 spin;
 *  - after ~1 s of waiting (no message takes that long: 1 KiB at 115200 baud
 *    is 90 ms) a waiter goes ahead regardless, so a panic still prints when the
 *    holder was stopped by smp_stop_others() or wedged.
 */
static spinlock_t con_lock;
static volatile int32_t con_owner = -1;

static int con_enter(uint32_t *fl) {
    __asm__ volatile("pushf; pop %0; cli" : "=r"(*fl) :: "memory");
    if (!apic_available()) return 0;            /* early boot: one CPU */
    int32_t me = (int32_t)this_cpu_id();
    if (con_owner == me) return 0;              /* nested on this CPU */
    for (uint32_t n = 0; !spin_trylock(&con_lock); n++) {
        if (n > 20000000U) return 0;            /* holder is gone: write anyway */
        do {
            tlb_serve_pending();
            __asm__ volatile("pause");
        } while (con_lock.locked && ++n <= 20000000U);
    }
    con_owner = me;
    return 1;
}

static void con_leave(uint32_t fl, int locked) {
    if (locked) {
        con_owner = -1;
        spin_unlock(&con_lock);
    }
    if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
}

void vprintk(const char *fmt, va_list args) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    uint32_t fl;
    int locked = con_enter(&fl);
    serial_puts(buf);
    vga_puts(buf);
    klog_write(buf, (size_t)n);
    con_leave(fl, locked);
}

void printk(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintk(fmt, args);
    va_end(args);
}

void printk_klog(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    uint32_t fl;
    int locked = con_enter(&fl);
    klog_write(buf, (size_t)n);
    con_leave(fl, locked);
}
