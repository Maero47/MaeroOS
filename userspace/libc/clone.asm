bits 32

; int __clone_thread(int flags, void *child_stack_top, void (*fn)(void *), void *arg);
;
; Thread-create stub: the kernel gives the child the SAME eip with eax=0 and
; esp=child_stack_top, so anything the child needs must already be ON the
; child stack.  We pre-push fn and arg there; the child pops and calls fn,
; then exits with its return value.

global __clone_thread

section .text

__clone_thread:
    push ebx
    push esi

    mov  ebx, [esp+12]      ; flags
    mov  ecx, [esp+16]      ; child stack top
    mov  esi, [esp+20]      ; fn
    mov  edx, [esp+24]      ; arg

    ; Build the child's initial stack: [arg][fn] (fn at the lower address)
    sub  ecx, 8
    mov  [ecx+4], edx       ; arg
    mov  [ecx],   esi       ; fn

    mov  eax, 120           ; sys_clone
    int  0x80

    test eax, eax
    jnz  .parent            ; parent: eax = tid (or negative errno)

    ; ── child: esp = child stack base ([fn][arg]) ──
    pop  eax                ; fn
    call eax                ; fn(arg) — arg is at [esp] per cdecl
    mov  ebx, eax           ; exit status = fn's return value
    mov  eax, 1             ; sys_exit
    int  0x80
.hang:
    jmp  .hang

.parent:
    pop  esi
    pop  ebx
    ret
