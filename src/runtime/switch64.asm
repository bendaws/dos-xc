;
; DOS-XC /runtime/switch64.asm
; Copyright (c) 2026 Ben Daws - see LICENSE.txt
;
; Program entry/exit and exception entry for the 64-bit runtime.
;

bits 64
default rel
section .text

global runtime_enter, runtime_leave, runtime_spurious, bios_call
global runtime_exception_stubs
extern runtime_exception

; int runtime_enter(uptr entry, uptr stack, struct saved_context *save, int sse)
; Saves the caller's callee-saved state, switches to the program stack and
; jumps to its entry point.  Returns the value later given to runtime_leave.
runtime_enter:
    mov [rdx+0], rbx
    mov [rdx+8], rbp
    mov [rdx+16], r12
    mov [rdx+24], r13
    mov [rdx+32], r14
    mov [rdx+40], r15
    lea rax, [rsp+8]
    mov [rdx+48], rax
    mov rax, [rsp]
    mov [rdx+56], rax
    pushfq
    pop qword [rdx+64]
    fnstcw [rdx+72]
    mov [rdx+80], ecx
    test ecx, ecx
    jz .no_sse
    stmxcsr [rdx+76]
    mov dword [rdx+84], 0x1f80
    ldmxcsr [rdx+84]
.no_sse:
    fninit
    mov rsp, rsi
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx                ; no atexit function from the "dynamic linker"
    xor esi, esi
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    jmp rdi

; void runtime_leave(struct saved_context *save, int value)
runtime_leave:
    mov eax, esi
    mov rbx, [rdi+0]
    mov rbp, [rdi+8]
    mov r12, [rdi+16]
    mov r13, [rdi+24]
    mov r14, [rdi+32]
    mov r15, [rdi+40]
    mov rsp, [rdi+48]
    fninit
    fldcw [rdi+72]
    cmp dword [rdi+80], 0
    je .no_sse
    ldmxcsr [rdi+76]
.no_sse:
    push qword [rdi+64]
    popfq
    jmp [rdi+56]

; BIOS services through the kernel's real-mode bridge (kernel base + 320).
bios_call:
    mov eax, 0x20140
    jmp rax

runtime_spurious:
    iretq

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

; Frame: vector, error, rip, cs, rflags, rsp, ss
exception_common:
    cld
    mov rdi, rsp
    and rsp, -16
    call runtime_exception
.halt:
    cli
    hlt
    jmp .halt

section .data
align 8
runtime_exception_stubs:
%assign v 0
%rep 32
    dq stub_ %+ v
%assign v v+1
%endrep
