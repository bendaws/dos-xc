;
; DOS-XC /os/runtime/switch32.asm
; Copyright (c) 2026 Ben Daws - see LICENSE.txt
;

bits 32
section .text

global runtime_enter, runtime_leave, runtime_spurious, bios_call, runtime_save, runtime_switch
global runtime_exception_stubs, runtime_syscall_entry
global runtime_timer_gate, runtime_on_stack
extern runtime_exception, runtime_syscall, runtime_fxsr, sched_tick, sched_preempted

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

; int runtime_save(struct saved_context *save): like setjmp.
runtime_save:
    mov edx, [esp+4]
    call save_state
    xor eax, eax
    ret

; int runtime_switch(struct saved_context *save, struct saved_context *target, int value)
runtime_switch:
    mov edx, [esp+4]
    call save_state
    mov eax, [esp+8]
    mov ecx, [esp+12]
    push ecx
    push eax
    push 0
    jmp runtime_leave

; Store the state of save_state's caller's caller in [edx].
save_state:
    mov [edx+0], ebx
    mov [edx+4], esi
    mov [edx+8], edi
    mov [edx+12], ebp
    lea eax, [esp+8]
    mov [edx+16], eax
    mov eax, [esp+4]
    mov [edx+20], eax
    pushfd
    pop dword [edx+24]
    fnstcw [edx+28]
    mov eax, [runtime_fxsr]
    mov [edx+36], eax
    test eax, eax
    jz .done
    stmxcsr [edx+32]
.done:
    ret

; uptr runtime_on_stack(uptr (*fn)(uptr), uptr argument, void *top)
runtime_on_stack:
    push ebp
    mov ebp, esp
    mov eax, [ebp+8]
    mov ecx, [ebp+12]
    mov edx, [ebp+16]
    and edx, -16
    mov esp, edx
    sub esp, 12
    push ecx
    call eax
    mov esp, ebp
    pop ebp
    ret

bios_call:
    mov eax, 0x20140
    jmp eax

runtime_spurious:
    iretd

; int 0x80 system call entry (ABI 2).  eax = number, arguments in ebx, ecx,
; edx, esi, edi, ebp.  Every register but eax (and edx for 64-bit results),
; and the x87/SSE state, is preserved.  Programs run in ring 0, so the
; frame is on the program stack.
runtime_syscall_entry:
    push ebp
    push edi
    push esi
    push edx
    push ecx
    push ebx
    push eax
    cld
    mov ebx, esp                ; struct syscall_frame
    sub esp, 512
    and esp, -16
    cmp dword [runtime_fxsr], 0
    je .fnsave
    fxsave [esp]
    jmp .saved
.fnsave:
    fnsave [esp]
.saved:
    sub esp, 12
    push ebx
    call runtime_syscall
    add esp, 16
    cmp dword [runtime_fxsr], 0
    je .frstor
    fxrstor [esp]
    jmp .restored
.frstor:
    frstor [esp]
.restored:
    mov esp, ebx
    pop eax
    pop ebx
    pop ecx
    pop edx
    pop esi
    pop edi
    pop ebp
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

; Vector 8 is where the BIOS keeps IRQ 0 and also the double fault; only the
; timer sets its bit in the interrupt controller.
runtime_timer_gate:
    push eax
    mov al, 0x0b
    out 0x20, al
    in al, 0x20
    test al, 1
    pop eax
    jz stub_8
    pushad
    mov al, 0x20
    out 0x20, al
    mov eax, [esp+32]           ; eip
    cld
    mov ebx, esp
    and esp, -16
    sub esp, 12
    push eax
    call sched_tick
    mov esp, ebx
    test eax, eax
    jz .return
    sub esp, 512
    and esp, -16
    cmp dword [runtime_fxsr], 0
    je .fnsave
    fxsave [esp]
    jmp .saved
.fnsave:
    fnsave [esp]
.saved:
    call sched_preempted
    cmp dword [runtime_fxsr], 0
    je .frstor
    fxrstor [esp]
    jmp .restored
.frstor:
    frstor [esp]
.restored:
    mov esp, ebx
.return:
    popad
    iretd

section .data
align 4
runtime_exception_stubs:
%assign v 0
%rep 32
    dd stub_ %+ v
%assign v v+1
%endrep

; The real-mode USB bridge (os/dos/bridge.asm, copied to 0x1000 by
; bridge.c) calls bridge_entry in protected mode with interrupts off.
global bridge_entry, bridge_image, bridge_image_end
extern bridge_service
section .text
bridge_entry:
    mov [bridge_saved], esp
    mov esp, bridge_stack_top
    call bridge_service
    mov esp, [bridge_saved]
    ret
section .rodata
align 16
bridge_image:
    incbin "bridge.bin"
bridge_image_end:
section .bss
align 16
bridge_saved: resd 1
bridge_stack: resb 65536
bridge_stack_top:
