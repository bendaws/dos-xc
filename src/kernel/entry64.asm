;
; DOS-XC /kernel/entry64.asm
; Copyright (c) 2026 Ben Daws - see LICENSE.txt
;

bits 64
default rel
global _start
global enter_dos
extern kmain
extern __bss_start, __bss_end
section .entry
_start:
    test rdi, rdi
    jnz .firmware
    cli
.firmware:
    mov r8, rdi
    cld
    mov rdi, __bss_start
    mov rcx, __bss_end
    sub rcx, rdi
    xor eax, eax
    rep stosb
    mov rsp, stack_top
    and rsp, -16
    mov rdi, r8
    call kmain
.halt:
    hlt
    jmp .halt

section .dos_entry
enter_dos:
    cli
    mov edi, 1
    jmp far [rel dos_entry]
dos_entry:
    dq 0x90000
    dw 0x08
section .bss
align 16
stack: resb 32768
stack_top:

section .bios_entry
global bios_transfer
bios_transfer:
    jmp bios_body
section .text
bios_body:
    pushfq
    cli
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    cmp byte [abs 0x70d2], 0          ; multitasking: the BIOS needs its own interrupts
    je .bios_masks
    mov al, [abs 0x70d0]
    out 0x21, al
    mov al, [abs 0x70d1]
    out 0xa1, al
.bios_masks:
    mov [abs 0x7080], rsp
    mov edi, 1
    mov dword [abs 0x70b0], bios_resume
    mov word [abs 0x70b4], 0x18
    mov rax, cr0
    mov [abs 0x7088], eax
    mov rax, cr3
    mov [abs 0x708c], eax
    mov rax, cr4
    mov [abs 0x7090], eax
    sgdt [abs 0x70a0]
    sidt [abs 0x70c0]
    mov byte [abs 0x7040], 1
    jmp far [rel dos_entry]
bios_resume:
    mov rsp, [abs 0x7080]
    cmp byte [abs 0x70d2], 0
    je .resume_masks
    mov al, 0xfe
    out 0x21, al
    mov al, 0xff
    out 0xa1, al
.resume_masks:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    popfq
    ret
