#include "dfault.h"
#include "tss.h"
#include "percpu.h"
#include "../../../proc/process.h"
#include "../../../mm/kstack.h"
#include "../../../drivers/serial.h"
#include "../../../kernel/panic.h"
#include <kernel/config.h>
#include <stdint.h>

/*
 * Kernel stack overflow and double-fault reporting.
 *
 * Everything here writes to the serial port directly.  printk takes a lock,
 * and the overflow may well have happened inside printk with that lock held;
 * the port itself needs nothing but the CPU we are on.
 */

static void put_dec(uint32_t v) {
    char buf[11];
    int i = 0;
    do { buf[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) serial_putc(buf[--i]);
}

static void put_hex(uint32_t v) {
    serial_puts("0x");
    serial_write_hex(v);
}

/* "cpu N pid P 'name' kstack [lo,hi)" for the thread running on this CPU. */
static void put_thread(uint32_t cpu) {
    struct proc *p = cpus[cpu].proc;
    serial_puts("cpu ");
    put_dec(cpu);
    if (!p) {
        serial_puts(" (scheduler / no thread)");
        return;
    }
    serial_puts(" pid ");
    put_dec((uint32_t)p->pid);
    serial_puts(" tgid ");
    put_dec((uint32_t)p->tgid);
    serial_puts(" '");
    for (int i = 0; i < 16 && p->name[i]; i++) serial_putc(p->name[i]);
    serial_puts("' kstack [");
    put_hex((uint32_t)(uintptr_t)p->kstack);
    serial_puts(",");
    put_hex((uint32_t)(uintptr_t)p->kstack + KSTACKSIZE);
    serial_puts(")");
}

void kstack_overflow_panic(const char *why, registers_t *regs, uint32_t addr) {
    __asm__ volatile("cli");
    /* The stack pointer at the moment of the fault: a same-privilege trap does
     * not push SS:ESP, so it is the address just above the eip/cs/eflags the
     * CPU pushed. */
    uint32_t esp = (uint32_t)(uintptr_t)&regs->useresp;
    serial_puts("\r\n[KSTACK] KERNEL STACK OVERFLOW: ");
    put_thread(this_cpu_id());
    serial_puts("\r\n[KSTACK]   eip=");
    put_hex(regs->eip);
    serial_puts(" esp=");
    put_hex(esp);
    serial_puts(" ebp=");
    put_hex(regs->ebp);
    serial_puts(" fault addr=");
    put_hex(addr);
    serial_puts(" (guard below stack ");
    put_hex(kstack_guard_stack(addr));
    serial_puts(")\r\n");
    panic(why, regs);
}

/*
 * Entered from double_fault_task (isr.asm) on this CPU's double-fault task.
 * The task switch saved the interrupted context in this CPU's main TSS.
 */
void double_fault_report(void) {
    serial_sync_begin();        /* nothing will drain a queue after this */
    uint32_t cpu = this_cpu_id();
    const tss_entry_t *t = tss_saved_state((int)cpu);
    uint32_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    /* A push that crossed into the guard page: CR2 (the #PF that could not be
     * delivered) or the saved ESP lands in a guard. */
    int overflow = kstack_guard_stack(cr2) || kstack_guard_stack(t->esp);

    serial_puts("\r\n[DF] DOUBLE FAULT");
    if (overflow) serial_puts(" - KERNEL STACK OVERFLOW");
    serial_puts(": ");
    put_thread(cpu);
    serial_puts("\r\n[DF]   eip=");
    put_hex(t->eip);
    serial_puts(" esp=");
    put_hex(t->esp);
    serial_puts(" ebp=");
    put_hex(t->ebp);
    serial_puts(" cr2=");
    put_hex(cr2);
    serial_puts(" cs=");
    put_hex(t->cs);
    serial_puts(" eflags=");
    put_hex(t->eflags);
    serial_puts("\r\n");

    /* Hand the saved context to panic in its usual shape, which adds the
     * register dump and a frame-pointer backtrace. */
    registers_t r;
    __builtin_memset(&r, 0, sizeof(r));
    r.gs = t->gs; r.fs = t->fs; r.es = t->es; r.ds = t->ds;
    r.edi = t->edi; r.esi = t->esi; r.ebp = t->ebp; r.oesp = t->esp;
    r.ebx = t->ebx; r.edx = t->edx; r.ecx = t->ecx; r.eax = t->eax;
    r.int_no = 8;
    r.eip = t->eip; r.cs = t->cs; r.eflags = t->eflags;
    r.useresp = t->esp; r.ss = t->ss;
    panic(overflow ? "Double fault: kernel stack overflow" : "Double fault", &r);
}
