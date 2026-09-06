#include "isr.h"
#include <registers.h>
#include <stddef.h>

/* Forward declaration — defined in kernel/panic.c */
extern void panic(const char *msg, registers_t *regs) __attribute__((noreturn));

/* Forward declaration — defined in proc/syscall.c */
extern void syscall_dispatch(registers_t *regs);

/* User-mode fault containment: a CPU exception (GP fault, invalid opcode, divide,
 * etc.) raised by ring-3 code must terminate the faulting PROCESS (POSIX signal),
 * not panic the whole kernel — exactly as Linux delivers SIGSEGV/SIGILL/SIGFPE.
 * Only KERNEL-mode (ring 0) exceptions are truly fatal.  Implemented in
 * proc/ (which has the process table + signal machinery); we just call it. */
extern int user_fault_signal(registers_t *regs, int sig);  /* 1=handled, 0=panic */
#define _SIGILL  4
#define _SIGFPE  8
#define _SIGSEGV 11
#define _SIGBUS  7

/* Handler table for exceptions 0-31 (filled by isr_install_handler) */
static isr_handler_t exception_handlers[32];

void isr_install_handler(uint8_t vector, isr_handler_t handler) {
    if (vector < 32)
        exception_handlers[vector] = handler;
}

/* Human-readable names for the first 32 CPU exceptions */
static const char *exception_names[] = {
    "Divide-by-zero",           "Debug",
    "Non-maskable interrupt",   "Breakpoint",
    "Overflow",                 "Bound range exceeded",
    "Invalid opcode",           "Device not available",
    "Double fault",             "Coprocessor segment overrun",
    "Invalid TSS",              "Segment not present",
    "Stack-segment fault",      "General protection fault",
    "Page fault",               "(reserved)",
    "x87 FPU error",            "Alignment check",
    "Machine check",            "SIMD FP exception",
    "Virtualization exception", "Control protection exception",
    "(reserved)", "(reserved)", "(reserved)", "(reserved)",
    "(reserved)", "(reserved)", "(reserved)",
    "Hypervisor injection",     "VMM communication",
    "Security exception"
};

/*
 * isr_handler — C entry point for CPU exceptions (vectors 0-31 + 128).
 * Called from isr_common_stub in isr.asm.
 */
void isr_handler(registers_t *regs) {
    /* Syscall (int 0x80) */
    if (regs->int_no == 128) {
        syscall_dispatch(regs);
        return;
    }

    /* Dispatch to a registered handler if present (e.g. #PF → page_fault_handler) */
    if (regs->int_no < 32 && exception_handlers[regs->int_no]) {
        exception_handlers[regs->int_no](regs);
        return;
    }

    /* An unhandled CPU exception from RING 3 (CS RPL == 3) must kill the faulting
     * PROCESS via a POSIX signal, not panic the kernel — Linux behaviour.  Map the
     * exception to its signal; user_fault_signal() delivers it (and returns 1) if
     * there is a current user process, else returns 0 so we still panic. */
    if (regs->int_no < 32 && (regs->cs & 3) == 3) {
        int sig;
        switch (regs->int_no) {
            case 0:  case 16: case 19: sig = _SIGFPE;  break; /* div0, x87, SIMD */
            case 6:                    sig = _SIGILL;  break; /* invalid opcode  */
            case 17:                   sig = _SIGBUS;  break; /* alignment check */
            default:                   sig = _SIGSEGV; break; /* #GP, #SS, #NP … */
        }
        if (user_fault_signal(regs, sig))
            return;
    }

    /* Kernel-mode (or pre-process) unhandled exception → fatal */
    const char *name = (regs->int_no < 32)
                       ? exception_names[regs->int_no]
                       : "Unknown exception";
    panic(name, regs);
}
