bits 32

global _start
global environ          ; char **environ — accessible from C
extern main
extern exit

section .bss
environ: resd 1         ; char **environ

section .text

_start:
    ; Linux i386 process-entry stack (same as musl/glibc):
    ;   [esp+0]          argc
    ;   [esp+4]          argv[0..argc-1] (inline), NULL
    ;   after argv NULL  envp[0..] (inline), NULL
    ;   after envp NULL  auxv pairs, AT_NULL

    mov  eax, [esp]         ; argc
    lea  ecx, [esp+4]       ; argv = &stack[1]
    lea  edx, [ecx+eax*4+4] ; envp = argv + argc*4 + NULL slot

    mov  [environ], edx     ; store envp pointer in global environ

    push edx                ; arg3: envp
    push ecx                ; arg2: argv
    push eax                ; arg1: argc
    call main
    add  esp, 12

    push eax
    call exit
.hang:
    hlt
    jmp .hang
