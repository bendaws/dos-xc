;
; DOS-XC /os/runtime/switch64.asm
; Copyright (c) 2026 Ben Daws - see LICENSE.txt
;

bits 64
default rel
section .text

global runtime_enter, runtime_leave, runtime_spurious, bios_call, runtime_save, runtime_switch
global runtime_exception_stubs, runtime_syscall_entry
global runtime_timer_gate, runtime_on_stack, runtime_enter_frame, runtime_ap_timer, runtime_ap_ipi
global runtime_firmware_stubs
extern runtime_firmware_vectors
extern vm_fault, fault_stack, runtime_exception, runtime_syscall, runtime_fxsr, sched_tick, sched_preempted
extern runtime_ring3, runtime_tss_area, runtime_tss_selector, runtime_user_cs, runtime_user_ss
extern sched_ap_interrupt, smp_kernel_interrupt

; RCX = the RSP0 field of this processor's task state segment (128 bytes
; each, in the order of their descriptors).
%macro TSS_RSP0 0
    str cx
    movzx ecx, cx
    sub ecx, [rel runtime_tss_selector]
    shl ecx, 3
    add rcx, [rel runtime_tss_area]
%endmacro

; Null FS and GS selectors, keeping both bases (the thread pointer and the
; running thread's data).  Clobbers rax, rcx, rdx.
%macro NULL_SEGMENTS 0
    mov ecx, 0xc0000101
    rdmsr
    push rax
    push rdx
    mov ecx, 0xc0000100
    rdmsr
    push rax
    push rdx
    xor eax, eax
    mov fs, ax
    mov gs, ax
    pop rdx
    pop rax
    wrmsr
    pop rdx
    pop rax
    mov ecx, 0xc0000101
    wrmsr
%endmacro

; Before an IRETQ with RSP at its frame: when it returns to ring 3, the
; next entry from ring 3 must land just above this frame (RSP0), and FS/GS
; must hold null selectors (IRETQ to ring 3 would replace a ring 0 selector
; with null and lose the FS and GS bases).
%macro USER_RETURN 0
    test byte [rsp+8], 3
    jz %%done
    push rax
    push rcx
    push rdx
    lea rax, [rsp+24+40]
    TSS_RSP0
    mov [rcx], rax
    mov ax, fs
    mov cx, gs
    or ax, cx
    jz %%segments
    NULL_SEGMENTS
%%segments:
    pop rdx
    pop rcx
    pop rax
%%done:
%endmacro

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
    cmp dword [rel runtime_ring3], 0
    jne .ring3
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
.ring3:
    lea rax, [rsp-64]
    and rax, -16
    TSS_RSP0
    mov [rcx], rax
    mov ax, fs
    mov cx, gs
    or ax, cx
    jz .null_segments
    NULL_SEGMENTS
.null_segments:
    movzx eax, word [rel runtime_user_ss]
    push rax
    push rsi
    pushfq
    and qword [rsp], ~0x3000    ; IOPL 0
    movzx eax, word [rel runtime_user_cs]
    push rax
    push rdi
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    iretq

; int runtime_enter_frame(const uptr *frame, struct saved_context *save, const void *fx)
; Like runtime_enter, but continues a forked program where its system call
; returns: FRAME holds struct syscall_frame and the IRETQ frame (20 words), FX
; its fxsave (or fnsave) image.
runtime_enter_frame:
    mov [rsi+0], rbx
    mov [rsi+8], rbp
    mov [rsi+16], r12
    mov [rsi+24], r13
    mov [rsi+32], r14
    mov [rsi+40], r15
    lea rax, [rsp+8]
    mov [rsi+48], rax
    mov rax, [rsp]
    mov [rsi+56], rax
    pushfq
    pop qword [rsi+64]
    fnstcw [rsi+72]
    mov eax, [rel runtime_fxsr]
    mov [rsi+80], eax
    test eax, eax
    jz .x87
    stmxcsr [rsi+76]
    fxrstor [rdx]
    jmp .copy
.x87:
    frstor [rdx]
.copy:
    lea rax, [rsp-256]
    and rax, -16
    mov ecx, 20
.word:
    mov r8, [rdi+rcx*8-8]
    mov [rax+rcx*8-8], r8
    loop .word
    mov rsp, rax
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
    USER_RETURN
    iretq

; uptr runtime_on_stack(uptr (*fn)(uptr), uptr argument, void *top)
; Calls FN(ARGUMENT) on the stack ending at TOP, then returns on this one.
runtime_on_stack:
    push rbp
    mov rbp, rsp
    and rdx, -16
    mov rsp, rdx
    mov rax, rdi
    mov rdi, rsi
    call rax
    mov rsp, rbp
    pop rbp
    ret

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
    USER_RETURN
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

; A page fault may only be a mapped page being touched for the first time.
; Then the program carries on, so everything it could be using is preserved.
page_fault_entry:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    mov rdi, cr2
    mov rsi, [rsp+72]
    push rbp
    mov rbp, rsp
    and rsp, -16
    cld
    call vm_fault
    leave
    test eax, eax
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    jz .fatal
    add rsp, 8
    USER_RETURN
    iretq
.fatal:
    ; Ending the program can switch tasks, so leave the shared fault stack.
    ; A stack that is itself a mapping might be what faulted, so that case
    ; stays here.  A handler may resume the program: keep every register.
    mov [rel fault_saved], rax
    mov [rel fault_saved+8], rcx
    mov [rel fault_saved+16], rdx
    lea rax, [rel fault_stack]
    mov rcx, rsp
    sub rcx, rax
    cmp rcx, 16384
    jae .deliver                ; never switched stacks (UEFI)
    mov rax, [rsp+32]           ; the interrupted stack pointer
    mov rcx, 0x100000000000
    cmp rax, rcx
    jae .deliver
    sub rax, 128+48
    and rax, -16
    mov rcx, 6
.copy:
    mov rdx, [rsp+rcx*8-8]
    mov [rax+rcx*8-8], rdx
    loop .copy
    mov rsp, rax
.deliver:
    mov rax, [rel fault_saved]
    mov rcx, [rel fault_saved+8]
    mov rdx, [rel fault_saved+16]
    jmp stub_14

; Frame: vector, error, rip, cs, rflags, rsp, ss, with the registers below it
; in struct syscall_frame's order.  runtime_exception returns only when it has
; arranged for the program's fault handler to run.
exception_common:
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
    lea rdi, [rsp+15*8]
    mov rsi, rsp
    and rsp, -16
    call runtime_exception
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
    add rsp, 16                 ; vector and error code
    USER_RETURN
    iretq

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
    test byte [rsp+8], 3
    jnz timer_from_ring3
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

; The timer interrupted ring 3 code: already on the task's kernel stack, so
; switch tasks right here if its time is up.
timer_from_ring3:
    push rax
    mov al, 0x20
    out 0x20, al
    pop rax
; Also where a UEFI firmware interrupt of ring 3 continues, its handler done.
tick_from_ring3:
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
    mov rdi, [rsp+15*8]         ; rip
    and rsp, -16
    call sched_tick
    test eax, eax
    jz .return
    sub rsp, 512
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
    jmp .return
.frstor:
    frstor [rsp]
.return:
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
    USER_RETURN
    iretq

; UEFI: the firmware keeps its interrupt handlers (its timer drives its
; console, disk and pointer drivers).  Each wrapped vector's stub runs the
; firmware's handler as if the interrupt had gone straight there, on a
; frame of its own that returns here; an interrupt of ring 3 then goes on to
; tick_from_ring3, which may switch tasks.
%assign vector 32
%rep 224
firmware_stub_%+vector:
    push qword vector
    jmp firmware_interrupt
%assign vector vector+1
%endrep

firmware_interrupt:
    push rax
    push rdx
    movzx eax, byte [rsp+16]
    lea rdx, [rel runtime_firmware_vectors]
    mov rax, [rdx+rax*8]
    mov [rsp+16], rax           ; the vector's slot now holds its handler
    mov rdx, rsp
    and rsp, -16
    mov eax, ss
    push rax
    push rdx
    pushfq
    mov eax, cs
    push rax
    lea rax, [rel .back]
    push rax
    jmp [rdx+16]
.back:
    pop rdx
    pop rax
    add rsp, 8
    test byte [rsp+8], 3
    jnz tick_from_ring3
    iretq

; The other processors' LAPIC timer and wake-up interrupts.  From ring 3
; (on the task's kernel stack) the task may move or stop, as with
; timer_from_ring3; from the kernel (an idle processor) they only acknowledge.
runtime_ap_timer:
runtime_ap_ipi:
    test byte [rsp+8], 3
    jnz .user
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push rbp
    mov rbp, rsp
    and rsp, -16
    cld
    call smp_kernel_interrupt
    leave
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
.user:
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
    and rsp, -16
    sub rsp, 512
    fxsave [rsp]
    call sched_ap_interrupt
    fxrstor [rsp]
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
    USER_RETURN
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
fault_saved: dq 0, 0, 0
runtime_exception_stubs:
%assign v 0
%rep 32
%if v == 14
    dq page_fault_entry
%else
    dq stub_ %+ v
%endif
%assign v v+1
%endrep

; The real-mode USB bridge (os/dos/bridge.asm, copied to 0x1000 by
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

; Other processors start here in real mode at 0x8000 (the startup IPI's
; page; smp.c copies this there and fills in the fields at the end), and
; reach smp_ap_entry(index) in long mode on their own stack.
AP_BASE equ 0x8000
global ap_trampoline, ap_trampoline_end, ap_cr3, ap_efer, ap_stack, ap_entry, ap_index
section .rodata
align 16
ap_trampoline:
bits 16
    cli
    cld
    mov ax, cs
    mov ds, ax
    o32 lgdt [ap_gdtr - ap_trampoline]
    mov eax, cr0
    and eax, 0x9fffffff         ; caches on
    or al, 1
    mov cr0, eax
    jmp dword 0x08:(AP_BASE + ap_protected - ap_trampoline)
bits 32
ap_protected:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, cr4
    or eax, 0x20                ; PAE
    mov cr4, eax
    mov eax, [AP_BASE + ap_cr3 - ap_trampoline]
    mov cr3, eax
    mov ecx, 0xc0000080
    mov eax, [AP_BASE + ap_efer - ap_trampoline]
    mov edx, [AP_BASE + ap_efer - ap_trampoline + 4]
    wrmsr
    mov eax, cr0
    or eax, 0x80000001
    mov cr0, eax
    jmp 0x18:(AP_BASE + ap_long - ap_trampoline)
bits 64
ap_long:
    mov rsp, [abs AP_BASE + ap_stack - ap_trampoline]
    mov edi, [abs AP_BASE + ap_index - ap_trampoline]
    mov rax, [abs AP_BASE + ap_entry - ap_trampoline]
    call rax
.halt:
    hlt
    jmp .halt
align 8
ap_gdt:
    dq 0
    dq 0x00cf9a000000ffff       ; 32-bit code
    dq 0x00cf92000000ffff       ; data
    dq 0x00af9a000000ffff       ; 64-bit code
ap_gdtr:
    dw 31
    dd AP_BASE + ap_gdt - ap_trampoline
align 8
ap_cr3: dq 0
ap_efer: dq 0
ap_stack: dq 0
ap_entry: dq 0
ap_index: dq 0
ap_trampoline_end:

section .data
align 8
runtime_firmware_stubs:
%assign vector 32
%rep 224
    dq firmware_stub_%+vector
%assign vector vector+1
%endrep
