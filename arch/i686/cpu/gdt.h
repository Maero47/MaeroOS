#pragma once
#include <stdint.h>

/* GDT selector values (index * 8, with RPL in low 2 bits) */
#define SEG_NULL        0x00
#define SEG_KERNEL_CODE 0x08    /* Ring 0, executable */
#define SEG_KERNEL_DATA 0x10    /* Ring 0, data */
#define SEG_USER_CODE   0x1B    /* Ring 3, executable  (0x18 | RPL=3) */
#define SEG_USER_DATA   0x23    /* Ring 3, data        (0x20 | RPL=3) */
#define SEG_TSS         0x28    /* TSS descriptor */

void gdt_init(void);     /* BSP: build + load this CPU's GDT */
void gdt_init_ap(void);  /* AP:  build + load this CPU's GDT (per-CPU TSS/TLS) */

/* Re-base the user TLS segment (GDT entry 6, selector 0x33) */
void gdt_set_tls(uint32_t base);
uint32_t gdt_get_tls(void);   /* DEBUG: read back GDT entry 6 base */
