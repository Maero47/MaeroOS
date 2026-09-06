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

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
    }
}
