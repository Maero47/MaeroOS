; boot.asm — Higher-half multiboot bootstrap for i686 kernel
;
; GRUB / QEMU's built-in multiboot loader loads this kernel into physical
; memory, switches to 32-bit protected mode, and jumps to _start with:
;   EAX = 0x2BADB002  (multiboot magic)
;   EBX = physical address of multiboot_info struct
;   Paging OFF, interrupts OFF, no defined stack.
;
; Strategy
; ─────────
; 1. _start lives in .boot.text (placed by linker at physical 0x00100xxx).
;    This gives the ELF e_entry field a physical address that GRUB/QEMU can
;    actually jump to without paging.
;
; 2. The boot page directory, page table, and initial stack live in the main
;    .bss section (linked at virtual 0xC01xxxxx, loaded at physical 0x001xxxxx).
;    Before paging, we access them as (label - KERNEL_VMA) to get physical addr.
;    After paging is enabled and we jump to the higher-half virtual address,
;    we use the labels directly (virtual == physical + KERNEL_VMA via PDE[768]).
;
; 3. We PRESERVE EAX and EBX throughout — all setup uses EDI, ESI, ECX, EDX.

KERNEL_VMA equ 0xC0000000

; ─── Multiboot 1 header ─────────────────────────────────────────────────────
; Must appear within the first 8 KiB of the kernel image.
section .multiboot
align 4
multiboot_header:
    dd 0x1BADB002                       ; magic
    dd 0x10007                          ; flags: ALIGN | MEMINFO | VIDEO_MODE | ADDR
    dd -(0x1BADB002 + 0x10007)          ; checksum (truncated to 32 bits)
    dd multiboot_header                 ; header_addr
    dd 0x00100000                       ; load_addr
    dd 0                                ; load_end_addr: load whole file
    dd _kernel_phys_end + 0x10000       ; bss_end_addr: leave room for PMM bitmap
    dd _start                           ; entry_addr
    dd 0                                ; mode_type: linear graphics
    dd 0                                ; width:  0 = no preference (GRUB's
    dd 0                                ; height: gfxpayload picks the mode,
    dd 32                               ; depth   so the user can choose it)

; ─── Paging structures + initial stack (in main .bss, linked at 0xC01xxxxx) ─
; Declared here (in boot.asm) so they are the very first things in .bss and
; remain 4096-byte aligned.
section .bss
global boot_page_directory
global boot_page_table1
align 4096
boot_page_directory: resb 4096          ; 1024 PDEs
boot_page_table1:    resb 4096          ; 1024 PTEs (first 4 MiB)
boot_page_table2:    resb 4096          ; 1024 PTEs (4-8 MiB; kernel > 4 MiB now)
boot_page_table3:    resb 4096          ; 1024 PTEs (8-12 MiB; headroom for big .bss)
align 16
stack_bottom: resb 16384               ; 16 KiB kernel stack
stack_top:

; ─── Entry point — lives at a physical address so GRUB can jump to it ────────
section .boot.text progbits alloc exec
global _start
extern kernel_main
extern _kernel_phys_end
extern _bss_start
extern _bss_end

_start:
    ; EAX = 0x2BADB002, EBX = multiboot_info ptr.  Preserve both.

    ; GRUB's address-header loading path does not reliably clear NOBITS.
    ; Clear the full kernel .bss before touching C globals.
    mov edi, (_bss_start - KERNEL_VMA)
    mov ecx, (_bss_end - KERNEL_VMA)
    xor edx, edx
.zero_bss:
    cmp edi, ecx
    jae .bss_done
    mov byte [edi], dl
    inc edi
    jmp .zero_bss
.bss_done:

    ; ── Zero boot_page_directory (physical address before paging) ────────────
    ; (label - KERNEL_VMA) converts virtual link address → physical load address
    mov edi, (boot_page_directory - KERNEL_VMA)
    mov ecx, 1024
    xor edx, edx
.zero_pd:
    mov dword [edi], edx
    add edi, 4
    dec ecx
    jnz .zero_pd

    ; ── Fill boot_page_table1 + 2 + 3: identity PTEs for the first 12 MiB ────
    ; (the kernel image incl. BSS exceeds 8 MiB; PT3 covers 8-12 MiB)
    mov edi, (boot_page_table1 - KERNEL_VMA)
    mov esi, 0                          ; physical frame counter (0, 4096, ...)
    mov ecx, 3072                       ; 3 page tables, contiguous in .bss
.fill_loop:
    mov edx, esi
    or  edx, 0x003                      ; Present + Read/Write
    mov [edi], edx
    add esi, 4096
    add edi, 4
    dec ecx
    jnz .fill_loop

    ; Also map VGA text buffer (physical 0xB8000) at PTE index 184
    mov dword [(boot_page_table1 - KERNEL_VMA) + 184*4], (0x000B8000 | 0x003)

    ; ── Install page table into page directory ────────────────────────────────
    ; PDE[0]    = identity map  (keeps execution valid right after CR0.PG=1)
    mov dword [(boot_page_directory - KERNEL_VMA) + 0*4],   \
              ((boot_page_table1 - KERNEL_VMA) + 0x003)
    ; PDE[768]  = higher-half map  (0xC0000000 >> 22 = 768)
    mov dword [(boot_page_directory - KERNEL_VMA) + 768*4], \
              ((boot_page_table1 - KERNEL_VMA) + 0x003)
    ; PDE[1] + PDE[769]: second 4 MiB (identity + higher-half)
    mov dword [(boot_page_directory - KERNEL_VMA) + 1*4],   \
              ((boot_page_table2 - KERNEL_VMA) + 0x003)
    mov dword [(boot_page_directory - KERNEL_VMA) + 769*4], \
              ((boot_page_table2 - KERNEL_VMA) + 0x003)
    ; PDE[2] + PDE[770]: third 4 MiB (identity + higher-half), 8-12 MiB
    mov dword [(boot_page_directory - KERNEL_VMA) + 2*4],   \
              ((boot_page_table3 - KERNEL_VMA) + 0x003)
    mov dword [(boot_page_directory - KERNEL_VMA) + 770*4], \
              ((boot_page_table3 - KERNEL_VMA) + 0x003)
    ; PDE[1023] = recursive self-mapping  (for get_pte/get_pde)
    mov dword [(boot_page_directory - KERNEL_VMA) + 1023*4], \
              ((boot_page_directory - KERNEL_VMA) + 0x003)

    ; ── Enable paging ─────────────────────────────────────────────────────────
    mov ecx, (boot_page_directory - KERNEL_VMA)
    mov cr3, ecx
    mov ecx, cr0
    or  ecx, 0x80010000                 ; CR0.PG (bit 31) + CR0.WP (bit 16)
    mov cr0, ecx

    ; Far jump to higher-half virtual address — transfers execution to 0xC0xxxxx.
    lea ecx, [higher_half]
    jmp ecx

; ─── Now executing at virtual address ────────────────────────────────────────
; From here, all virtual addresses work normally via PDE[768].
higher_half:
    ; Remove identity maps (PDE[0], PDE[1]).  Higher-half only from here.
    mov dword [(boot_page_directory) + 0*4], 0
    mov dword [(boot_page_directory) + 1*4], 0
    invlpg [0]

    ; Set up the kernel stack (stack_top is the virtual .bss symbol, now valid).
    mov esp, stack_top

    ; Call kernel_main(u32 mb_magic, u32 mb_phys)
    push ebx                            ; arg2: multiboot_info physical address
    push eax                            ; arg1: multiboot magic
    call kernel_main

    ; kernel_main must not return.
    cli
.hang:
    hlt
    jmp .hang
