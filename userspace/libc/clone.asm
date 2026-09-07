bits 32

; int __clone_thread(int flags, void *child_stack_top, void (*fn)(void *),
;                    void *arg, int *tidptr);
;
; Thread-create stub: the kernel gives the child the SAME eip with eax=0 and
; esp=child_stack_top, so anything the child needs must already be ON the
; child stack.  We pre-push fn and arg there; the child pops and calls fn,
; then exits with sys_exit (which ends only this thread of the group).
; tidptr is passed as both the CLONE_PARENT_SETTID and CLONE_CHILD_CLEARTID
; address (i386 clone: flags=ebx, stack=ecx, ptid=edx, tls=esi, ctid=edi).

global __clone_thread

section .text

__clone_thread:
    push ebx
    push esi
    push edi

    mov  ebx, [esp+16]      ; flags
    mov  ecx, [esp+20]      ; child stack top
    mov  esi, [esp+24]      ; fn
    mov  edx, [esp+28]      ; arg
    mov  edi, [esp+32]      ; tidptr (ctid)

    ; Build the child's initial stack: [arg][fn] (fn at the lower address)
    sub  ecx, 8
    mov  [ecx+4], edx       ; arg
    mov  [ecx],   esi       ; fn

    mov  edx, edi           ; ptid = tidptr
    xor  esi, esi           ; no CLONE_SETTLS descriptor
    mov  eax, 120           ; sys_clone
    int  0x80

    test eax, eax
    jnz  .parent            ; parent: eax = tid (or negative errno)

    ; ── child: esp = child stack base ([fn][arg]) ──
    pop  eax                ; fn
    call eax                ; fn(arg) — arg is at [esp] per cdecl
    xor  ebx, ebx           ; thread exit status (return value lives in the descriptor)
    mov  eax, 1             ; sys_exit: this thread only
    int  0x80
.hang:
    jmp  .hang

.parent:
    pop  edi
    pop  esi
    pop  ebx
    ret
