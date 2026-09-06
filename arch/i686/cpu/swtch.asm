; swtch.asm — low-level context switch
;
; void swtch(struct context **old, struct context *new);
;
; Saves callee-saved registers (EBP, EBX, ESI, EDI) plus the implicit EIP
; (via the CALL/RET mechanism) onto the current stack, then stores ESP in
; *old.  Loads ESP from `new`, pops the new context's registers, and RET
; jumps to the new task's saved EIP.
;
; Register allocation on entry:
;   [esp+4] = old  (pointer to where we store the old context ptr)
;   [esp+8] = new  (the new context ptr)
;
; Context layout in memory (low→high, i.e. first-popped at lowest address):
;   edi, esi, ebx, ebp, eip

global swtch
swtch:
    mov eax, [esp+4]    ; eax = old (address of context* to save into)
    mov edx, [esp+8]    ; edx = new (context* to switch to)

    ; Save callee-saved registers + implicit EIP (which is the return addr
    ; already on the stack when CALL swtch was executed).
    push ebp
    push ebx
    push esi
    push edi

    ; Save current stack pointer into *old
    mov [eax], esp

    ; Switch to the new stack and restore the new context
    mov esp, edx

    pop edi
    pop esi
    pop ebx
    pop ebp
    ret                 ; pops EIP — jumps to the new task's saved return address
