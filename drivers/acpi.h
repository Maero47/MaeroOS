#pragma once
#include <stdint.h>

/*
 * ACPI: table discovery (RSDP/RSDT/XSDT, FADT, MADT, HPET) and the AML
 * namespace, through the vendored uACPI interpreter (third_party/uacpi, MIT).
 * This file is the kernel side: the uacpi_kernel_* host API, boot-time setup,
 * the SCI (power button) and the power-off / reboot paths used by reboot(2).
 */

/* Find the tables, load and initialise the namespace, hook the SCI.  Call
 * once, after the heap, PCI and the TSC are up and before init is launched
 * (it reserves the page tables of the ACPI mapping window, which every later
 * page directory then inherits). */
void acpi_init(void);

/* 1 once the namespace is loaded and the hardware is in ACPI mode. */
int acpi_available(void);

/* Number of enabled processors the MADT lists (0: no MADT). */
uint32_t acpi_madt_cpu_count(void);

/* Enter S5 (\_PTS, \_S5 SLP_TYPa/b, PM1 control) — falls back to the
 * well-known emulator ports when ACPI is unavailable or S5 fails. */
void acpi_poweroff(void) __attribute__((noreturn));

/* FADT reset register, then 0xCF9, the 8042 and a triple fault. */
void acpi_reboot(void) __attribute__((noreturn));
