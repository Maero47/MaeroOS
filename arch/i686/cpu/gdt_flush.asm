; gdt_flush.asm — load a new GDT and reload all segment registers
;
; void gdt_flush(gdt_ptr_t *ptr);
;
; After lgdt, CS still holds the old (GRUB's) code selector.  We must do a
; far jump to reload CS with our kernel code selector (0x08).  All other
; segment registers (DS, ES, FS, GS, SS) are reloaded with 0x10 (kernel data).

section .text
global gdt_flush

gdt_flush:
    mov eax, [esp+4]        ; eax = pointer to gdt_ptr_t
    lgdt [eax]              ; load the new GDT

    ; Reload data segments
    mov ax, 0x10            ; kernel data selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Far jump to reload CS with kernel code selector (0x08)
    jmp 0x08:.flush
.flush:
    ret
