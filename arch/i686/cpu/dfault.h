#pragma once
#include <stdint.h>
#include <registers.h>

/* Report a kernel-mode fault on a kernel stack's guard (`addr`) as a stack
 * overflow — which thread, eip, esp — and halt.  Serial only: it must work with
 * whatever lock the overflowing code was holding. */
void kstack_overflow_panic(const char *why, registers_t *regs, uint32_t addr)
     __attribute__((noreturn));

/* C half of the double-fault task (isr.asm double_fault_task). */
void double_fault_report(void) __attribute__((noreturn));
