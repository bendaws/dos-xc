;
; DOS-XC /kernel/entry32.asm
; Copyright (c) 2026 Ben Daws - see LICENSE.txt
;

bits 32
global _start
global enter_dos
extern kmain
extern __bss_start, __bss_end
section .entry
_start:
    cli
    mov esp, 0x90000
    cld
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb
    push dword 0
    call kmain
.halt:
    hlt
    jmp .halt

section .dos_entry
enter_dos:
    cli
    xor edi, edi
    jmp 0x08:0x90000

section .bios_entry
global bios_transfer
bios_transfer:
    jmp bios_body
section .text
bios_body:
    pushfd
    cli
    pushad
    cmp byte [0x70d2], 0          ; multitasking: the BIOS needs its own interrupts
    je .bios_masks
    mov al, [0x70d0]
    out 0x21, al
    mov al, [0x70d1]
    out 0xa1, al
.bios_masks:
    mov [0x7080], esp
    mov edi, 0
    mov dword [0x70b0], bios_resume
    mov word [0x70b4], 8
    mov eax, cr0
    mov [0x7088], eax
    mov eax, cr3
    mov [0x708c], eax
    mov eax, cr4
    mov [0x7090], eax
    sgdt [0x70a0]
    sidt [0x70c0]
    mov byte [0x7040], 1
    jmp 8:0x90000
bios_resume:
    mov esp, [0x7080]
    cmp byte [0x70d2], 0
    je .resume_masks
    mov al, 0xfe
    out 0x21, al
    mov al, 0xff
    out 0xa1, al
.resume_masks:
    popad
    popfd
    ret
