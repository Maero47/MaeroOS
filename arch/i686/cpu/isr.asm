; isr.asm — Interrupt / exception stub generators
;
; ISR_NOERRCODE n  — for vectors that do NOT push an error code (CPU or none)
; ISR_ERRCODE   n  — for vectors where the CPU automatically pushes an error code
;
; Both macros push the interrupt number and jump to isr_common_stub, which
; saves all registers and calls the C dispatcher isr_handler(registers_t *).
;
; The stack layout on entry to isr_handler matches registers_t exactly:
;
;   [high addr — pushed first]
;   gs, fs, es, ds           ← pushed manually
;   edi,esi,ebp,esp,ebx,edx,ecx,eax  ← PUSHA
;   int_no, err_code         ← pushed by macro / CPU
;   eip, cs, eflags          ← pushed by CPU on interrupt
;   useresp, ss              ← pushed by CPU on ring change only
;   [low addr]

section .text

; ─── Macro definitions ───────────────────────────────────────────────────────

%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push dword 0        ; Dummy error code (keeps stack layout uniform)
    push dword %1       ; Interrupt number
    jmp isr_common_stub
%endmacro

%macro ISR_ERRCODE 1
global isr%1
isr%1:
    ; CPU already pushed the error code
    push dword %1       ; Interrupt number
    jmp isr_common_stub
%endmacro

%macro IRQ 2
global irq%1
irq%1:
    push dword 0        ; Dummy error code
    push dword %2       ; Interrupt vector (32 + IRQ number)
    jmp irq_common_stub
%endmacro

; ─── Exception stubs (vectors 0-31) ─────────────────────────────────────────
; Exceptions that push an error code: 8, 10, 11, 12, 13, 14, 17, 21, 29, 30

ISR_NOERRCODE  0   ; #DE Divide-by-zero
ISR_NOERRCODE  1   ; #DB Debug
ISR_NOERRCODE  2   ; NMI
ISR_NOERRCODE  3   ; #BP Breakpoint
ISR_NOERRCODE  4   ; #OF Overflow
ISR_NOERRCODE  5   ; #BR Bound range exceeded
ISR_NOERRCODE  6   ; #UD Invalid opcode
ISR_NOERRCODE  7   ; #NM Device not available (no math coprocessor)
ISR_ERRCODE    8   ; #DF Double fault
ISR_NOERRCODE  9   ; Coprocessor segment overrun (legacy)
ISR_ERRCODE   10   ; #TS Invalid TSS
ISR_ERRCODE   11   ; #NP Segment not present
ISR_ERRCODE   12   ; #SS Stack-segment fault
ISR_ERRCODE   13   ; #GP General protection fault
ISR_ERRCODE   14   ; #PF Page fault
ISR_NOERRCODE 15   ; Reserved
ISR_NOERRCODE 16   ; #MF x87 FPU floating-point error
ISR_ERRCODE   17   ; #AC Alignment check
ISR_NOERRCODE 18   ; #MC Machine check
ISR_NOERRCODE 19   ; #XF SIMD floating-point exception
ISR_NOERRCODE 20   ; #VE Virtualization exception
ISR_ERRCODE   21   ; #CP Control protection exception
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29   ; #HV Hypervisor injection exception
ISR_ERRCODE   30   ; #VC VMM communication exception
ISR_NOERRCODE 31   ; #SX Security exception (reserved)

; ─── Hardware IRQ stubs (vectors 32-47) ─────────────────────────────────────

IRQ  0, 32   ; Timer (PIT)
IRQ  1, 33   ; PS/2 Keyboard
IRQ  2, 34   ; Cascade (used internally by slave PIC)
IRQ  3, 35   ; COM2
IRQ  4, 36   ; COM1
IRQ  5, 37   ; LPT2
IRQ  6, 38   ; Floppy disk
IRQ  7, 39   ; LPT1 / spurious
IRQ  8, 40   ; CMOS Real-time clock
IRQ  9, 41   ; Free / ACPI
IRQ 10, 42   ; Free
IRQ 11, 43   ; Free
IRQ 12, 44   ; PS/2 Mouse
IRQ 13, 45   ; FPU / coprocessor
IRQ 14, 46   ; ATA primary channel
IRQ 15, 47   ; ATA secondary / spurious

; ─── Syscall stub (int 0x80) ─────────────────────────────────────────────────
global isr128
isr128:
    push dword 0
    push dword 128
    jmp isr_common_stub

; ─── LAPIC timer stub (vector 0xF0) ──────────────────────────────────────────
; Per-CPU preemption clock for APs (the PIT only interrupts the BSP).  Routed
; through irq_common_stub; irq_handler special-cases int_no 0xF0 → scheduler_tick
; + LAPIC EOI (no PIC EOI).
global lapic_timer_isr
lapic_timer_isr:
    push dword 0
    push dword 0xF0
    jmp irq_common_stub

; ─── TLB shootdown IPI stub (vector 0xFD) ────────────────────────────────────
; Bare handler: flush this CPU's TLB + EOI.  Must NOT acquire the Big Kernel
; Lock — the sending CPU holds it, so taking it here would deadlock.  No
; scheduling, no user-pointer work; just a CR3 reload via the C helper.
global tlb_ipi_isr
extern tlb_ipi_handler
tlb_ipi_isr:
    pusha
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld
    call tlb_ipi_handler
    pop gs
    pop fs
    pop es
    pop ds
    popa
    iret

; ─── NMI stub (vector 2) ─────────────────────────────────────────────────────
; Bare handler: the NMI is the way into a guest that has stopped, and the most
; likely reason it has stopped is that this CPU is holding the Big Kernel Lock
; and cannot let go.  Acquiring the lock here would deadlock against exactly
; the state we came to look at, so this stub does not — like tlb_ipi_isr, it
; goes straight to its C handler.  The frame it builds is a registers_t.
global nmi_isr
extern nmi_handler
nmi_isr:
    push dword 0        ; Dummy error code (keeps registers_t layout)
    push dword 2        ; Interrupt number
    pusha
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld
    push esp
    call nmi_handler
    add esp, 4
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8          ; Remove int_no and err_code
    iret

; ─── Common exception stub ───────────────────────────────────────────────────
extern isr_handler
extern bkl_enter
extern bkl_leave
extern tlb_serve_pending

isr_common_stub:
    pusha               ; Push EAX,ECX,EDX,EBX,ESP(original),EBP,ESI,EDI

    ; Push segment registers
    push ds
    push es
    push fs
    push gs

    ; Switch to kernel data segment
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    cld                 ; SysV ABI requires DF=0

    call bkl_enter      ; SMP: acquire (or nest) the Big Kernel Lock
    call tlb_serve_pending ; SMP backstop: flush a missed TLB-shootdown request

    push esp            ; Argument: pointer to registers_t on the stack
    call isr_handler
    add esp, 4

    call bkl_leave      ; SMP: release (or un-nest) the Big Kernel Lock

    ; Restore segments
    pop gs
    pop fs
    pop es
    pop ds

    popa
    add esp, 8          ; Remove int_no and err_code
    iret

; ─── Common IRQ stub ─────────────────────────────────────────────────────────
extern irq_handler

irq_common_stub:
    pusha
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    cld

    call bkl_enter      ; SMP: acquire (or nest) the Big Kernel Lock
    call tlb_serve_pending ; SMP backstop: flush a missed TLB-shootdown request

    push esp
    call irq_handler
    add esp, 4

    call bkl_leave      ; SMP: release (or un-nest) the Big Kernel Lock

    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8
    iret

; ─── trapret ─────────────────────────────────────────────────────────────────
; Used by newly created processes: their kernel stack is pre-filled with a
; trapframe, and swtch() "returns" through forkret → trapret → iret into user
; mode (or the first kernel-thread function).
global trapret
trapret:
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8          ; Remove int_no + err_code from the pre-built trapframe
    iret
