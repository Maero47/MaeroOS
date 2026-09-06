; AP (application processor) trampoline.
;
; Copied verbatim to physical 0x8000 and entered by each AP in 16-bit real mode
; via a STARTUP IPI (SIPI vector 0x08 → CS=0x0800, IP=0 → linear 0x8000).
; Brings the AP from real mode up to 32-bit, paging-enabled, higher-half C.
;
; The code is POSITION-DEPENDENT on running at TRAMP_BASE (0x8000): all absolute
; references are computed as TRAMP_BASE + (label - ap_trampoline_start).  The BSP
; (smp.c) copies [ap_trampoline_start, ap_trampoline_end) to 0x8000, patches the
; CR3 / stack / entry words, identity-maps 0x8000 in the kernel page directory,
; then sends INIT-SIPI-SIPI.

TRAMP_BASE equ 0x8000

section .text
global ap_trampoline_start
global ap_trampoline_end
global ap_tramp_cr3
global ap_tramp_stack
global ap_tramp_entry

bits 16
ap_trampoline_start:
    cli
    cld
    mov ax, cs              ; SIPI sets CS=0x0800; mirror into the data segs
    mov ds, ax
    mov es, ax
    mov ss, ax
    ; load our temporary 32-bit GDT (offset relative to the segment base)
    o32 lgdt [ap_gdtr - ap_trampoline_start]
    mov eax, cr0
    or  eax, 1             ; CR0.PE
    mov cr0, eax
    ; far jump to 32-bit code at its absolute linear address
    jmp dword 0x08:(TRAMP_BASE + (ap_pm32 - ap_trampoline_start))

bits 32
ap_pm32:
    mov ax, 0x10           ; flat 32-bit data selector
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    ; CR3 = kernel page directory (patched by the BSP)
    mov eax, [TRAMP_BASE + (ap_tramp_cr3 - ap_trampoline_start)]
    mov cr3, eax
    ; enable paging + write-protect (match the BSP's CR0)
    mov eax, cr0
    or  eax, 0x80010000    ; CR0.PG | CR0.WP
    mov cr0, eax
    ; paging is on; 0x8000 stays valid (BSP identity-maps it) and the kernel
    ; higher half is mapped, so we can use a virtual stack + call C.
    mov esp, [TRAMP_BASE + (ap_tramp_stack - ap_trampoline_start)]
    mov ebp, esp
    mov eax, [TRAMP_BASE + (ap_tramp_entry - ap_trampoline_start)]
    call eax               ; ap_entry() — should not return
.hang:
    cli
    hlt
    jmp .hang

align 16
ap_gdt:
    dq 0x0000000000000000  ; 0x00 null
    dq 0x00CF9A000000FFFF  ; 0x08 32-bit code, base 0, limit 4G
    dq 0x00CF92000000FFFF  ; 0x10 32-bit data, base 0, limit 4G
ap_gdtr:
    dw (ap_gdtr - ap_gdt) - 1
    dd TRAMP_BASE + (ap_gdt - ap_trampoline_start)

align 4
ap_tramp_cr3:   dd 0       ; patched: kernel pgdir physical address
ap_tramp_stack: dd 0       ; patched: per-CPU kernel stack top (virtual)
ap_tramp_entry: dd 0       ; patched: ap_entry() virtual address
ap_trampoline_end:
