ENTRY(_start)
OUTPUT_FORMAT(elf32-i386)
KERNEL_VMA = 0xC0000000;

SECTIONS {
    . = 0x00100000;
    _kernel_phys_start = .;

    /* Multiboot header — must be within first 8 KiB of the image */
    .multiboot : {
        *(.multiboot)
    }

    /*
     * Boot code and data run at physical addresses (before paging is on).
     * They are placed here so that _start has a physical address and the ELF
     * entry point (e_entry) can be resolved to a physical address that GRUB
     * and QEMU's multiboot loader can actually jump to.
     */
    /* Pre-paging boot code — placed at physical addresses */
    .boot.text : {
        *(.boot.text)
    }
    .boot.data : {
        *(.boot.data)
    }

    . += KERNEL_VMA;

    .text ALIGN(4096) : AT(ADDR(.text) - KERNEL_VMA) {
        _text_start = .;
        *(.text .text.*)
        _text_end = .;
    }

    .rodata ALIGN(4096) : AT(ADDR(.rodata) - KERNEL_VMA) {
        _rodata_start = .;
        *(.rodata .rodata.*)
        /* Exception table (extable): (faulting-insn-addr, fixup-addr) pairs
         * emitted by copy_from/to_user.  The page-fault handler searches this to
         * recover from a fault during a user-memory access (return -EFAULT)
         * instead of panicking — Linux-style. */
        . = ALIGN(4);
        __start___ex_table = .;
        *(__ex_table)
        __stop___ex_table = .;
        _rodata_end = .;
    }

    .data ALIGN(4096) : AT(ADDR(.data) - KERNEL_VMA) {
        _data_start = .;
        *(.data .data.*)
        _data_end = .;
    }

    .bss ALIGN(4096) : AT(ADDR(.bss) - KERNEL_VMA) {
        _bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        _bss_end = .;
    }

    _kernel_end = .;
    _kernel_phys_end = _kernel_end - KERNEL_VMA;

    /* boot.asm maps only the first boot_mapped_bytes (12 MiB) before
     * paging_init; a .bss tail past it page-faults the first time early C
     * code touches it, before there is an IDT to report the fault.  Move big static tables to the heap, or add a boot
     * page table (BOOT_PAGE_TABLES in boot.asm). */
    ASSERT(_kernel_phys_end <= boot_mapped_bytes,
           "kernel image (_kernel_end) exceeds boot.asm's initial mapping (boot_mapped_bytes, 12 MiB): shrink .bss or raise BOOT_PAGE_TABLES")

    /* Limine's Multiboot 2 loader must find [1 MiB, bss_end_addr) free, and
     * bss_end_addr is _kernel_phys_end + 64 KiB (boot.asm's address tag).
     * OVMF x64 keeps memory reserved from 8 MiB up (its MEMFD area: SEC page
     * tables, lock box, work area), so a longer image does not boot on most
     * UEFI PCs ("could not find viable load address", make smoke-uefi). */
    ASSERT(_kernel_phys_end + 0x10000 <= 0x800000,
           "kernel image ends past 8 MiB - 64 KiB, where OVMF x64 reserves memory: Limine cannot load it on UEFI; move big static tables to the heap")

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
    }
}
