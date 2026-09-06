#include "panic.h"
#include "../drivers/serial.h"
#include <registers.h>
#include <stdint.h>

/* Forward declaration: printk defined in kernel/printk.c (not yet compiled
 * at Milestone 3, so we use serial directly) */
static void print_hex(uint32_t v) {
    serial_puts("0x");
    serial_write_hex(v);
}

void panic(const char *msg, registers_t *regs) {
    __asm__ volatile("cli");

    serial_puts("\r\n=== KERNEL PANIC ===\r\n");
    serial_puts(msg);
    serial_puts("\r\n");

    if (regs) {
        serial_puts("EIP="); print_hex(regs->eip);
        serial_puts("  CS=");  print_hex(regs->cs);
        serial_puts("  EFLAGS="); print_hex(regs->eflags); serial_puts("\r\n");
        serial_puts("EAX="); print_hex(regs->eax);
        serial_puts("  EBX="); print_hex(regs->ebx);
        serial_puts("  ECX="); print_hex(regs->ecx);
        serial_puts("  EDX="); print_hex(regs->edx); serial_puts("\r\n");
        serial_puts("ESI="); print_hex(regs->esi);
        serial_puts("  EDI="); print_hex(regs->edi);
        serial_puts("  EBP="); print_hex(regs->ebp);
        serial_puts("  ESP="); print_hex(regs->oesp);  serial_puts("\r\n");
        serial_puts("int_no="); print_hex(regs->int_no);
        serial_puts("  err_code="); print_hex(regs->err_code); serial_puts("\r\n");

        /* Walk the EBP chain for a stack trace */
        serial_puts("Stack trace:\r\n");
        uint32_t *ebp = (uint32_t *)regs->ebp;
        for (int i = 0; i < 10; i++) {
            /* Validate: EBP must be a kernel-space, dword-aligned address */
            if (!ebp || (uint32_t)ebp < 0xC0000000 || ((uint32_t)ebp & 3))
                break;
            serial_puts("  #");
            serial_putc((char)('0' + i));
            serial_puts(": ");
            print_hex(ebp[1]);      /* Return address at [EBP+4] */
            serial_puts("\r\n");
            ebp = (uint32_t *)ebp[0];   /* Previous frame pointer at [EBP] */
        }
    }

    serial_puts("System halted.\r\n");
    for (;;)
        __asm__ volatile("hlt");
}
