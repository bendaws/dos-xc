;
; DOS-XC /runtime/switch32.asm
; Copyright (c) 2026 Ben Daws - see LICENSE.txt
;
; Program entry/exit and exception entry for the 32-bit runtime.
;

bits 32
section .text

global runtime_enter, runtime_leave, runtime_spurious, bios_call
global runtime_exception_stubs
extern runtime_exception

; int runtime_enter(uptr entry, uptr stack, struct saved_context *save, int sse)
runtime_enter:
    mov edx, [esp+12]
    mov [edx+0], ebx
    mov [edx+4], esi
    mov [edx+8], edi
    mov [edx+12], ebp
    lea eax, [esp+4]
    mov [edx+16], eax
    mov eax, [esp]
    mov [edx+20], eax
    pushfd
    pop dword [edx+24]
    fnstcw [edx+28]
    mov ecx, [esp+16]
    mov [edx+36], ecx
    test ecx, ecx
    jz .no_sse
    stmxcsr [edx+32]
    mov dword [edx+40], 0x1f80
    ldmxcsr [edx+40]
.no_sse:
    fninit
    mov eax, [esp+4]
    mov esp, [esp+8]
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx                ; no atexit function from the "dynamic linker"
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    jmp eax

; void runtime_leave(struct saved_context *save, int value)
runtime_leave:
    mov edx, [esp+4]
    mov eax, [esp+8]
    mov ebx, [edx+0]
    mov esi, [edx+4]
    mov edi, [edx+8]
    mov ebp, [edx+12]
    mov esp, [edx+16]
    fninit
    fldcw [edx+28]
    cmp dword [edx+36], 0
    je .no_sse
    ldmxcsr [edx+32]
.no_sse:
    push dword [edx+24]
    popfd
    jmp [edx+20]

bios_call:
    mov eax, 0x20140
    jmp eax

runtime_spurious:
    iretd

%macro stub 1
stub_%1:
%if %1 != 8 && %1 != 10 && %1 != 11 && %1 != 12 && %1 != 13 && %1 != 14 && %1 != 17 && %1 != 21 && %1 != 29 && %1 != 30
    push 0
%endif
    push %1
    jmp exception_common
%endmacro
%assign v 0
%rep 32
    stub v
%assign v v+1
%endrep

; Frame: vector, error, eip, cs, eflags
exception_common:
    cld
    mov eax, esp
    and esp, -16
    sub esp, 12
    push eax
    call runtime_exception
.halt:
    cli
    hlt
    jmp .halt

section .data
align 4
runtime_exception_stubs:
%assign v 0
%rep 32
    dd stub_ %+ v
%assign v v+1
%endrep
