;
; DOS-XC /runtime/switch64.asm
; Copyright (c) 2026 Ben Daws - see LICENSE.txt
;
; Program entry/exit, exception and system call entry for the 64-bit runtime.
;

bits 64
default rel
section .text

global runtime_enter, runtime_leave, runtime_spurious, bios_call, runtime_save, runtime_switch
global runtime_exception_stubs, runtime_syscall_entry
global runtime_timer_gate
extern runtime_exception, runtime_syscall, runtime_fxsr, sched_tick, sched_preempted

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

; int runtime_save(struct saved_context *save): like setjmp.  Returns 0; a
; later runtime_leave(save, value) returns value from here again.
runtime_save:
    call save_state
    xor eax, eax
    ret

; int runtime_switch(struct saved_context *save, struct saved_context *target, int value)
; Saves the caller in SAVE, then continues TARGET with VALUE.
runtime_switch:
    call save_state
    mov rdi, rsi
    mov esi, edx
    jmp runtime_leave

; Store the state of save_state's caller's caller (rsp/return address of
; the runtime_save or runtime_switch call) in [rdi].
save_state:
    mov [rdi+0], rbx
    mov [rdi+8], rbp
    mov [rdi+16], r12
    mov [rdi+24], r13
    mov [rdi+32], r14
    mov [rdi+40], r15
    lea rax, [rsp+16]
    mov [rdi+48], rax
    mov rax, [rsp+8]
    mov [rdi+56], rax
    pushfq
    pop qword [rdi+64]
    fnstcw [rdi+72]
    mov eax, [runtime_fxsr]
    mov [rdi+80], eax
    test eax, eax
    jz .done
    stmxcsr [rdi+76]
.done:
    ret

; BIOS services through the kernel's real-mode bridge (kernel base + 320).
bios_call:
    mov eax, 0x20140
    jmp rax

runtime_spurious:
    iretq

; int 0x80 system call entry (ABI 2).  rax = number, arguments in rdi, rsi,
; rdx, r10, r8, r9.  Every register but rax, and the x87/SSE state, is
; preserved.  Programs run in ring 0, so the frame is on the program stack.
runtime_syscall_entry:
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax
    cld
    mov rbx, rsp                ; struct syscall_frame
    sub rsp, 512
    and rsp, -16
    cmp dword [runtime_fxsr], 0
    je .fnsave
    fxsave [rsp]
    jmp .saved
.fnsave:
    fnsave [rsp]
.saved:
    mov rdi, rbx
    call runtime_syscall
    cmp dword [runtime_fxsr], 0
    je .frstor
    fxrstor [rsp]
    jmp .restored
.frstor:
    frstor [rsp]
.restored:
    mov rsp, rbx
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
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

; Vector 8 is where the BIOS keeps IRQ 0 and also the double fault; only the
; timer sets its bit in the interrupt controller.  This gate runs on its own
; stack (programs use the red zone), so a switch is done by making the
; program itself continue at runtime_preempt_entry.
runtime_timer_gate:
    push rax
    mov al, 0x0b
    out 0x20, al
    in al, 0x20
    test al, 1
    pop rax
    jz stub_8
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    mov al, 0x20
    out 0x20, al
    mov rdi, [rsp+72]           ; rip
    cld
    call sched_tick
    test eax, eax
    jz .return
    mov rax, [rsp+72+24]        ; rsp, past the red zone
    sub rax, 128+16
    mov rcx, [rsp+72+16]        ; rflags
    mov [rax], rcx
    mov rcx, [rsp+72]           ; rip
    mov [rax+8], rcx
    mov [rsp+72+24], rax
    lea rcx, [rel runtime_preempt_entry]
    mov [rsp+72], rcx
    and qword [rsp+72+16], ~0x200
.return:
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    iretq

runtime_preempt_entry:
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax
    cld
    mov rbx, rsp
    sub rsp, 512
    and rsp, -16
    cmp dword [runtime_fxsr], 0
    je .fnsave
    fxsave [rsp]
    jmp .saved
.fnsave:
    fnsave [rsp]
.saved:
    call sched_preempted
    cmp dword [runtime_fxsr], 0
    je .frstor
    fxrstor [rsp]
    jmp .restored
.frstor:
    frstor [rsp]
.restored:
    mov rsp, rbx
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
    popfq
    ret 128

section .data
align 8
runtime_exception_stubs:
%assign v 0
%rep 32
    dq stub_ %+ v
%assign v v+1
%endrep

; The real-mode USB bridge (src/dos/bridge.asm, copied to 0x1000 by
; bridge.c) calls bridge_entry in long mode with interrupts off.
global bridge_entry, bridge_image, bridge_image_end
extern bridge_service
section .text
bridge_entry:
    mov [rel bridge_saved], rsp
    lea rsp, [rel bridge_stack_top]
    call bridge_service
    mov rsp, [rel bridge_saved]
    ret
section .rodata
align 16
bridge_image:
    incbin "bridge.bin"
bridge_image_end:
section .bss
align 16
bridge_saved: resq 1
bridge_stack: resb 65536
bridge_stack_top:
