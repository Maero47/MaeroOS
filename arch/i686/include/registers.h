#pragma once
#include <stdint.h>

/*
 * registers_t — snapshot of all CPU registers at the time of an interrupt.
 *
 * This struct is built on the kernel stack by isr_common_stub (in isr.asm).
 * The layout MUST match the push order exactly:
 *
 *   [high address — first pushed]
 *   gs, fs, es, ds          ← pushed manually by the stub
 *   edi,esi,ebp,oesp,ebx,edx,ecx,eax  ← pushed by PUSHA (in that order)
 *   int_no, err_code        ← pushed by the ISR macro (dummy or real)
 *   eip, cs, eflags         ← pushed automatically by the CPU on interrupt
 *   useresp, ss             ← pushed by CPU only on privilege-level change
 *   [low address — last pushed / top of stack when C handler is called]
 */
typedef struct registers {
    /* Segment registers pushed manually */
    uint32_t gs, fs, es, ds;

    /* General-purpose registers (PUSHA order: EDI first, EAX last) */
    uint32_t edi, esi, ebp, oesp, ebx, edx, ecx, eax;

    /* Interrupt metadata pushed by the ISR stub */
    uint32_t int_no;    /* interrupt / exception vector number */
    uint32_t err_code;  /* error code (0 for exceptions that don't push one) */

    /* Pushed automatically by the CPU */
    uint32_t eip;
    uint32_t cs;
    uint32_t eflags;

    /* Pushed by CPU only on ring transition (ring 3 → ring 0) */
    uint32_t useresp;
    uint32_t ss;
} registers_t;
