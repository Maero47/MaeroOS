ENTRY(_start)
OUTPUT_FORMAT(elf32-i386)

SECTIONS {
    . = 0x08048000;

    .text : { *(.text .text.*) }

    .rodata : { *(.rodata .rodata.*) }

    .data : { *(.data .data.*) }

    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    }
}
