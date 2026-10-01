#pragma once
#include <stdint.h>
#include <kernel/multiboot.h>

/*
 * Boot information, whichever protocol loaded the kernel.
 *
 *   Multiboot 1  GRUB's `multiboot` command (make iso), QEMU -kernel
 *   Multiboot 2  Limine `protocol: multiboot2` (make limine-iso), BIOS or UEFI
 *
 * boot_info_init() turns either into the Multiboot 1 view the rest of the
 * kernel already reads (pmm_init, framebuffer_init, the initrd module): for
 * Multiboot 2 it rebuilds the memory map, module list, command line and
 * framebuffer fields from the tags into static storage, so nothing past
 * kernel_main needs to know which protocol was used.  Everything Multiboot 2
 * adds on top (the ACPI RSDP, whether the firmware was UEFI) is reached through
 * the accessors below.
 */

#define MULTIBOOT1_BOOTLOADER_MAGIC 0x2BADB002u
#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36D76289u

/* Called once from kernel_main, before pmm_init, with the registers _start
 * received (for Multiboot 2, EBX already points at boot.asm's copy of the
 * info block).  Returns the Multiboot 1 view (kernel virtual address), or
 * NULL when the magic is neither protocol's. */
multiboot_info_t *boot_info_init(uint32_t magic, uint32_t info_phys);

/* 1 or 2: which Multiboot protocol booted us (0 before boot_info_init). */
int boot_info_protocol(void);

/* Non-zero when the loader reported UEFI firmware (an EFI system table or an
 * EFI memory map tag).  Legacy BIOS areas (EBDA, 0xE0000-0xFFFFF) need not
 * exist then: ACPI must come from boot_info_rsdp(), not a BIOS-area scan. */
int boot_info_is_efi(void);

/*
 * The ACPI RSDP the loader handed over (Multiboot 2 tags 14 "ACPI old RSDP"
 * and 15 "ACPI new RSDP"; the new one wins when both are present).  Returns a
 * kernel-virtual pointer to a COPY of the RSDP structure itself, not its
 * firmware address: Multiboot 2 passes the bytes, not a pointer.  The copy
 * lives in kernel .bss and stays valid forever.  *len (if non-NULL) gets its
 * length: 20 for an ACPI 1.0 RSDP (revision 0, RsdtAddress only), 36 for an
 * ACPI 2.0+ one (XsdtAddress valid).  The checksums were verified here.
 *
 * NULL when the loader passed none (every Multiboot 1 boot): an ACPI parser
 * should then fall back to scanning the EBDA and 0xE0000-0xFFFFF for
 * "RSD PTR " (BIOS machines only; see boot_info_is_efi()).
 */
const void *boot_info_rsdp(uint32_t *len);

/* Kernel command line, "" when the loader passed none. */
const char *boot_info_cmdline(void);

/* Boot loader name, "" when the loader passed none. */
const char *boot_info_loader_name(void);
