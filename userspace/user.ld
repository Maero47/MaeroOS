ENTRY(_start)
OUTPUT_FORMAT(elf32-i386)

/* Two PT_LOAD segments: text+rodata read/execute, data+bss read/write.  The
 * kernel maps each segment with its own p_flags (W^X), so .data must start on
 * a fresh page — a page shared with .rodata would have to be writable. */
PHDRS {
    text PT_LOAD FILEHDR PHDRS FLAGS(5);    /* PF_R | PF_X */
    data PT_LOAD FLAGS(6);                  /* PF_R | PF_W */
}

SECTIONS {
    . = 0x08048000 + SIZEOF_HEADERS;

    .text : { *(.text .text.*) } :text

    .rodata : { *(.rodata .rodata.*) } :text

    . = ALIGN(0x1000);

    .data : { *(.data .data.*) } :data

    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :data
}
