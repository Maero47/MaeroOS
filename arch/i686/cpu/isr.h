#pragma once
#include <registers.h>

/* Exception ISR stubs (0-31) + syscall (128) — defined in isr.asm */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);
extern void isr128(void); /* int 0x80 syscall */

/* Hardware IRQ stubs (0-15 → vectors 32-47) — defined in isr.asm */
extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

/* trapret: restores trapframe and executes iret (used by new processes) */
extern void trapret(void);

typedef void (*isr_handler_t)(registers_t *);

/* Register a C handler for an exception vector (0-31) */
void isr_install_handler(uint8_t vector, isr_handler_t handler);

/* Register a C handler for a hardware IRQ (0-15) */
void irq_install_handler(uint8_t irq, isr_handler_t handler);

/* C-level exception dispatcher — called from isr_common_stub */
void isr_handler(registers_t *regs);

/* C-level IRQ dispatcher — called from irq_common_stub */
void irq_handler(registers_t *regs);
